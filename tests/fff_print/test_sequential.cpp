#include <catch2/catch_test_macros.hpp>

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
#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

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
