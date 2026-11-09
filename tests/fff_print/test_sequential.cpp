#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "libslic3r/ArrangeHelper.hpp"
#include "libslic3r/ConfigViews.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SeqArrange.hpp"
#include "Slic3r/Biz/GCodeReader/GCodeReader.hpp"
#include "Slic3r/Biz/Slicing/BackgroundProcess.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <set>
#include <sstream>
#include <limits>
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Approx;

using Biz::Algorithms::ModelObject::add_volume;

namespace {

struct Placement
{
    std::string name;
    double x;
    double y;
    double height;
};

// Adds a 20 x 20 mm box of the given height for each placement, centered at (x, y), in the given order.
void add_boxes(Domain::Model& model, const std::vector<Placement>& placements)
{
    for (const Placement& p : placements) {
        Domain::ModelObject* object = model.add_object();
        object->name = p.name;
        add_volume(object, mesh(TestMesh::cube_20x20x20, Vec3d(-10., -10., 0.), Vec3d(1., 1., p.height / 20.)));
        object->add_instance()->set_offset(Vec3d(p.x, p.y, 0.));
    }
}

std::optional<std::pair<std::string, std::string>> check(
    const std::vector<Placement>& placements,
    double clearance_radius,
    double clearance_height,
    const Domain::Preset::HwModel& hw_model = {}
)
{
    TestConfig config;
    config.hw_config.model = hw_model;
    config.print.items.opt("complete_objects").set(true);
    config.printer.items.opt("extruder_clearance_radius").set(clearance_radius);
    config.printer.items.opt("extruder_clearance_height").set(clearance_height);
    // Without gantry data for hw_model, the check uses the clearance radius and height.
    const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};

    Domain::Model model;
    add_boxes(model, placements);
    return check_seq_conflict(model, view);
}

} // namespace

TEST_CASE("Sequential print: collision check", "[SequentialPrint]")
{
    SECTION("Low objects far apart do not collide") {
        CHECK(! check({{"a", 50., 100., 10.}, {"b", 150., 100., 10.}}, 20., 20.).has_value());
    }

    SECTION("Objects closer than the clearance radius collide") {
        const auto conflict = check({{"a", 90., 100., 10.}, {"b", 115., 100., 10.}}, 20., 20.);
        REQUIRE(conflict.has_value());
        // The first object is hit while the second object is printed.
        CHECK(conflict->first == "a");
        CHECK(conflict->second == "b");
    }

    SECTION("An object taller than the clearance height is hit by the gantry") {
        const auto conflict = check({{"tall", 50., 100., 30.}, {"b", 150., 100., 10.}}, 20., 20.);
        REQUIRE(conflict.has_value());
        CHECK(conflict->first == "tall");
        CHECK(conflict->second == "b");
    }

    SECTION("A single object never collides") {
        CHECK(! check({{"a", 100., 100., 30.}}, 20., 20.).has_value());
    }
}

TEST_CASE("Sequential print: gantry data of the printer model", "[SequentialPrint]")
{
    // geometries.json is in the source resources directory. Restore the old value also if a REQUIRE throws.
    struct ResourcesDirGuard
    {
        std::string old{resources_dir()};
        ResourcesDirGuard() { set_resources_dir(std::string(TEST_DATA_DIR) + "/../../resources"); }
        ~ResourcesDirGuard() { set_resources_dir(old); }
    } resources_dir_guard;

    // A 31 mm tall object is printed first, 47 mm in +Y and 100 mm in -X from the second object.
    // The check mirrors the slice polygons of geometries.json: the INDX gantry slice (29 mm, Y -58 to -36 mm)
    // makes the band from +26 to +68 mm in Y unreachable (with the 20 mm footprint of the second object).
    // CORE One INDX: the gantry is at 29 mm, so it hits the object.
    // CORE One: the gantry is at 33 mm, above the object. The toolhead does not reach 100 mm to the side.
    const std::vector<Placement> placements{{"tall", 50., 147., 31.}, {"b", 150., 100., 10.}};

    SECTION("CORE One INDX uses its own gantry data") {
        const auto conflict = check(placements, 1., 1., {.model = "COREONE_INDX", .base_model = "COREONE_INDX"});
        REQUIRE(conflict.has_value());
        CHECK(conflict->first == "tall");
    }

    SECTION("CORE One gantry is above the object") {
        CHECK(! check(placements, 1., 1., {.model = "COREONE", .base_model = "COREONE"}).has_value());
    }

    SECTION("An unknown model does not use the data of its base model") {
        // With the CORE One data, there is no conflict. The clearance fallback (radius 60, height 25) finds one.
        const auto conflict = check(placements, 60., 25., {.model = "UNKNOWN_INDX", .base_model = "COREONE"});
        CHECK(conflict.has_value());
    }
}

