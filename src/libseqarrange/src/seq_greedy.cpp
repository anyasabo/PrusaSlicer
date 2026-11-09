// The greedy sequential arrange: schedule_ObjectsForSequentialPrintGreedy() (see seq_interface.hpp).

#include "libseqarrange/seq_interface.hpp"
#include "seq_defs.hpp"
#include "seq_preprocess.hpp"

#include "Slic3r/Biz/Algorithms/ClipperUtils.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Sequential {

using Slic3r::BoundingBox;
using Slic3r::ExPolygon;
using Slic3r::ExPolygons;
using Slic3r::Point;
using Slic3r::Points;
using Slic3r::Polygon;
using Slic3r::Polygons;

namespace {

// The margin (solver units, 0.1 mm) around the forbidden regions. A point near the edge of a zone can touch the polygon
// after the rounding of the check. With 1 unit, 2 of 89 corpus runs had a conflict. With 3 and 10 units, none.
const int GREEDY_MARGIN = 10;
// The number of orders of the units that are tried (see greedy_arrange_corner()).
const int GREEDY_ORDERS = 4;

// A greedy placement with no-fit regions, without z3 (see seq_interface.hpp).
//
// The objects go on the plate one after the other, in their print order. When an object N is printed after a placed
// object P, the polygon of N must not intersect an unreachable zone U of P (this is the check of
// check_ScheduledObjectsForSequentialConflict(): the zones of an object are where later objects cannot be). Thus the
// reference point of N must be outside U ⊕ (−N), moved to the position of P. All polygons are convex, so each sum is
// the convex hull of the sums of the vertices. The object goes to the corner of the free region that the preference selects. The polygons are in solver units (0.1 mm), as in
// the check.
//
// The order: the units (an object, or the glued instances of one object) by height, the lowest first. A unit that does
// not fit waits for the next plate, and the next units are tried. Four corner preferences are tried, and the result
// with the fewest plates (then the most objects on the first plate) is kept. Four orders of the units are tried with
// each corner preference: 16 variants.

struct GreedyShapes
{
    std::vector<Polygon> polygon;                     // the object polygon, local
    std::vector<std::vector<Polygon>> unreachable;    // the unreachable zones, local
    std::vector<BoundingBox> footprint;               // the box of the exact footprint (slicer units), local
};

GreedyShapes greedy_shapes(const SolverConfiguration& config, const PrinterGeometry& printer, const std::vector<ObjectToPrint>& objects)
{
    GreedyShapes shapes;
    for (const ObjectToPrint& o : objects) {
        std::vector<Polygon> convex_level, box_level;
        std::vector<std::vector<Polygon>> extruder_convex_level, extruder_box_level;
        prepare_ExtruderPolygons(config, printer, o, convex_level, box_level, extruder_convex_level, extruder_box_level, false);
        Polygon polygon;
        std::vector<Polygon> unreachable;
        prepare_ObjectPolygons(config, convex_level, box_level, extruder_convex_level, extruder_box_level, polygon, unreachable);
        shapes.polygon.push_back(polygon);
        shapes.unreachable.push_back(unreachable);
        shapes.footprint.push_back(get_extents(o.pgns_at_height.front().second));
    }
    return shapes;
}

Polygon convex_hull_of(Points points)
{
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y()); });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3)
        return Polygon(points);
    const auto cross = [](const Point& o, const Point& a, const Point& b) {
        return double(a.x() - o.x()) * double(b.y() - o.y()) - double(a.y() - o.y()) * double(b.x() - o.x());
    };
    Points hull(2 * points.size());
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], points[i]) <= 0)
            --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross(hull[k - 2], hull[k - 1], points[i - 1]) <= 0)
            --k;
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);
    return Polygon(hull);
}

// The position (solver units) of object n after the placed objects, or nothing if it does not fit.
std::optional<Point> greedy_position(const SolverConfiguration& config, const GreedyShapes& shapes, int n,
                                     const std::vector<std::pair<int, Point>>& placed, int corner)
{
    namespace CU = Slic3r::Biz::Algorithms::ClipperUtils;
    // The exact footprint must be inside the plate. The decimated polygon can be smaller. Round inward.
    const BoundingBox plate = config.plate_bounding_box;
    const BoundingBox& box = shapes.footprint[n];
    const auto floor_div = [](coord_t a) { return a >= 0 ? a / SEQ_SLICER_SCALE_FACTOR : -((-a + SEQ_SLICER_SCALE_FACTOR - 1) / SEQ_SLICER_SCALE_FACTOR); };
    const auto ceil_div = [&floor_div](coord_t a) { return -floor_div(-a); };
    const coord_t x0 = ceil_div(plate.min.x() * SEQ_SLICER_SCALE_FACTOR - box.min.x());
    const coord_t x1 = floor_div(plate.max.x() * SEQ_SLICER_SCALE_FACTOR - box.max.x());
    const coord_t y0 = ceil_div(plate.min.y() * SEQ_SLICER_SCALE_FACTOR - box.min.y());
    const coord_t y1 = floor_div(plate.max.y() * SEQ_SLICER_SCALE_FACTOR - box.max.y());
    if (x0 > x1 || y0 > y1)
        return std::nullopt;
    const Polygon inside{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};

    Polygons forbidden;
    for (const auto& [p, at] : placed) {
        std::vector<Polygon> zones = shapes.unreachable[p];
        zones.push_back(shapes.polygon[p]);  // the objects do not overlap
        for (const Polygon& u : zones) {
            Points sum;
            for (const Point& b : u.points)
                for (const Point& a : shapes.polygon[n].points)
                    sum.emplace_back(b.x() - a.x() + at.x(), b.y() - a.y() + at.y());
            forbidden.push_back(convex_hull_of(sum));
        }
    }
    // A margin (default 10 units, 1 mm): a point near the edge of a zone can touch the polygon after the rounding of
    // the check. With 1 unit, 2 of 89 corpus runs had a conflict.
    const Polygons grown = forbidden.empty() ? Polygons() : CU::offset(CU::union_(forbidden), float(GREEDY_MARGIN));
    const ExPolygons free = CU::diff_ex(Polygons{inside}, grown);

    std::optional<Point> best;
    const auto score = [corner](const Point& q) {
        const double x = corner & 1 ? -double(q.x()) : double(q.x());
        const double y = corner & 2 ? -double(q.y()) : double(q.y());
        return std::make_pair(y, x);
    };
    for (const ExPolygon& e : free) {
        for (const Point& q : e.contour.points)
            if (! best || score(q) < score(*best))
                best = q;
        for (const Polygon& hole : e.holes)
            for (const Point& q : hole.points)
                if (! best || score(q) < score(*best))
                    best = q;
    }
    return best;
}

