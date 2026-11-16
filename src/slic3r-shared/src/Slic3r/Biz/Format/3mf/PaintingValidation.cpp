///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "Slic3r/Biz/Format/3mf/PaintingValidation.hpp"

#include <cstddef>

namespace Slic3r::format_3MF {

bool is_valid_triangle_paint_string(std::string_view str)
{
    if (str.empty())
        return true;

    // set_triangle_from_string() stores the last character first, so the decoder
    // (TriangleSelector::deserialize(), has_facets(), decode_leaf_state()) reads the nibbles in
    // reverse string order.
    std::size_t pos = str.size();
    bool ok = true;
    const auto next_nibble = [&str, &pos, &ok]() -> int {
        if (pos == 0) {
            ok = false; // Truncated.
            return 0;
        }
        const char ch = str[--pos];
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        ok = false; // Not an upper-case hex digit.
        return 0;
    };

    // The tree is stored depth-first, each node as a code nibble followed by its children (split
    // node) or by optional extra state nibbles (leaf). Counting the nodes still to be read is
    // enough to check that the stream holds exactly one complete tree.
    std::size_t pending_nodes = 1;
    while (ok && pending_nodes > 0) {
        --pending_nodes;
        const int code = next_nibble();
        if (!ok)
            break;
        if (const int split_sides = code & 0b11; split_sides != 0) {
            // A triangle has sides 0..2, and perform_split() indexes verts_idxs[special_side]. For a
            // split of all three sides, the writer stores side 0, and the decoder requires it
            // (Triangle::set_division()).
            const int special_side = code >> 2;
            if (special_side > 2 || (split_sides == 3 && special_side != 0))
                return false;
            pending_nodes += std::size_t(split_sides) + 1;
        } else if ((code & 0b1100) == 0b1100) {
            // Extended leaf state, see decode_leaf_state().
            if (next_nibble() == 0b1110) {
                const int lo = next_nibble();
                const int hi = next_nibble();
                if (ok && (lo | (hi << 4)) + 17 > 255)
                    return false; // Does not fit TriangleStateType.
            }
        }
    }
    return ok && pos == 0;
}

} // namespace Slic3r::format_3MF