TEST_CASE("Sequential print: all gantry slices at one height are used", "[SequentialPrint]")
{
    struct ResourcesDirGuard
    {
        std::string old{resources_dir()};
        ResourcesDirGuard() { set_resources_dir(std::string(TEST_DATA_DIR) + "/../../resources"); }
        ~ResourcesDirGuard() { set_resources_dir(old); }
    } resources_dir_guard;

    // The CORE One has two convex slices at 23 mm. The second one (Y -98 to -47 mm) reaches 29 mm farther
    // than the 2 mm slice (Y -69 to 20 mm). Mirrored and with the two 20 mm footprints, the 2 mm slice makes
    // the band up to +89 mm in Y unreachable, and the second 23 mm slice the band up to +118 mm.
    // The first object is 102 mm in +Y from the second object.
    const Domain::Preset::HwModel core_one{.model = "COREONE", .base_model = "COREONE"};

    SECTION("An object above 23 mm is hit by the second slice") {
        const auto conflict = check({{"tall", 100., 152., 30.}, {"b", 100., 50., 10.}}, 1., 1., core_one);
        REQUIRE(conflict.has_value());
        CHECK(conflict->first == "tall");
        CHECK(conflict->second == "b");
    }

    SECTION("An object below 23 mm is not hit") {
        CHECK(! check({{"low", 100., 152., 20.}, {"b", 100., 50., 10.}}, 1., 1., core_one).has_value());
    }
}

namespace {

// Slices the boxes with complete_objects and returns the G-code. Without a printer model, the collision check
// uses the clearance values. A conflict gives only a warning, so the G-code is generated.
// If the environment variable SEQ_TEST_GCODE_DUMP is set, the G-code is also written to that file.
std::string slice_sequential(const std::vector<Placement>& placements)
{
    TestConfig config;
    config.print.items.opt("complete_objects").set(true);
    config.print.items.opt("gcode_comments").set(true);
    config.print.items.opt("skirts").set(0);
    config.print.items.opt("layer_height").set(0.2);
    config.print.items.opt("first_layer_height").set(Domain::FloatOrPercentage{0.2});

    Domain::Model model;
    add_boxes(model, placements);
    Print print;
    Domain::Preset::SelectedPresetMetadata preset_metadata = create_dummy_selected_preset_metadata(config.hw_config);
    auto gcode_metadata = Biz::Slicing::build_gcode_metadata({}, preset_metadata, config);
    Domain::Bed model_bed;
    Domain::BedInstance bed_instance{model_bed};
    for (const Domain::ModelObject* object : model.objects)
        for (Domain::ModelInstance* instance : object->instances)
            bed_instance.model_instances.push_back(instance);
    print.update(model, config, bed_instance, preset_metadata,
                 Biz::Slicing::build_metadata_serializer(gcode_metadata, preset_metadata, config));
    print.validate();
    const std::string gcode = Test::gcode(print);
    if (const char* dump = std::getenv("SEQ_TEST_GCODE_DUMP"))
        boost::nowide::ofstream(dump) << gcode;
    return gcode;
}

// Returns the XY targets of the moves to the next object, in G-code order.
std::vector<Vec2d> next_object_moves(const std::string& gcode)
{
    std::vector<Vec2d> out;
    Biz::GCodeReader::GCodeReader reader;
    reader.parse_buffer(gcode, [&](Biz::GCodeReader::GCodeReader& self, const Biz::GCodeReader::GCodeReader::GCodeLine& line) {
        if (line.comment().find("move to origin position for next object") != std::string_view::npos)
            out.emplace_back(line.new_X(self), line.new_Y(self));
    });
    return out;
}

} // namespace