std::vector<ScheduledPlate> greedy_arrange_corner(const SolverConfiguration& config, const std::vector<ObjectToPrint>& objects,
                                                  const GreedyShapes& shapes, int corner, int order, bool split_glued)
{
    // Units: the glued instances of one object stay together.
    std::vector<std::vector<int>> units;
    for (int i = 0; i < int(objects.size()); ++i) {
        if (i == 0 || ! objects[i - 1].glued_to_next)
            units.emplace_back();
        units.back().push_back(i);
    }
    // Orders: 0 by height, 1 by height and then by area (the largest first), 2 by area, 3 the input order.
    const auto area = [&shapes](const std::vector<int>& u) {
        double a = 0.;
        for (int n : u)
            a += std::abs(shapes.polygon[n].area());
        return a;
    };
    const auto height = [&objects](const std::vector<int>& u) { return objects[u.front()].total_height; };
    if (order == 0)
        std::stable_sort(units.begin(), units.end(), [&](const auto& a, const auto& b) { return height(a) < height(b); });
    else if (order == 1)
        std::stable_sort(units.begin(), units.end(), [&](const auto& a, const auto& b) {
            return height(a) < height(b) || (height(a) == height(b) && area(a) > area(b)); });
    else if (order == 2)
        std::stable_sort(units.begin(), units.end(), [&](const auto& a, const auto& b) { return area(a) > area(b); });

    std::vector<ScheduledPlate> plates;
    std::vector<bool> done(units.size(), false);
    size_t remaining = units.size();
    while (remaining > 0) {
        std::vector<std::pair<int, Point>> placed;
        for (size_t u = 0; u < units.size(); ++u) {
            if (done[u])
                continue;
            std::vector<std::pair<int, Point>> tentative = placed;
            bool ok = true;
            for (int n : units[u]) {
                const std::optional<Point> at = greedy_position(config, shapes, n, tentative, corner);
                if (! at) {
                    ok = false;
                    break;
                }
                tentative.emplace_back(n, *at);
            }
            if (ok) {
                placed = std::move(tentative);
                done[u] = true;
                --remaining;
            }
        }
        if (placed.empty()) {
            if (! split_glued)
                throw std::runtime_error("greedy: an object or a unit of glued objects does not fit on an empty plate");
            // A unit of glued instances that does not fit on one plate: the instances go on as single objects.
            const auto it = std::find_if(units.begin(), units.end(), [&](const std::vector<int>& u) {
                return ! done[&u - units.data()] && u.size() > 1; });
            if (it == units.end())
                throw std::runtime_error("greedy: an object does not fit on an empty plate");
            const size_t at = it - units.begin();
            std::vector<std::vector<int>> singles;
            for (int n : units[at])
                singles.push_back({n});
            units.erase(units.begin() + at);
            done.erase(done.begin() + at);
            units.insert(units.begin() + at, singles.begin(), singles.end());
            done.insert(done.begin() + at, singles.size(), false);
            remaining += singles.size() - 1;
            continue;
        }
        ScheduledPlate plate;
        for (const auto& [n, at] : placed)
            plate.scheduled_objects.emplace_back(objects[n].id, at.x() * SEQ_SLICER_SCALE_FACTOR, at.y() * SEQ_SLICER_SCALE_FACTOR);
        plates.push_back(std::move(plate));
    }
    return plates;
}

} // namespace

std::vector<ScheduledPlate> schedule_ObjectsForSequentialPrintGreedy(const SolverConfiguration        &solver_configuration,
								     const PrinterGeometry            &printer_geometry,
								     const std::vector<ObjectToPrint> &objects_to_print,
								     bool                              split_glued)
{
    const SolverConfiguration& config = solver_configuration;
    const PrinterGeometry& printer = printer_geometry;
    const std::vector<ObjectToPrint>& objects = objects_to_print;
    const GreedyShapes shapes = greedy_shapes(config, printer, objects);
    std::vector<ScheduledPlate> best;
    for (int k = 0; k < 4 * GREEDY_ORDERS; ++k) {
        const int corner = k % 4, order = k / 4;
        std::vector<ScheduledPlate> plates = greedy_arrange_corner(config, objects, shapes, corner, order, split_glued);
        const auto first = [](const std::vector<ScheduledPlate>& p) { return p.empty() ? size_t(0) : p.front().scheduled_objects.size(); };
        if (best.empty() || plates.size() < best.size() || (plates.size() == best.size() && first(plates) > first(best)))
            best = std::move(plates);
    }
    return best;
}


} // namespace Sequential
