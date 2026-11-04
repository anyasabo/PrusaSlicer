// Fuzz the paint-on facet data path: the "facets" array of a 3MF's PrusaSlicer JSON
// (supports / seam / MMU / fuzzy-skin painting) -> FacetsAnnotation -> TriangleSelector.
//
// The 3MF loader (Biz/Format/3mf/PrusaFile.cpp, json_to_facets) passes the JSON "triangle"
// index and "dividing" hex string straight into FacetsAnnotation::set_triangle_from_string.
// TriangleSelector::deserialize / has_facets then index the mesh with that triangle id and
// read the bitstream, guarded only by assert() (compiled out in release builds).
//
// To reach TriangleSelector rather than stopping at set_triangle_from_string's own ASSERTs
// (non-hex characters, unsorted ids; those are covered by the whole-3MF fuzzer), this harness
// emits only well-formed records: hex digits, strictly increasing triangle ids. Everything else
// - the id range, the string length, the bit patterns - is under fuzzer control.
//
// Input format (all little-endian, consumed until exhausted):
//   u8   kind           which FacetsAnnotation of the volume to fill (mod 4)
//   repeated:
//     u8   id_delta      triangle id = previous id + 1 + id_delta
//     u8   len           number of hex digits that follow
//     u8 * len           each byte's low nibble becomes one hex digit

#include "Slic3r/Biz/Algorithms/FacetsAnnotation.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/TriangleMesh.hpp"
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

    // A 12-facet cube: small enough that most ids are out of range, which is exactly the
    // condition a malicious 3MF controls.
    Domain::Model        model;
    Domain::ModelObject *object = model.add_object();
    Domain::ModelVolume *volume = Biz::Algorithms::ModelObject::add_volume(object, Biz::Algorithms::TriangleMesh::make_cube(10., 10., 10.));

    Domain::FacetsAnnotation *const kinds[] = {
        &volume->supported_facets, &volume->seam_facets, &volume->mm_segmentation_facets, &volume->fuzzy_skin_facets};
    Domain::FacetsAnnotation &facets = *kinds[in.u8() % 4];

    static constexpr char hex[] = "0123456789ABCDEF";
    int                   triangle_id = -1;
    while (!in.empty()) {
        triangle_id += 1 + in.u8();
        std::string str(in.u8(), '0');
        for (char &c : str)
            c = hex[in.u8() & 0xF];
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