TEST_CASE("Sequential print: objects are printed in model order", "[SequentialPrint]")
{
    // 17 low boxes in a 6 x 3 grid, in an order that is not sorted by position. Before the fix, the order
    // came from the memory addresses of the print instances.
    std::vector<Placement> placements;
    for (int i = 0; i < 17; ++i) {
        const int k = (i * 7) % 17; // a permutation of 0..16
        placements.push_back({"box" + std::to_string(i), 20. + 30. * (k % 6), 40. + 50. * (k / 6), 2.});
    }
    const std::vector<Vec2d> moves = next_object_moves(slice_sequential(placements));
    REQUIRE(moves.size() == placements.size() - 1);
    for (size_t i = 1; i < placements.size(); ++i) {
        INFO("object " << i);
        CHECK(moves[i - 1].x() == Approx(placements[i].x));
        CHECK(moves[i - 1].y() == Approx(placements[i].y));
    }
}

TEST_CASE("Sequential print: travel to the next object clears the printed objects", "[SequentialPrint]")
{
    // The tall object is printed first, between the two low objects. The travel from the first low object to
    // the second low object crosses the tall object. Before this travel, GCode.cpp lifts Z to the highest
    // printed layer ("ensure z position to clear all already printed objects").
    const double tall_height = 25.;
    const std::vector<Placement> placements{{"tall", 100., 100., tall_height}, {"low1", 30., 100., 5.}, {"low2", 170., 100., 5.}};
    const std::string gcode = slice_sequential(placements);

    // The objects are printed in model order.
    const std::vector<Vec2d> moves = next_object_moves(gcode);
    REQUIRE(moves.size() == 2);
    CHECK(moves[0].x() == Approx(30.));
    CHECK(moves[1].x() == Approx(170.));

    // The footprint of the tall object, 1 mm larger on each side.
    const double min_x = 100. - 11., max_x = 100. + 11., min_y = 100. - 11., max_y = 100. + 11.;
    const auto crosses_tall = [&](double x0, double y0, double x1, double y1) {
        const double len = std::hypot(x1 - x0, y1 - y0);
        const int steps = std::max(1, int(len / 0.25));
        for (int i = 0; i <= steps; ++i) {
            const double t = double(i) / steps;
            const double x = x0 + t * (x1 - x0), y = y0 + t * (y1 - y0);
            if (x > min_x && x < max_x && y > min_y && y < max_y)
                return true;
        }
        return false;
    };

    int objects_done = 0;
    int crossings = 0;
    double lowest_crossing_z = std::numeric_limits<double>::max();
    std::string lowest_crossing;
    Biz::GCodeReader::GCodeReader reader;
    reader.parse_buffer(gcode, [&](Biz::GCodeReader::GCodeReader& self, const Biz::GCodeReader::GCodeReader::GCodeLine& line) {
        if (line.comment().find("move to origin position for next object") != std::string_view::npos)
            ++objects_done;
        // After the tall object is done, each move that crosses it must be above it.
        if (objects_done > 0 && line.cmd_is("G1") && line.dist_XY(self) > 0. &&
            crosses_tall(self.x(), self.y(), line.new_X(self), line.new_Y(self))) {
            ++crossings;
            if (double(line.new_Z(self)) < lowest_crossing_z) {
                lowest_crossing_z = double(line.new_Z(self));
                lowest_crossing = "from X" + std::to_string(self.x()) + " Y" + std::to_string(self.y()) + ": " + line.raw();
            }
        }
    });

    // The travel from low1 to low2 crosses the tall object.
    REQUIRE(crossings > 0);
    INFO("lowest move over the tall object: " << lowest_crossing);
    CHECK(lowest_crossing_z >= tall_height);
}

TEST_CASE("Sequential print: geometries.json is valid", "[SequentialPrint]")
{
    // The file that ships with the slicer. ArrangeHelper.cpp does not use the data of a printer with an error
    // in its entry: then the check uses the clearance values. Thus find the errors here.
    namespace pt = boost::property_tree;
    pt::ptree tree;
    pt::read_json(std::string(TEST_DATA_DIR) + "/../../resources/data/printer_gantries/geometries.json", tree);

    std::set<std::string> all_models;
    for (const auto& printer : tree.get_child("printers")) {
        std::vector<std::string> models;
        for (const auto& model : printer.second.get_child("printer_models"))
            models.push_back(model.second.data());
        REQUIRE(! models.empty());
        INFO("printer " << models.front());
        for (const std::string& model : models) {
            INFO("model " << model);
            CHECK(all_models.insert(model).second); // each model id in one entry only
        }

        std::set<double> heights;
        for (const auto& slice : printer.second.get_child("slices")) {
            const double height = slice.second.get<double>("height");
            INFO("height " << height);
            CHECK(heights.insert(height).second); // one entry for each height
            const std::string type = slice.second.get<std::string>("type");
            CHECK((type == "convex" || type == "box"));
            const auto polygons = slice.second.get_child("polygons");
            CHECK(! polygons.empty());
            for (const auto& polygon : polygons) {
                std::string text = polygon.second.data();
                std::replace(text.begin(), text.end(), ';', ' ');
                std::replace(text.begin(), text.end(), ',', ' ');
                std::istringstream ss(text);
                int points = 0;
                double x, y;
                while (ss >> x >> y)
                    ++points;
                CHECK(points >= 3);
            }
        }
        CHECK(heights.count(0.) == 1); // the nozzle level
    }
}

