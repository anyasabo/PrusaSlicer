// Fuzz project (.3mf) loading end to end, the way FileLoadingLogic::load_from_project does it:
// the current loader, falling back to the legacy (PrusaSlicer <= 2.x) loader, then the
// paint-on facet queries the GUI makes for every painted volume.
//
// 3MF is a zip. Mutating raw zip bytes mostly breaks CRCs and deflate streams and never reaches
// the XML/JSON parsers, so the input is a structure-aware container, zipped (stored, no
// compression) by the harness:
//
//     --- [Content_Types].xml
//     <?xml ...?>...
//     --- 3D/3dmodel.model
//     <model ...>...
//
// i.e. "--- <archive path>\n" starts an entry; everything up to the next such line is its body.
// Inputs that start with "PK" are written unchanged, so real .3mf files can be corpus seeds and
// the zip reader is exercised too. src/slic3r-shared/fuzz/tools/3mf_to_corpus.py converts .3mf -> container.

#include "Slic3r/Biz/Algorithms/FacetsAnnotation.hpp"
#include "Slic3r/Biz/Config/3mf_legacy.hpp"
#include "Slic3r/Biz/Config/ConfigLegacy.hpp"
#include "Slic3r/Biz/Format/3mf.hpp"
#include "Slic3r/Biz/Format/ResultLoad3mf.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/ModelObject.hpp"
#include "Slic3r/Domain/ModelVolume.hpp"

#include <miniz.h>

#include <boost/filesystem.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Build a stored zip from the "--- name" container. Returns false if there are no entries.
bool container_to_zip(std::string_view in, std::string &zip_out)
{
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, in.size() + 4096))
        return false;

    constexpr std::string_view marker = "--- ";
    size_t pos = in.starts_with(marker) ? 0 : in.find("\n--- ");
    if (pos == std::string_view::npos) {
        mz_zip_writer_end(&zip);
        return false;
    }
    if (in[pos] == '\n')
        ++pos;

    size_t entries = 0;
    while (pos != std::string_view::npos && pos < in.size()) {
        size_t name_begin = pos + marker.size();
        size_t name_end   = in.find('\n', name_begin);
        if (name_end == std::string_view::npos)
            break;
        std::string name(in.substr(name_begin, name_end - name_begin));
        size_t      body_begin = name_end + 1;
        size_t      next       = in.find("\n--- ", body_begin);
        size_t      body_end   = next == std::string_view::npos ? in.size() : next;
        // miniz rejects empty and absolute names; skip rather than abort the whole input.
        if (!name.empty() && name.size() < 256)
            if (mz_zip_writer_add_mem(&zip, name.c_str(), in.data() + body_begin, body_end - body_begin, MZ_NO_COMPRESSION))
                ++entries;
        pos = next == std::string_view::npos ? next : next + 1;
    }

    void  *buf = nullptr;
    size_t len = 0;
    bool   ok  = entries > 0 && mz_zip_writer_finalize_heap_archive(&zip, &buf, &len);
    if (ok)
        zip_out.assign(static_cast<const char *>(buf), len);
    mz_free(buf); // finalize_heap_archive hands the buffer to the caller; writer_end does not free it
    mz_zip_writer_end(&zip);
    return ok;
}

void query_painting(const Domain::Model &model)
{
    namespace FA = Biz::Algorithms::FacetsAnnotation;
    using Domain::TriangleSelector::TriangleStateType;
    for (const Domain::ModelObject *object : model.objects)
        for (const Domain::ModelVolume *volume : object->volumes)
            for (const Domain::FacetsAnnotation *facets :
                 {&volume->supported_facets, &volume->seam_facets, &volume->mm_segmentation_facets, &volume->fuzzy_skin_facets})
                if (!facets->empty()) {
                    FA::has_facets(*facets, TriangleStateType::ENFORCER);
                    FA::get_facets(*facets, *volume, TriangleStateType::ENFORCER);
                }
}

const std::string &temp_path()
{
    static const std::string path = (fs::temp_directory_path() / fs::unique_path("fuzz-3mf-%%%%-%%%%-%%%%.3mf")).string();
    // Registered after path is constructed, so it runs before path is destroyed.
    static const bool cleanup = (std::atexit([] { std::remove(path.c_str()); }), true);
    (void) cleanup;
    return path;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const std::string_view in(reinterpret_cast<const char *>(data), size);
    std::string            zip;
    if (in.starts_with("PK"))
        zip.assign(in);
    else if (!container_to_zip(in, zip))
        return 0;

    const std::string &path = temp_path();
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(zip.data(), 1, zip.size(), f);
        std::fclose(f);
    } else {
        return 0;
    }

    try {
        Loaded3MF loaded = load_3mf(path);
        query_painting(loaded.model);
    } catch (const Loaded3MFException &e) {
        if (e.issue.type != Read3mfIssueType::legacy_loader_required)
            return 0; // Rejected as invalid: the expected outcome for most inputs.

        // Same call as FileLoadingLogic.cpp load_legacy_project().
        Loaded3MF loaded;
        loaded.config_containers_data.emplace_back();
        auto                       &cc = loaded.config_containers_data.back();
        Biz::LegacyPresetMetadata   hw_printer;
        std::string                 error;
        if (Slic3rLegacy::load_3mf_legacy(path.c_str(), cc.config_pack, hw_printer, &loaded.model, false, loaded.version,
                                          cc.wipe_towers, cc.custom_gcodes, cc.virtual_extruders_config, &error))
            query_painting(loaded.model);
    }
    // Any other exception escapes and is reported: the application only expects
    // Loaded3MFException from load_from_project().
    return 0;
}
