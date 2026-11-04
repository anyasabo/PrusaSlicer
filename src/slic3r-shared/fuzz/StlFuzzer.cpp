// Fuzz STL import: load_stl(path) -> admesh stl_open (ASCII and binary detection, facet-count
// header handling) -> repair on import (edge hashing, neighbour fixing, hole filling) ->
// shared-vertex generation. load_stl only takes a path, so input goes through one temp file.

#include "Slic3r/Biz/Format/STL.hpp"
#include "Slic3r/Domain/TriangleMesh.hpp"

#include <boost/filesystem.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fs = boost::filesystem;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const std::string path = (fs::temp_directory_path() / fs::unique_path("fuzz-stl-%%%%-%%%%-%%%%.stl")).string();
    // Registered after path is constructed, so it runs before path is destroyed.
    static const bool cleanup = (std::atexit([] { std::remove(path.c_str()); }), true);
    (void) cleanup;
    if (FILE *f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
        (void) Slic3r::Biz::load_stl(path);
    }
    return 0;
}
