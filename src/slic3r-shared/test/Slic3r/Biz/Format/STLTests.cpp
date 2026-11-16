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

TEST_CASE("STL with coordinates out of the range of the repair grid loads", "[stl]")
{
    // The repair puts the vertices of open edges into a grid of int32_t cells. A coordinate this
    // far from the others has no cell in that range, and the float to int conversion was
    // undefined behavior. Only UBSan with halt_on_error=1 sees it.
    const ::Tests::TestTempDir temp_dir;
    const boost::filesystem::path file_path = temp_dir.path() / "huge_coordinates.stl";
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
                "      vertex 3e38 0 0\n"
                "      vertex 0 3e38 0\n"
                "      vertex 0 0 3e38\n"
                "    endloop\n"
                "  endfacet\n"
                "endsolid a\n";
    }

    const auto loaded = Biz::load_stl(file_path.string());

    CHECK((loaded.has_value() || !loaded.error().empty()));
}

TEST_CASE("STL with only degenerate triangles is reported as empty", "[stl]")
{
    // The repair removes all degenerate triangles. The size of the edge hash table was then
    // calculated from 0 * 6 - 1, which wraps around, and the table had 1.6 billion entries
    // (12.9 GB). Run with ASAN_OPTIONS=max_allocation_size_mb=4096 to make that allocation fail.
    const ::Tests::TestTempDir temp_dir;
    const boost::filesystem::path file_path = temp_dir.path() / "degenerate_triangles.stl";
    {
        boost::nowide::ofstream file(file_path.string());
        file << "solid a\n"
                "  facet normal 0 0 1\n"
                "    outer loop\n"
                "      vertex 0 0 0\n"
                "      vertex 0 0 0\n"
                "      vertex 0 10 0\n"
                "    endloop\n"
                "  endfacet\n"
                "  facet normal 0 0 1\n"
                "    outer loop\n"
                "      vertex 5 5 5\n"
                "      vertex 5 6 5\n"
                "      vertex 5 6 5\n"
                "    endloop\n"
                "  endfacet\n"
                "endsolid a\n";
    }

    const auto loaded = Biz::load_stl(file_path.string());

    REQUIRE_FALSE(loaded.has_value());
    CHECK_THAT(loaded.error(), Catch::Matchers::ContainsSubstring("empty"));
}
