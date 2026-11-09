#include "ArrangeHelper.hpp"
#include "libslic3r/SeqArrange.hpp"

#include "libseqarrange/seq_interface.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "Slic3r/Biz/Algorithms/BoundingBox.hpp"
#include "Slic3r/Biz/Algorithms/ClipperUtils.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Biz/Algorithms/TriangleMesh.hpp"
#include "Slic3r/Directories.hpp"
#include "Slic3r/Domain/ConfigDefsFDM.hpp"
#include "Slic3r/Domain/Model.hpp"
#include "Slic3r/Domain/Percentage.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
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
std::vector<ExtruderSlice> read_gantry_slices_from_file(const Domain::Preset::HwModel& hw_model)
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

// read_gantry_slices_from_file() with a cache. One Print::process() needs the slices up to 3 times (the check, the
// suggested order, the skirt), and each arrange needs them too. The key has the resources directory: a test can
// change it.
std::vector<ExtruderSlice> read_gantry_slices(const Domain::Preset::HwModel& hw_model)
{
	static std::mutex mutex;
	static std::map<std::pair<std::string, std::string>, std::vector<ExtruderSlice>> cache;
	const std::pair<std::string, std::string> key{resources_dir(), hw_model.model};
	std::lock_guard<std::mutex> lock(mutex);
	auto it = cache.find(key);
	if (it == cache.end())
		it = cache.emplace(key, read_gantry_slices_from_file(hw_model)).first;
	return it->second;
}

Sequential::PrinterGeometry get_printer_geometry(const Domain::ConfigView& config)
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
		// read_gantry_slices() rejects two slices at one height. The fallback has two at 1 mm if
		// extruder_clearance_height is 1 mm: keep the polygons of both.
		std::vector<Polygon>& polygons = out.extruder_slices[slice.height];
		polygons.insert(polygons.end(), slice.polygons.begin(), slice.polygons.end());
	}
	return out;
}

// The convex hull of a polygon grown by a distance (mm).
Polygon grow_convex(const Polygon& pgn, double distance)
{
	if (distance <= 0. || pgn.points.empty())
		return pgn;
	Points grown;
	for (const Polygon& p : Algorithms::ClipperUtils::offset(pgn, float(scaled(distance))))
		grown.insert(grown.end(), p.points.begin(), p.points.end());
	return Slic3r::Geometry::convex_hull(grown);
}

// The heights of the slices of the printer geometry (mm), sorted.
std::vector<double> slice_heights(const Sequential::PrinterGeometry& printer_geometry)
{
	std::vector<double> heights;
	for (const auto& [height, pgns] : printer_geometry.extruder_slices)
		heights.push_back(unscaled(height));
	Slic3r::sort_remove_duplicates(heights);
	return heights;
}

// An instance with the projections of its convex hull above each height, relative to the instance offset.
// footprint_growth (mm) grows the polygon at the bed (for example by the brim).
Sequential::ObjectToPrint make_object_to_print(const Domain::ModelObject& mo, const Domain::ModelInstance& mi, const std::vector<double>& heights,
                                               const SeqRaft& raft, double footprint_growth = 0., const SeqSupportLayers* supports = nullptr,
                                               double growth = 0.)
{
	namespace TriMesh = Biz::Algorithms::TriangleMesh;
	using Biz::Algorithms::BoundingBox::sizes;

	const Domain::TriangleMesh& raw_mesh = mo.raw_mesh();
	const coord_t height = scaled(sizes(Algorithms::ModelObject::instance_bounding_box(mo, mi)).z() + raft.z_lift);
	Sequential::ObjectToPrint otp{int(mi.id().id), false, height, {}};
	for (double h : heights) {
		// The bed is at zero level. The instance offset in Z is subtracted so that the hull is
		// taken above the given height over the bed.
		Polygon pgn;
		if (raft.z_lift > 0. && h <= raft.z_lift) {
			// The raft is here, and the nozzle prints it: the footprint grown by the raft expansion.
			pgn = grow_convex(TriMesh::its_convex_hull_2d_above(raw_mesh.its, mi.get_matrix_no_offset().cast<float>(), - mi.get_offset().z()),
			                  raft.xy_expansion);
		} else {
			pgn = TriMesh::its_convex_hull_2d_above(raw_mesh.its, mi.get_matrix_no_offset().cast<float>(), h - raft.z_lift - mi.get_offset().z());
		}
		pgn = grow_convex(pgn, h == 0. ? std::max(footprint_growth, growth) : growth);
		// The support layers above h: the toolhead slice at h is hit by them too.
		if (supports != nullptr) {
			Points points = pgn.points;
			for (const auto& [print_z, hull] : *supports)
				if (print_z > h)
					points.insert(points.end(), hull.points.begin(), hull.points.end());
			if (points.size() > pgn.points.size())
				pgn = Slic3r::Geometry::convex_hull(points);
		}
		otp.pgns_at_height.emplace_back(scaled(h), std::move(pgn));
	}
	return otp;
}