TEST_CASE("Sequential print: a working print order for fixed positions", "[SequentialPrint]")
{
    // Clearance fallback (radius 20, height 20): a 40 x 40 mm toolhead box, and a gantry band of 40 mm in Y
    // that spans X at 20 mm. The tall object (25 mm) reaches the gantry. The low object is 100 mm away in X,
    // so the toolhead box does not reach the other object in either order. Printed first, the tall object is
    // hit by the gantry while the low object (15 mm away in Y) prints. Printed last, nothing is hit: the
    // low object is below the gantry.
    const std::vector<Placement> placements{{"tall", 60., 100., 25.}, {"low", 160., 115., 5.}};

    TestConfig config;
    config.print.items.opt("complete_objects").set(true);
    config.printer.items.opt("extruder_clearance_radius").set(20.);
    config.printer.items.opt("extruder_clearance_height").set(20.);
    const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};

    Domain::Model model;
    add_boxes(model, placements);
    REQUIRE(check_seq_conflict(model, view).has_value());

    const auto order = find_seq_print_order(model, view);
    REQUIRE(order.has_value());
    CHECK(*order == std::vector<std::string>{"low", "tall"});

    // The model in that order has no conflict.
    Domain::Model reordered;
    add_boxes(reordered, {placements[1], placements[0]});
    CHECK(! check_seq_conflict(reordered, view).has_value());

    SECTION("Two tall objects in the same gantry band have no order")
    {
        Domain::Model both_tall;
        add_boxes(both_tall, {{"tall1", 60., 100., 25.}, {"tall2", 160., 100., 25.}});
        CHECK(check_seq_conflict(both_tall, view).has_value());
        CHECK(! find_seq_print_order(both_tall, view).has_value());
    }
}

TEST_CASE("Sequential print: an object above the ceiling clearance prints last", "[SequentialPrint]")
{
    // The CORE One firmware stops when a printed object gets above Z_CEILING_CLEARANCE (100 mm) while the bed
    // moves up for the next object (issue #15101). geometries.json has a box over the whole bed at 99 mm.
    struct ResourcesDirGuard
    {
        std::string old{resources_dir()};
        ResourcesDirGuard() { set_resources_dir(std::string(TEST_DATA_DIR) + "/../../resources"); }
        ~ResourcesDirGuard() { set_resources_dir(old); }
    } resources_dir_guard;

    const Domain::Preset::HwModel core_one{.model = "COREONE", .base_model = "COREONE"};
    // Far apart: 150 mm in X, 100 mm in Y. Only the ceiling box can make a conflict.
    const std::vector<Placement> tall_first{{"tree", 50., 50., 110.}, {"small", 200., 150., 10.}};

    SECTION("Printed first, the tall object is a conflict")
    {
        const auto conflict = check(tall_first, 1., 1., core_one);
        REQUIRE(conflict.has_value());
        CHECK(conflict->first == "tree");
        CHECK(conflict->second == "small");
    }

    SECTION("Printed last, the tall object is not a conflict")
    {
        CHECK(! check({tall_first[1], tall_first[0]}, 1., 1., core_one).has_value());
    }

    SECTION("The solver prints the tall object last")
    {
        TestConfig config;
        config.hw_config.model = core_one;
        config.print.items.opt("complete_objects").set(true);
        const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};
        Domain::Model model;
        add_boxes(model, tall_first);
        const auto order = find_seq_print_order(model, view);
        REQUIRE(order.has_value());
        CHECK(*order == std::vector<std::string>{"small", "tree"});

        SECTION("Two objects above the ceiling clearance have no order")
        {
            Domain::Model trees;
            add_boxes(trees, {{"tree1", 50., 50., 110.}, {"tree2", 200., 150., 110.}});
            CHECK(check_seq_conflict(trees, view).has_value());
            CHECK(! find_seq_print_order(trees, view).has_value());
        }
    }
}

