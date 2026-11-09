#ifndef libslic3r_Arrange_Helper_hpp
#define libslic3r_Arrange_Helper_hpp

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ConfigViews.hpp"
#include "Slic3r/Domain/Polygon.hpp"

namespace Slic3r::Domain {
class Model;
} // namespace Slic3r::Domain

namespace Slic3r {

// The raft of an instance: the print of the instance is lifted by z_lift (mm), and the raft reaches xy_expansion (mm)
// beyond the instance. Print fills it from its PrintObjects. The key is the id of the ModelInstance.
struct SeqRaft
{
	double z_lift = 0.;
	double xy_expansion = 0.;
};
using SeqRafts = std::map<size_t, SeqRaft>;

// The support layers of an instance above the raft: for each layer, its print_z (mm) and the convex hull of its support
// extrusions, relative to the offset of the instance. Supports can reach some mm outside the object (measured: up to
// 5 mm at the bed and 2 mm above it, grid style). Print fills it from its PrintObjects. The key is the id of the
// ModelInstance.
using SeqSupportLayers = std::vector<std::pair<double, Domain::Polygon>>;
using SeqSupports = std::map<size_t, SeqSupportLayers>;

// Checks whether the printable instances in the model can be printed sequentially
// ("complete_objects") without the extruder or gantry hitting an already printed instance.
// The model must contain only the instances of a single bed, in bed coordinates, as Print
// receives it. Instances are printed in their model order.
// Returns the names of the conflicting instances: the first one is hit when printing the second. An object with
// more instances gets " (n)" after its name.
std::optional<std::pair<std::string, std::string>> check_seq_conflict(const Domain::Model& model, const PrintConfigView& config, const SeqRafts& rafts = {}, const SeqSupports& supports = {});

// Finds a print order of the printable instances at their positions in which the extruder and the gantry hit
// no printed instance. The order is as near to the model order as possible. Returns the names of the
// instances in that order (an object with more instances gets " (n)" after its name), or nullopt if no order
// works.
std::optional<std::vector<std::string>> find_seq_print_order(const Domain::Model& model, const PrintConfigView& config, const SeqRafts& rafts = {}, const SeqSupports& supports = {});

// The height (mm) above the nozzle tip of the lowest slice of the extruder and gantry shape, other than the
// slice at the nozzle. In a sequential print, the skirt prints before all objects. A skirt higher than this
// is hit while the objects print. Returns 0 for a printer without data in geometries.json: the clearance
// fallback does not know the real toolhead.
double seq_skirt_clearance(const PrintConfigView& config);

} // namespace Slic3r

#endif // libslic3r_Arrange_Helper_hpp