// Collects the printable instances in model order, with the projections of their convex hulls
// above each height of the printer geometry.
std::vector<Sequential::ObjectToPrint> get_objects_to_print(const Domain::Model& model, const Sequential::PrinterGeometry& printer_geometry, const SeqRafts& rafts,
                                                            const SeqSupports& supports)
{
	const std::vector<double> heights = slice_heights(printer_geometry);
	std::vector<Sequential::ObjectToPrint> objects;
	for (const Domain::ModelObject* mo : model.objects) {
		for (const Domain::ModelInstance* mi : mo->instances) {
			if (! mi->printable)
				continue;
			// A raft lifts the instance, and it is larger than the instance.
			const auto raft_it = rafts.find(mi->id().id);
			const auto support_it = supports.find(mi->id().id);
			objects.push_back(make_object_to_print(*mo, *mi, heights, raft_it == rafts.end() ? SeqRaft{} : raft_it->second, 0.,
			                                       support_it == supports.end() ? nullptr : &support_it->second));
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

// The name of an instance in a message: the object name, and " (n)" if the object has more instances.
std::string instance_name(const Domain::Model& model, int instance_id)
{
	for (const Domain::ModelObject* mo : model.objects)
		for (size_t k = 0; k < mo->instances.size(); ++k)
			if (int(mo->instances[k]->id().id) == instance_id)
				return mo->instances.size() > 1 ? mo->name + " (" + std::to_string(k + 1) + ")" : mo->name;
	return {};
}

} // namespace

std::optional<std::vector<std::string>> find_seq_print_order(const Domain::Model& model, const PrintConfigView& config, const SeqRafts& rafts, const SeqSupports& supports)
{
	Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	if (printer_geometry.extruder_slices.empty())
		return std::nullopt;
	Sequential::SolverConfiguration solver_config(printer_geometry);
	std::vector<Sequential::ObjectToPrint> objects = get_objects_to_print(model, printer_geometry, rafts, supports);
	// The user can change the order of the objects, not of the instances inside an object, and the instances of
	// an object print one after the other (sort_object_instances_by_model_order()). Thus each object is a group.
	std::vector<int> groups;
	for (size_t i = 0; i < model.objects.size(); ++i)
		for (const Domain::ModelInstance* mi : model.objects[i]->instances)
			if (mi->printable)
				groups.push_back(int(i));
	const std::optional<std::vector<int>> order =
		Sequential::find_ObjectOrderForSequentialPrint(solver_config, printer_geometry, objects, get_plate(model), nullptr, groups);
	if (! order)
		return std::nullopt;

	std::vector<std::string> names;
	for (int id : *order)
		names.push_back(instance_name(model, id));
	return names;
}

std::optional<std::pair<std::string, std::string>> check_seq_conflict(const Domain::Model& model, const PrintConfigView& config, const SeqRafts& rafts, const SeqSupports& supports)
{
	Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	if (printer_geometry.extruder_slices.empty())
		return std::nullopt;

	Sequential::SolverConfiguration solver_config(printer_geometry);
	std::vector<Sequential::ObjectToPrint> objects = get_objects_to_print(model, printer_geometry, rafts, supports);
	if (objects.size() < 2)
		return std::nullopt;

	// The print order is the model order (see sort_object_instances_by_model_order() in GCode.cpp).
	const Sequential::ScheduledPlate plate = get_plate(model);

	std::optional<std::pair<int, int>> conflict = Sequential::check_ScheduledObjectsForSequentialConflict(
		solver_config, printer_geometry, objects, std::vector<Sequential::ScheduledPlate>(1, plate));
	if (! conflict)
		return std::nullopt;

	return std::make_pair(instance_name(model, conflict->first), instance_name(model, conflict->second));
}

double seq_skirt_clearance(const PrintConfigView& config)
{
	// The fallback (extruder_clearance_radius and _height) does not know the real toolhead.
	if (read_gantry_slices(config.hw_config().model).empty())
		return 0.;
	const Sequential::PrinterGeometry printer_geometry = get_printer_geometry(config);
	double lowest = 0.;
	for (const auto& [height, polygons] : printer_geometry.extruder_slices)
		if (height > 0 && (lowest == 0. || unscaled(height) < lowest))
			lowest = unscaled(height);
	return lowest;
}

struct SeqArrange::Data
{
	Sequential::PrinterGeometry printer_geometry;
	std::vector<Sequential::ObjectToPrint> objects;
};

namespace {

// The brim of an object (mm): the print config, with the overrides of the object.
double seq_object_brim(const Domain::ModelObject& mo, const Domain::ConfigView& config)
{
	Domain::BrimType type = config.get<Domain::BrimType>("brim_type");
	double width = config.get<double>("brim_width");
	if (const auto t = mo.object_settings.overrides.get("brim_type"))
		type = t->get<Domain::BrimType>();
	if (const auto w = mo.object_settings.overrides.get("brim_width"))
		width = w->get<double>();
	return type == Domain::BrimType::OuterOnly || type == Domain::BrimType::OuterAndInner ? width : 0.;
}

// The raft of the print config, before the slicing. Print::process() knows the exact lift (SeqRaft in Print.cpp). This
// is an estimate from the layer heights. The overrides of an object are not used.
SeqRaft seq_config_raft(const Domain::ConfigView& config)
{
	const int layers = config.get<int>("raft_layers");
	if (layers <= 0)
		return {};
	const double first_layer = config.get<Domain::FloatOrPercentage>("first_layer_height").get_abs_value(1.0);
	return SeqRaft{
		.z_lift = first_layer + (layers - 1) * config.get<double>("layer_height") + config.get<double>("raft_contact_distance"),
		.xy_expansion = config.get<double>("raft_expansion") + config.get<double>("raft_first_layer_expansion")
	};
}

std::vector<SeqArrange::Plate> to_plates(const std::vector<Sequential::ScheduledPlate>& plates)
{
	std::vector<SeqArrange::Plate> out;
	for (const Sequential::ScheduledPlate& plate : plates) {
		SeqArrange::Plate& p = out.emplace_back();
		for (const Sequential::ScheduledObject& o : plate.scheduled_objects)
			p.push_back(SeqArrange::Placement{size_t(o.id), unscaled(o.x), unscaled(o.y)});
	}
	return out;
}

} // namespace

SeqArrange::SeqArrange(const Domain::Model& model, const Domain::ConfigView& config, const std::vector<size_t>& instance_ids)
{
	auto data = std::make_shared<Data>();
	data->printer_geometry = get_printer_geometry(config);
	const std::vector<double> heights = slice_heights(data->printer_geometry);
	const SeqRaft raft = seq_config_raft(config);
	// The arrange runs before the slicing, so the supports are not known. Automatic supports can reach outside the
	// object: measured with grid, snug and organic supports, up to 5 mm at the bed (base, support brim) and 2.1 mm
	// above. If automatic supports are on, all shapes grow by these distances. Print checks the real supports later.
	const bool auto_supports = config.get<Domain::SupportMode>("support_material") == Domain::SupportMode::Everywhere;
	const double support_footprint = auto_supports ? 5. : 0.;
	const double support_growth = auto_supports ? 2.5 : 0.;
	const BoundingBox bed = get_extents(data->printer_geometry.plate);
	const std::set<size_t> wanted(instance_ids.begin(), instance_ids.end());

	for (const Domain::ModelObject* mo : model.objects) {
		const double brim = seq_object_brim(*mo, config);
		const size_t first = data->objects.size();
		for (const Domain::ModelInstance* mi : mo->instances) {
			if (! mi->printable || ! wanted.count(mi->id().id))
				continue;
			Sequential::ObjectToPrint otp = make_object_to_print(*mo, *mi, heights, raft, std::max({brim, raft.xy_expansion, support_footprint}),
			                                                     nullptr, support_growth);
			const BoundingBox footprint = otp.pgns_at_height.empty() ? BoundingBox() : get_extents(otp.pgns_at_height.front().second);
			if (! footprint.defined || Algorithms::BoundingBox::sizes(footprint).x() >= Algorithms::BoundingBox::sizes(bed).x() ||
			    Algorithms::BoundingBox::sizes(footprint).y() >= Algorithms::BoundingBox::sizes(bed).y()) {
				m_not_arranged.push_back(mi->id().id);
				continue;
			}
			data->objects.push_back(std::move(otp));
		}
		// The instances of one object print one after the other (sort_object_instances_by_model_order() in GCode.cpp).
		// Thus they must be on one plate, one after the other.
		for (size_t i = first; i + 1 < data->objects.size(); ++i)
			data->objects[i].glued_to_next = true;
	}
	m_data = std::move(data);
}

SeqArrange::~SeqArrange() = default;

bool SeqArrange::empty() const
{
	return m_data->objects.empty();
}

std::vector<SeqArrange::Plate> SeqArrange::process(const std::function<void(int)>& progress) const
{
	if (empty())
		return {};

	// Measured with seqarrange_bench on 89 cases (2026-11-09):
	// - The greedy arrange (no z3) needs 145 plates in 1.5 s for all cases. The solver with group sizes 2 and 8
	//   needs 155 plates in 362 s.
	// - After the greedy arrange, the solver with group size 8 saves 3 more plates. It can save a plate only if the
	//   greedy result has more than one plate: then it runs, 7 s mean and 18 s at most.
	// - If the greedy arrange fails (a glued unit is larger than its arrange on one plate), the solver runs with
	//   group sizes 2 and 8.
	// The z3 resource limit gives the same result on each machine. The wall-time timeout stays as a limit for a
	// very slow machine.
	const auto first_count = [](const std::vector<Sequential::ScheduledPlate>& p) { return p.empty() ? size_t(0) : p.front().scheduled_objects.size(); };
	std::vector<Sequential::ScheduledPlate> best;
	const auto offer = [&](std::vector<Sequential::ScheduledPlate>&& plates, const Sequential::SolverConfiguration& solver_config, const char* name) {
		// Each result must pass the geometric check (the solver had an error with this before, see the overflow fix
		// in Rational).
		if (Sequential::check_ScheduledObjectsForSequentialConflict(solver_config, m_data->printer_geometry, m_data->objects, plates)) {
			SPDLOG_ERROR("Sequential arrange, {}: the result has a collision. It is not used.", name);
			return;
		}
		if (best.empty() || plates.size() < best.size() || (plates.size() == best.size() && first_count(plates) > first_count(best)))
			best = std::move(plates);
	};

	{
		const Sequential::SolverConfiguration solver_config(m_data->printer_geometry);
		try {
			offer(Sequential::schedule_ObjectsForSequentialPrintGreedy(solver_config, m_data->printer_geometry, m_data->objects), solver_config, "greedy");
		} catch (const std::runtime_error& e) {
			SPDLOG_INFO("Sequential arrange, greedy: {}", e.what());
		}
	}
	if (best.size() == 1) {
		// One plate: the solver cannot do better.
		progress(100);
		return to_plates(best);
	}
	progress(5);

	struct Member { int group_size; int progress_from; int progress_to; };
	const std::vector<Member> members = best.empty() ? std::vector<Member>{{2, 5, 20}, {8, 20, 100}} : std::vector<Member>{{8, 5, 100}};
	for (const Member& m : members) {
		Sequential::SolverConfiguration solver_config(m_data->printer_geometry);
		solver_config.set_ObjectGroupSize(m.group_size);
		solver_config.optimization_rlimit = "2000000";
		const auto member_progress = [&](int p) {
			progress(m.progress_from + (m.progress_to - m.progress_from) * std::clamp(p, 0, 100) / 100);
		};
		try {
			offer(Sequential::schedule_ObjectsForSequentialPrint(solver_config, m_data->printer_geometry, m_data->objects, member_progress),
			      solver_config, m.group_size == 2 ? "group size 2" : "group size 8");
		} catch (const std::runtime_error& e) {
			// The solver throws if it cannot place an object. Another member can still succeed.
			SPDLOG_WARN("Sequential arrange, group size {}: {}", m.group_size, e.what());
		}
	}
	progress(100);
	return to_plates(best);
}

} // namespace Slic3r
