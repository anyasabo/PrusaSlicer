#ifndef libslic3r_Arrange_Helper_hpp
#define libslic3r_Arrange_Helper_hpp

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ConfigViews.hpp"

namespace Slic3r::Domain {
class Model;
} // namespace Slic3r::Domain

namespace Slic3r {

// Checks whether the printable instances in the model can be printed sequentially
// ("complete_objects") without the extruder or gantry hitting an already printed instance.
// The model must contain only the instances of a single bed, in bed coordinates, as Print
// receives it. Instances are printed in their model order.
// Returns the names of the conflicting objects: the first one is hit when printing the second.
std::optional<std::pair<std::string, std::string>> check_seq_conflict(const Domain::Model& model, const PrintConfigView& config);

// Finds a print order of the printable instances at their positions in which the extruder and the gantry hit
// no printed instance. The order is as near to the model order as possible. Returns the names of the
// instances in that order (an object with more instances gets " (n)" after its name), or nullopt if no order
// works.
std::optional<std::vector<std::string>> find_seq_print_order(const Domain::Model& model, const PrintConfigView& config);

} // namespace Slic3r

#endif // libslic3r_Arrange_Helper_hpp
