#include "ArrangeHelper.hpp"

#include "libseqarrange/seq_interface.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "Slic3r/Biz/Algorithms/BoundingBox.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/TriangleMesh.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Domain/Model.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <Slic3r/Log.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/property_tree/json_parser.hpp>

namespace Slic3r {

using namespace Slic3r::Biz;

namespace {

enum class ShapeType { Box, Convex };

struct ExtruderSlice
{
	coord_t height;
	ShapeType shape_type;
	std::vector<Polygon> polygons;
};

// Reads the extruder and gantry shape of the printer from resources/data/printer_gantries/geometries.json.
// A printer is matched by its exact hardware model id only. The base model id is not used: printers with the
// same base model can have a different toolhead or gantry (for example a tool changer), and wrong data can
// hide a collision. Without an entry, the caller uses extruder_clearance_radius and extruder_clearance_height.
// Returns an empty vector if there is no entry for the printer or the file cannot be read.
std::vector<ExtruderSlice> read_gantry_slices(const Domain::Preset::HwModel& hw_model)
{
	namespace pt = boost::property_tree;
	pt::ptree tree;
	try {
		boost::nowide::ifstream in(resources_dir() + "/data/printer_gantries/geometries.json");
		pt::read_json(in, tree);
	} catch (const pt::json_parser_error&) {
		return {};
	}

	const auto find_printer = [&tree](const std::string& model) -> const pt::ptree* {
		if (model.empty())
			return nullptr;
		const auto printers = tree.get_child_optional("printers");
		if (! printers)
			return nullptr;
		for (const auto& printer : *printers) {
			const auto names = printer.second.get_child_optional("printer_models");
			if (! names)
				continue;
			for (const auto& name : *names)
				if (name.second.data() == model)
					return &printer.second;
		}
		return nullptr;
	};
	const pt::ptree* printer = find_printer(hw_model.model);
	if (printer == nullptr)
		return {};

	std::vector<ExtruderSlice> slices;
	try {
		for (const auto& obj : printer->get_child("slices")) {
			ExtruderSlice slice;
			slice.height = scaled(obj.second.get<double>("height"));
			slice.shape_type = obj.second.get<std::string>("type") == "box" ? ShapeType::Box : ShapeType::Convex;
			for (const auto& polygon : obj.second.get_child("polygons")) {
				Polygon pgn;
				std::string pgn_str = polygon.second.data();
				boost::replace_all(pgn_str, ";", " ");
				boost::replace_all(pgn_str, ",", " ");
				std::stringstream ss(pgn_str);
				double x = 0.;
				double y = 0.;
				while (ss >> x >> y)
					pgn.points.emplace_back(scaled(Vec2d(x, y)));
				if (! pgn.points.empty())
					slice.polygons.emplace_back(std::move(pgn));
			}
			slices.emplace_back(std::move(slice));
		}
	} catch (const pt::ptree_error&) {
		return {};
	}
	// One entry for each height. Put more polygons of one height into the "polygons" list of that entry.
	// Two entries for one height are an error in the file: they can have different types, and a reader that
	// keeps only one of them loses a part of the toolhead.
	for (size_t i = 0; i < slices.size(); ++i)
		for (size_t j = i + 1; j < slices.size(); ++j)
			if (slices[i].height == slices[j].height) {
				SPDLOG_ERROR("geometries.json: printer model {} has two slices at height {} mm. The gantry data is not used.",
				             hw_model.model, unscaled(slices[i].height));
				return {};
			}
	return slices;
}

Sequential::PrinterGeometry get_printer_geometry(const PrintConfigView& config)
{
	BuildVolume bv(config.get<std::vector<Vec2d>>("bed_shape"), 10.);
	const BoundingBox& bb = bv.bounding_box();
	Polygon bed_polygon;
	if (bv.type() == BuildVolume::Type::Circle) {
		// Generate an inscribed octagon.
		double r = Algorithms::BoundingBox::sizes(bv.bounding_volume2d()).x() / 2.;
		for (double a = 2*M_PI; a > 0.1; a -= M_PI/4.)
			bed_polygon.points.emplace_back(scaled(Vec2d(r * std::sin(a), r * std::cos(a))));
	} else {
		// Rectangle or custom. Use the bounding box.
		bed_polygon = Algorithms::BoundingBox::to_polygon(bb);
	}

	std::vector<ExtruderSlice> slices = read_gantry_slices(config.hw_config().model);
	if (slices.empty()) {
		// Fallback to a primitive model from the clearance radius and height.
		coord_t r = scaled(std::max(0.1, config.get<double>("extruder_clearance_radius")));
		coord_t h = scaled(std::max(0.1, config.get<double>("extruder_clearance_height")));
		double bed_x = Algorithms::BoundingBox::sizes(bv.bounding_volume2d()).x();
		slices.push_back(ExtruderSlice{ 0, ShapeType::Convex, { { {  -5000000,   -5000000 }, {   5000000,   -5000000 }, {   5000000,   5000000 }, {  -5000000,   5000000 } } } });
		slices.push_back(ExtruderSlice{ 1000000, ShapeType::Box, { { {  -r, -r }, { r, -r }, {   r,   r }, {  -r,  r } } } });
		slices.push_back(ExtruderSlice{ h, ShapeType::Box, { { { -scaled(bed_x),  -r }, { scaled(bed_x),  -r }, { scaled(bed_x), r }, { -scaled(bed_x), r}}} });
	}

	// Convert the data so that libseqarrange can use them.
	Sequential::PrinterGeometry out;
	out.plate = bed_polygon;
	for (const ExtruderSlice& slice : slices) {
		(slice.shape_type == ShapeType::Convex ? out.convex_heights : out.box_heights).emplace(slice.height);
		out.extruder_slices.emplace(slice.height, slice.polygons); // read_gantry_slices() rejects two slices at one height
	}
	return out;
}

// Collects the printable instances in model order, with the projections of their convex hulls
// above each height of the printer geometry.
std::vector<Sequential::ObjectToPrint> get_objects_to_print(const Domain::Model& model, const Sequential::PrinterGeometry& printer_geometry)
{
	std::vector<double> heights;
	for (const auto& [height, pgns] : printer_geometry.extruder_slices)
		heights.push_back(unscaled(height));
	Slic3r::sort_remove_duplicates(heights);

	namespace TriMesh = Biz::Algorithms::TriangleMesh;
	using Biz::Algorithms::BoundingBox::sizes;

	std::vector<Sequential::ObjectToPrint> objects;
	for (const Domain::ModelObject* mo : model.objects) {
		const Domain::TriangleMesh& raw_mesh = mo->raw_mesh();
		for (const Domain::ModelInstance* mi : mo->instances) {
			if (! mi->printable)
				continue;
			const coord_t height = scaled(sizes(Algorithms::ModelObject::instance_bounding_box(*mo, *mi)).z());
			Sequential::ObjectToPrint& otp = objects.emplace_back(Sequential::ObjectToPrint{int(mi->id().id), false, height, {}});
			for (double h : heights) {
				// The bed is at zero level. The instance offset in Z is subtracted so that the hull is
				// taken above the given height over the bed.
				Polygon pgn = TriMesh::its_convex_hull_2d_above(raw_mesh.its, mi->get_matrix_no_offset().cast<float>(), h - mi->get_offset().z());
				otp.pgns_at_height.emplace_back(scaled(h), std::move(pgn));
			}
		}
	}
	return objects;
}

// The printable instances in model order, at their positions.
Sequential::ScheduledPlate get_plate(const Domain::Model& model)
{
	Sequential::ScheduledPlate plate;
	for (const Domain::ModelObject* mo : model.objects)
		for (const Domain::ModelInstance* mi : mo->instances)
			if (mi->printable)
				plate.scheduled_objects.emplace_back(int(mi->id().id), scaled(mi->get_offset().x()), scaled(mi->get_offset().y()));
	return plate;
}

} // namespace

std::optional<std::vector<std::string>> find_seq_print_order(const Domain::Model& model, const PrintConfigView& config)
{
	Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	if (printer_geometry.extruder_slices.empty())
		return std::nullopt;
	Sequential::SolverConfiguration solver_config(printer_geometry);
	std::vector<Sequential::ObjectToPrint> objects = get_objects_to_print(model, printer_geometry);
	const std::optional<std::vector<int>> order =
		Sequential::find_ObjectOrderForSequentialPrint(solver_config, printer_geometry, objects, get_plate(model));
	if (! order)
		return std::nullopt;

	std::vector<std::string> names;
	for (int id : *order)
		for (const Domain::ModelObject* mo : model.objects)
			for (size_t k = 0; k < mo->instances.size(); ++k)
				if (int(mo->instances[k]->id().id) == id)
					names.push_back(mo->instances.size() > 1 ? mo->name + " (" + std::to_string(k + 1) + ")" : mo->name);
	return names;
}

std::optional<std::pair<std::string, std::string>> check_seq_conflict(const Domain::Model& model, const PrintConfigView& config)
{
	Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	if (printer_geometry.extruder_slices.empty())
		return std::nullopt;

	Sequential::SolverConfiguration solver_config(printer_geometry);
	std::vector<Sequential::ObjectToPrint> objects = get_objects_to_print(model, printer_geometry);
	if (objects.size() < 2)
		return std::nullopt;

	// The print order is the model order (see sort_object_instances_by_model_order() in GCode.cpp).
	const Sequential::ScheduledPlate plate = get_plate(model);

	std::optional<std::pair<int, int>> conflict = Sequential::check_ScheduledObjectsForSequentialConflict(
		solver_config, printer_geometry, objects, std::vector<Sequential::ScheduledPlate>(1, plate));
	if (! conflict)
		return std::nullopt;

	std::pair<std::string, std::string> names;
	for (const Domain::ModelObject* mo : model.objects)
		for (const Domain::ModelInstance* mi : mo->instances) {
			if (int(mi->id().id) == conflict->first)
				names.first = mo->name;
			if (int(mi->id().id) == conflict->second)
				names.second = mo->name;
		}
	return names;
}

} // namespace Slic3r
