#include "Slic3r/Biz/Format/STL.hpp"
#include "Slic3r/Domain/TriangleMesh.hpp"
#include "Slic3r/Log.hpp"
#include "Slic3r/TestUtils/TestTempDir.hpp"

#include <boost/nowide/fstream.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace Slic3r;

TEST_CASE("STL whose triangles the repair removes completely is reported as empty", "[stl]")
{
    // Two triangles without neighbors: the repair removes both as unconnected facets. The volume
    // calculation after the repair then read facet 0 of the empty vector. The read stays in the
    // old allocation of the vector, so only _GLIBCXX_ASSERTIONS (or libc++ hardening) sees it.
    const ::Tests::TestTempDir temp_dir;
    const boost::filesystem::path file_path = temp_dir.path() / "isolated_triangles.stl";
    {
        boost::nowide::ofstream file(file_path.string());
        file << "solid a\n"
                "  facet normal 0 0 1\n"
                "    outer loop\n"
                "      vertex 0 0 0\n"
                "      vertex 10 0 0\n"
                "      vertex 0 10 0\n"
                "    endloop\n"
                "  endfacet\n"
                "  facet normal 0 0 1\n"
                "    outer loop\n"
                "      vertex 50 0 0\n"
                "      vertex 60 0 0\n"
                "      vertex 50 10 0\n"
                "    endloop\n"
                "  endfacet\n"
                "endsolid a\n";
    }

    const auto loaded = Biz::load_stl(file_path.string());

    REQUIRE_FALSE(loaded.has_value());
    CHECK_THAT(loaded.error(), Catch::Matchers::ContainsSubstring("empty"));
}