namespace {

// Processes the boxes with complete_objects and returns the warnings of Print::process().
std::vector<Biz::Slicing::Warning> process_sequential(const std::vector<Placement>& placements,
                                                      const std::function<void(TestConfig&)>& tweak)
{
    TestConfig config;
    config.print.items.opt("complete_objects").set(true);
    config.print.items.opt("layer_height").set(0.2);
    config.print.items.opt("first_layer_height").set(Domain::FloatOrPercentage{0.2});
    config.print.items.opt("skirts").set(0);
    config.printer.items.opt("extruder_clearance_radius").set(20.);
    config.printer.items.opt("extruder_clearance_height").set(20.);
    tweak(config);

    Domain::Model model;
    add_boxes(model, placements);
    Print print;
    std::vector<Biz::Slicing::Warning> warnings;
    print.append_warning_callback = [&warnings](Biz::Slicing::Warning w) { warnings.push_back(std::move(w)); };
    Domain::Preset::SelectedPresetMetadata preset_metadata = create_dummy_selected_preset_metadata(config.hw_config);
    auto gcode_metadata = Biz::Slicing::build_gcode_metadata({}, preset_metadata, config);
    Domain::Bed model_bed;
    Domain::BedInstance bed_instance{model_bed};
    for (const Domain::ModelObject* object : model.objects)
        for (Domain::ModelInstance* instance : object->instances)
            bed_instance.model_instances.push_back(instance);
    print.update(model, config, bed_instance, preset_metadata,
                 Biz::Slicing::build_metadata_serializer(gcode_metadata, preset_metadata, config));
    print.validate();
    print.process();
    return warnings;
}

bool has_warning(const std::vector<Biz::Slicing::Warning>& warnings, Biz::Slicing::WarningCode code)
{
    return std::any_of(warnings.begin(), warnings.end(), [code](const auto& w) { return w.code == code; });
}

} // namespace

TEST_CASE("Sequential print: a raft lifts the object", "[SequentialPrint]")
{
    // Clearance fallback (radius 20, height 20). The tall box (19.5 mm) is just below the gantry. The low box is
    // 100 mm away in X and 25 mm in Y: only the gantry band can reach the tall box.
    const std::vector<Placement> placements{{"tall", 60., 115., 19.5}, {"low", 160., 140., 5.}};

    SECTION("In the check: a lift of 0.95 mm puts the tall box into the gantry")
    {
        TestConfig config;
        config.print.items.opt("complete_objects").set(true);
        config.printer.items.opt("extruder_clearance_radius").set(20.);
        config.printer.items.opt("extruder_clearance_height").set(20.);
        const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};
        Domain::Model model;
        add_boxes(model, placements);
        CHECK(! check_seq_conflict(model, view).has_value());

        SeqRafts rafts;
        for (const Domain::ModelObject* mo : model.objects)
            rafts[mo->instances.front()->id().id] = SeqRaft{.z_lift = 0.95, .xy_expansion = 4.5};
        const auto conflict = check_seq_conflict(model, view, rafts);
        REQUIRE(conflict.has_value());
        CHECK(conflict->first == "tall");
    }

    SECTION("Through Print: raft_layers = 3 gives the collision warning")
    {
        CHECK(! has_warning(process_sequential(placements, [](TestConfig&) {}), Biz::Slicing::WarningCode::SequentialCollision));
        CHECK(has_warning(process_sequential(placements, [](TestConfig& c) { c.print.items.opt("raft_layers").set(3); }),
                          Biz::Slicing::WarningCode::SequentialCollision));
    }
}

