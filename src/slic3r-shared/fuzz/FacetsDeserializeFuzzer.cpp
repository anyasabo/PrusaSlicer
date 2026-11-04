// Fuzz the paint-on facet data path: the "facets" array of a 3MF's PrusaSlicer JSON
// (supports / seam / MMU / fuzzy-skin painting) -> FacetsAnnotation -> TriangleSelector.
//
// The 3MF loaders (Biz/Format/3mf/PrusaFile.cpp json_to_facets, and the legacy loader) accept a
// triangle string only if format_3MF::is_valid_triangle_paint_string() accepts it, and the
// current loader also requires ids that are in the mesh and that increase. This harness applies
// the same checks and then gives the data to the decoder, so it fuzzes TriangleSelector with the
// data that a 3MF file can deliver. Load3mfFuzzer covers the checks themselves.
//
// Input format (consumed until exhausted):
//   u8   kind           which FacetsAnnotation of the volume to fill (mod 4)
//   repeated:
//     u8   id_delta      triangle id = previous id + 1 + id_delta
//     u8   len           number of hex digits that follow
//     u8 * len           each byte's low nibble becomes one hex digit

#include "Slic3r/Biz/Algorithms/FacetsAnnotation.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/TriangleMesh.hpp"
#include "Slic3r/Biz/Format/3mf/PaintingValidation.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/ModelVolume.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

using namespace Slic3r;
using Slic3r::Domain::TriangleSelector::TriangleStateType;

namespace {

struct Reader
{
    const uint8_t *p;
    size_t         n;
    bool    empty() const { return n == 0; }
    uint8_t u8()
    {
        if (n == 0)
            return 0;
        --n;
        return *p++;
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    Reader in{data, size};

    // A 12-facet cube: a small mesh keeps each run fast.
    Domain::Model        model;
    Domain::ModelObject *object = model.add_object();
    Domain::ModelVolume *volume = Biz::Algorithms::ModelObject::add_volume(object, Biz::Algorithms::TriangleMesh::make_cube(10., 10., 10.));

    Domain::FacetsAnnotation *const kinds[] = {
        &volume->supported_facets, &volume->seam_facets, &volume->mm_segmentation_facets, &volume->fuzzy_skin_facets};
    Domain::FacetsAnnotation &facets = *kinds[in.u8() % 4];

    static constexpr char hex[]       = "0123456789ABCDEF";
    const int             facets_count = static_cast<int>(volume->mesh().facets_count());
    int                   triangle_id  = -1;
    while (!in.empty()) {
        triangle_id += 1 + in.u8();
        std::string str(in.u8(), '0');
        for (char &c : str)
            c = hex[in.u8() & 0xF];
        if (triangle_id >= facets_count)
            break; // The ids only increase, so all later ids are out of the mesh too.
        if (format_3MF::is_valid_triangle_paint_string(str))
            facets.set_triangle_from_string(triangle_id, str);
    }
    facets.shrink_to_fit();

    // The calls the application makes after loading a painted 3MF.
    namespace FA = Biz::Algorithms::FacetsAnnotation;
    FA::has_facets(facets, TriangleStateType::ENFORCER);
    FA::get_facets(facets, *volume, TriangleStateType::ENFORCER);
    FA::get_facets_strict(facets, *volume, TriangleStateType::BLOCKER);
    FA::get_all_facets_with_colors(facets, *volume);
    return 0;
}
