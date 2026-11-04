// Fuzz Wavefront OBJ import: the production path load_obj(path) (file-based objparse + face
// assembly) and the stream overload objparse(std::istream&), which shares obj_parseline but
// has its own buffering loop.

#include "Slic3r/Biz/Format/OBJ.hpp"
#include "Slic3r/Domain/TriangleMesh.hpp"
#include "../src/Slic3r/Biz/Format/objparser.hpp"

#include <boost/filesystem.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace fs = boost::filesystem;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const std::string path = (fs::temp_directory_path() / fs::unique_path("fuzz-obj-%%%%-%%%%-%%%%.obj")).string();
    // Registered after path is constructed, so it runs before path is destroyed.
    static const bool cleanup = (std::atexit([] { std::remove(path.c_str()); }), true);
    (void) cleanup;
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
        (void) Slic3r::Biz::load_obj(path);
    }

    std::istringstream     stream(std::string(reinterpret_cast<const char *>(data), size));
    ObjParser::ObjData     obj;
    (void) ObjParser::objparse(stream, obj);
    return 0;
}