TEST_CASE("Sequential print: a high skirt is hit", "[SequentialPrint]")
{
    // The skirt prints before all objects. The lowest toolhead slice of the CORE One above the tip is at 2 mm.
    struct ResourcesDirGuard
    {
        std::string old{resources_dir()};
        ResourcesDirGuard() { set_resources_dir(std::string(TEST_DATA_DIR) + "/../../resources"); }
        ~ResourcesDirGuard() { set_resources_dir(old); }
    } resources_dir_guard;
    const std::vector<Placement> placements{{"a", 60., 100., 10.}, {"b", 160., 100., 10.}};
    using Biz::Slicing::WarningCode;
    const auto core_one = [](TestConfig& c) { c.hw_config.model = Domain::Preset::HwModel{.model = "COREONE", .base_model = "COREONE"}; };

    SECTION("The clearance of the CORE One is 2 mm. Without gantry data, it is not known")
    {
        TestConfig fallback;
        const PrintConfigView fallback_view{std::make_shared<const Domain::FullConfigFDM>(fallback.get_full_config())};
        CHECK(seq_skirt_clearance(fallback_view) == 0.);
        TestConfig config;
        core_one(config);
        const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};
        CHECK(seq_skirt_clearance(view) == Approx(2.));
    }

    SECTION("CORE One: a skirt of one layer is not hit")
    {
        const auto warnings = process_sequential(placements, [&](TestConfig& c) {
            core_one(c);
            c.print.items.opt("skirts").set(1);
            c.print.items.opt("skirt_height").set(1);
        });
        CHECK(! has_warning(warnings, WarningCode::SequentialSkirtCollision));
    }

    SECTION("CORE One: a skirt of 15 layers (3 mm) is hit")
    {
        const auto warnings = process_sequential(placements, [&](TestConfig& c) {
            core_one(c);
            c.print.items.opt("skirts").set(1);
            c.print.items.opt("skirt_height").set(15);
        });
        CHECK(has_warning(warnings, WarningCode::SequentialSkirtCollision));
    }

    SECTION("Without gantry data: a skirt of 15 layers is not reported, a draft shield is")
    {
        CHECK(! has_warning(process_sequential(placements, [](TestConfig& c) {
            c.print.items.opt("skirts").set(1);
            c.print.items.opt("skirt_height").set(15);
        }), WarningCode::SequentialSkirtCollision));
        CHECK(has_warning(process_sequential(placements, [](TestConfig& c) {
            c.print.items.opt("skirts").set(1);
            c.print.items.opt("draft_shield").set(Domain::DraftShield::dsEnabled);
        }), WarningCode::SequentialSkirtCollision));
    }
}

TEST_CASE("Sequential print: the suggested order keeps the instances of an object together", "[SequentialPrint]")
{
    // Clearance fallback (radius 20, height 20). Object "pair" has two instances. The second instance is 25 mm
    // from "low" in Y, 100 mm away in X, and 25 mm high: the gantry band of "low" hits it, so "low" must print
    // first. The first instance of "pair" is far away. A per-instance order would be pair (1), low, pair (2),
    // which the user cannot set. The order for whole objects is low, pair (1), pair (2).
    TestConfig config;
    config.print.items.opt("complete_objects").set(true);
    config.printer.items.opt("extruder_clearance_radius").set(20.);
    config.printer.items.opt("extruder_clearance_height").set(20.);
    const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};

    Domain::Model model;
    Domain::ModelObject* pair = model.add_object();
    pair->name = "pair";
    add_volume(pair, mesh(TestMesh::cube_20x20x20, Vec3d(-10., -10., 0.), Vec3d(1., 1., 25. / 20.)));
    pair->add_instance()->set_offset(Vec3d(60., 30., 0.));
    pair->add_instance()->set_offset(Vec3d(60., 115., 0.));
    add_boxes(model, {{"low", 160., 140., 5.}});

    REQUIRE(check_seq_conflict(model, view).has_value());
    const auto order = find_seq_print_order(model, view);
    REQUIRE(order.has_value());
    CHECK(*order == std::vector<std::string>{"low", "pair (1)", "pair (2)"});
}

