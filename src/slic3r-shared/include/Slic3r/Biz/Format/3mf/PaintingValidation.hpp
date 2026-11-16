///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef slic3r_Format_3mf_PaintingValidation_hpp_
#define slic3r_Format_3mf_PaintingValidation_hpp_

#include <string_view>

namespace Slic3r::format_3MF {

// Checks that `str` is a well-formed paint encoding of one source triangle, as written by
// FacetsAnnotation::get_triangle_as_string(): upper-case hex digits only, and a complete split
// tree with valid split sides, no truncation and no trailing data. An empty string (unpainted
// triangle) is valid.
//
// Paint data comes from untrusted 3MF files, and FacetsAnnotation::set_triangle_from_string()
// and the TriangleSelector decoder trust it, so the loaders call this first.
bool is_valid_triangle_paint_string(std::string_view str);

} // namespace Slic3r::format_3MF

#endif // slic3r_Format_3mf_PaintingValidation_hpp_
