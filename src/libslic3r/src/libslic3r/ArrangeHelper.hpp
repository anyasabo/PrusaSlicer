#ifndef libslic3r_Arrange_Helper_hpp
#define libslic3r_Arrange_Helper_hpp

#include <optional>
#include <string>
#include <utility>

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

} // namespace Slic3r

#endif // libslic3r_Arrange_Helper_hpp
