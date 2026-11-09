#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace Slic3r::Domain {
class ConfigView;
class Model;
} // namespace Slic3r::Domain

namespace Slic3r {

// The sequential arrange (complete_objects): places the instances on plates and sets the print order, so that the
// extruder and the gantry do not hit a printed instance. The shapes of the extruder and the gantry come from
// geometries.json (see ArrangeHelper.cpp).
//
// The constructor collects the input from the model. Call it in the main thread. process() runs the solver and does
// not use the model. Call it in a worker thread. A copy shares the input.
class SeqArrange
{
public:
    // The new position of an instance: the offset of the instance in X and Y (mm, in bed coordinates).
    struct Placement
    {
        size_t instance_id = 0;
        double x = 0.;
        double y = 0.;
    };
    // The instances of one plate, in print order.
    using Plate = std::vector<Placement>;

    // Collects the printable instances of the model with an id in instance_ids, in model order. The instances of
    // one object stay together on one plate: the G-code prints them one after the other. An instance with a
    // footprint (with its brim) larger than the bed is not arranged (see not_arranged()).
    // config: the print config of the bed (bed shape, printer model, brim, raft).
    SeqArrange(const Domain::Model& model, const Domain::ConfigView& config, const std::vector<size_t>& instance_ids);
    ~SeqArrange();

    // True if there is no instance to arrange.
    bool empty() const;
    // The ids of the instances that are too large for the bed.
    const std::vector<size_t>& not_arranged() const { return m_not_arranged; }

    // Arranges. First the greedy arrange (milliseconds). If it needs more than one plate, the z3 solver tries to find
    // fewer plates (some seconds). The result has the fewest plates, then the most instances on the first plate.
    // progress gets 0 to 100. To stop, throw an exception from progress: process() passes it on.
    std::vector<Plate> process(const std::function<void(int)>& progress) const;

private:
    struct Data;
    std::shared_ptr<const Data> m_data;
    std::vector<size_t> m_not_arranged;
};

} // namespace Slic3r