TEST_CASE("Sequential print: the arrange gives plates without a collision", "[SequentialPrint]")
{
    struct ResourcesDirGuard
    {
        std::string old{resources_dir()};
        ResourcesDirGuard() { set_resources_dir(std::string(TEST_DATA_DIR) + "/../../resources"); }
        ~ResourcesDirGuard() { set_resources_dir(old); }
    } resources_dir_guard;

    TestConfig config;
    config.hw_config.model = {.model = "COREONE", .base_model = "COREONE"};
    config.print.items.opt("complete_objects").set(true);
    config.printer.items.opt("bed_shape").set(std::vector<Vec2d>{{0., 0.}, {250., 0.}, {250., 220.}, {0., 220.}});
    const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};

    // Boxes of 20 x 20 mm, all at one position. Some are taller than the gantry (33 mm) or the ceiling box (99 mm).
    // The last object has 3 instances.
    const std::vector<double> heights{10., 40., 10., 25., 50., 10., 105., 15., 30., 10.};
    Domain::Model model;
    std::vector<Placement> boxes;
    for (size_t i = 0; i < heights.size(); ++i)
        boxes.push_back({"box" + std::to_string(i), 100., 100., heights[i]});
    add_boxes(model, boxes);
    for (int k = 0; k < 2; ++k)
        model.objects.back()->add_instance()->set_offset(Vec3d(100., 100., 0.));

    std::vector<size_t> ids;
    std::map<size_t, std::pair<const Domain::ModelObject*, double>> by_instance;
    for (size_t i = 0; i < model.objects.size(); ++i)
        for (const Domain::ModelInstance* mi : model.objects[i]->instances) {
            ids.push_back(mi->id().id);
            by_instance[mi->id().id] = {model.objects[i], heights[i]};
        }

    const SeqArrange seq_arrange(model, view, ids);
    REQUIRE(! seq_arrange.empty());
    CHECK(seq_arrange.not_arranged().empty());
    int last_progress = -1;
    const std::vector<SeqArrange::Plate> plates = seq_arrange.process([&last_progress](int p) {
        CHECK(p >= 0);
        CHECK(p <= 100);
        last_progress = p;
    });
    CHECK(last_progress == 100);
    REQUIRE(! plates.empty());

    std::set<size_t> placed;
    for (const SeqArrange::Plate& plate : plates) {
        // The objects of the plate in print order, at their new positions. Each object has its instances, one after
        // the other: the instances of one object must be together.
        Domain::Model plate_model;
        const Domain::ModelObject* last_source = nullptr;
        for (const SeqArrange::Placement& p : plate) {
            CHECK(placed.insert(p.instance_id).second);
            const auto& [source, height] = by_instance.at(p.instance_id);
            CHECK(p.x >= 10.);
            CHECK(p.x <= 240.);
            CHECK(p.y >= 10.);
            CHECK(p.y <= 210.);
            if (source != last_source) {
                add_boxes(plate_model, {{source->name, p.x, p.y, height}});
                last_source = source;
            } else
                plate_model.objects.back()->add_instance()->set_offset(Vec3d(p.x, p.y, 0.));
        }
        CHECK(! check_seq_conflict(plate_model, view).has_value());
    }
    CHECK(placed.size() == ids.size());

    // The 3 instances of the last object are on one plate, one after the other.
    const Domain::ModelObject* last = model.objects.back();
    bool together = false;
    for (const SeqArrange::Plate& plate : plates)
        for (size_t i = 0; i + 2 < plate.size(); ++i)
            together |= plate[i].instance_id == last->instances[0]->id().id &&
                        plate[i + 1].instance_id == last->instances[1]->id().id &&
                        plate[i + 2].instance_id == last->instances[2]->id().id;
    CHECK(together);
}

TEST_CASE("Sequential print: the arrange does not place an object larger than the bed", "[SequentialPrint]")
{
    struct ResourcesDirGuard
    {
        std::string old{resources_dir()};
        ResourcesDirGuard() { set_resources_dir(std::string(TEST_DATA_DIR) + "/../../resources"); }
        ~ResourcesDirGuard() { set_resources_dir(old); }
    } resources_dir_guard;

    TestConfig config;
    config.hw_config.model = {.model = "COREONE", .base_model = "COREONE"};
    config.print.items.opt("complete_objects").set(true);
    config.printer.items.opt("bed_shape").set(std::vector<Vec2d>{{0., 0.}, {250., 0.}, {250., 220.}, {0., 220.}});
    const PrintConfigView view{std::make_shared<const Domain::FullConfigFDM>(config.get_full_config())};

    Domain::Model model;
    add_boxes(model, {{"small", 100., 100., 10.}});
    Domain::ModelObject* large = model.add_object();
    large->name = "large";
    add_volume(large, mesh(TestMesh::cube_20x20x20, Vec3d(-10., -10., 0.), Vec3d(15., 1., 1.)));
    large->add_instance()->set_offset(Vec3d(100., 100., 0.));

    const SeqArrange seq_arrange(model, view, {model.objects[0]->instances[0]->id().id, large->instances[0]->id().id});
    REQUIRE(seq_arrange.not_arranged().size() == 1);
    CHECK(seq_arrange.not_arranged().front() == large->instances[0]->id().id);
    const std::vector<SeqArrange::Plate> plates = seq_arrange.process([](int) {});
    REQUIRE(plates.size() == 1);
    CHECK(plates.front().size() == 1);
}
