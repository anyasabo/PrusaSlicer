// Benchmark and quality check of the sequential arrange (libseqarrange).
//
// Reads the cases of a corpus, arranges each case for each printer with schedule_ObjectsForSequentialPrint(),
// checks the result and writes one JSON line for each case and printer to stdout.
//
// Corpus file format: the text export of the arrange input (see load_exported_data_from_text()), with these
// extra lines. The loader ignores lines that start with '#'.
//   #NAME <name>             the case name (default: the file name)
//   #PRINTERS <model> ...    the printers of the case (default: all printers of the bench)
//   #GLUED                   the object above this line is glued to the next object (instances of one object)
// An object can have polygons at the heights of one printer (an export from the slicer), or only a polygon at
// height 0 (a footprint). The bench extrudes a footprint to TOTAL_HEIGHT for each printer. A case with an
// object with other heights is skipped for that printer.
//
// Each result must pass check_ScheduledObjectsForSequentialConflict() (no z3), and each object must be
// inside the bed. If not, the line has "ok": false, and the exit code is 1.
//
// Usage: seqarrange_bench --geometries geometries.json [--printers A,B] [--rlimit N] [--timeout MS]
//                         [--repeat N] [--decimation low|high|none] [--no-fork] FILE_OR_DIR ...
// Each run is in a child process (see run()). --no-fork runs all in this process, for a profiler.
// --decimation: the tolerance of the polygon decimation. "low" (0.15 mm) is the default of SolverConfiguration,
// "high" is 0.65 mm. The names are from DecimationPrecision.

#include "libseqarrange/seq_interface.hpp"
#include "seq_defs.hpp"
#include "seq_preprocess.hpp"
#include "seq_sequential.hpp"
#include "seq_utilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string/replace.hpp>
#include <boost/property_tree/json_parser.hpp>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace Sequential;
namespace fs = std::filesystem;

namespace {

// Bed sizes in mm, from resources/presets/prusa-research-fff/PrusaResearch/preset-printer-*.yaml.
const std::map<std::string, std::pair<int, int>> bed_sizes{
    {"MK4S", {250, 210}},  {"COREONE", {250, 220}}, {"COREONEL", {300, 300}},
    {"COREONE_INDX", {248, 205}}, {"XL", {360, 360}}, {"MINI", {180, 180}},
};

const std::vector<std::string> default_printers{"MK4S", "COREONE", "COREONEL", "COREONE_INDX", "XL", "MINI"};

coord_t scaled_mm(double mm) { return coord_t(mm * 1000000.); }

// Reads the slices of a printer model from geometries.json, as ArrangeHelper.cpp does. Two slices at one
// height are an error, as in ArrangeHelper.cpp.
std::optional<PrinterGeometry> read_printer_geometry(const boost::property_tree::ptree& tree, const std::string& model)
{
    const auto bed = bed_sizes.find(model);
    if (bed == bed_sizes.end())
        return std::nullopt;
    for (const auto& printer : tree.get_child("printers")) {
        bool found = false;
        for (const auto& name : printer.second.get_child("printer_models"))
            found |= name.second.data() == model;
        if (! found)
            continue;
        PrinterGeometry out;
        const coord_t x = scaled_mm(bed->second.first);
        const coord_t y = scaled_mm(bed->second.second);
        out.plate = Polygon{{0, 0}, {x, 0}, {x, y}, {0, y}};
        for (const auto& slice : printer.second.get_child("slices")) {
            const coord_t height = scaled_mm(slice.second.get<double>("height"));
            if (out.extruder_slices.count(height) != 0) {
                std::cerr << model << ": two slices at height " << slice.second.get<std::string>("height") << " mm\n";
                return std::nullopt;
            }
            const bool convex = slice.second.get<std::string>("type") != "box";
            (convex ? out.convex_heights : out.box_heights).insert(height);
            std::vector<Polygon>& polygons = out.extruder_slices[height];
            for (const auto& polygon : slice.second.get_child("polygons")) {
                std::string text = polygon.second.data();
                boost::replace_all(text, ";", " ");
                boost::replace_all(text, ",", " ");
                std::istringstream ss(text);
                Polygon pgn;
                double px = 0., py = 0.;
                while (ss >> px >> py)
                    pgn.points.emplace_back(scaled_mm(px), scaled_mm(py));
                polygons.push_back(pgn);
            }
        }
        return out;
    }
    return std::nullopt;
}

struct Case
{
    std::string name;
    std::string file;
    std::vector<std::string> printers;
    std::vector<ObjectToPrint> objects;
};

Case read_case(const fs::path& path)
{
    std::ifstream in(path);
    if (! in)
        throw std::runtime_error("cannot read " + path.string());
    Case c;
    c.name = path.stem().string();
    c.file = path.string();
    std::string data;
    std::vector<int> glued; // indices of glued objects
    int objects = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("#NAME ", 0) == 0)
            c.name = line.substr(6);
        else if (line.rfind("#PRINTERS ", 0) == 0) {
            std::istringstream ss(line.substr(10));
            std::string p;
            while (ss >> p)
                c.printers.push_back(p);
        } else if (line.rfind("#GLUED", 0) == 0)
            glued.push_back(objects - 1);
        else if (line.rfind("#", 0) == 0)
            continue;
        else {
            if (line.find("OBJECT_ID") != std::string::npos)
                ++objects;
            data += line + "\n";
        }
    }
    c.objects = load_exported_data_from_text(data);
    for (int i : glued)
        if (i >= 0 && i < int(c.objects.size()))
            c.objects[i].glued_to_next = true;
    return c;
}

// Returns the objects with polygons at the heights of the printer, or nullopt if an object has other heights.
std::optional<std::vector<ObjectToPrint>> objects_for_printer(const std::vector<ObjectToPrint>& objects, const PrinterGeometry& printer)
{
    std::set<coord_t> heights;
    for (const auto& [h, pgns] : printer.extruder_slices)
        heights.insert(h);
    std::vector<ObjectToPrint> out;
    for (const ObjectToPrint& o : objects) {
        std::set<coord_t> own;
        for (const auto& [h, pgn] : o.pgns_at_height)
            own.insert(h);
        if (own == heights) {
            out.push_back(o);
            continue;
        }
        if (own != std::set<coord_t>{0})
            return std::nullopt;
        ObjectToPrint e = o;
        e.pgns_at_height.clear();
        const Polygon& footprint = o.pgns_at_height.front().second;
        for (coord_t h : heights)
            e.pgns_at_height.emplace_back(h, h == 0 || h < o.total_height ? footprint : Polygon());
        out.push_back(e);
    }
    return out;
}

struct Result
{
    std::vector<ScheduledPlate> plates;
    double wall_ms = 0.;
    SolverStatistics stats;
    std::string error;
};

Result run_here(const SolverConfiguration& config, const PrinterGeometry& printer, const std::vector<ObjectToPrint>& objects)
{
    Result r;
    reset_SolverStatistics();
    const auto t0 = std::chrono::steady_clock::now();
    try {
        r.plates = schedule_ObjectsForSequentialPrint(config, printer, objects);
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    r.wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    r.stats = get_SolverStatistics();
    return r;
}

// The result as text, one item for each line: the error, the time, the statistics, then one line for each plate.
std::string serialize(const Result& r)
{
    std::ostringstream ss;
    ss.precision(17);
    ss << r.error << "\n" << r.wall_ms << " " << r.stats.checks << " " << r.stats.sat << " " << r.stats.unsat << " "
       << r.stats.unknown << "\n";
    for (const ScheduledPlate& p : r.plates) {
        for (const ScheduledObject& o : p.scheduled_objects)
            ss << o.id << " " << o.x << " " << o.y << " ";
        ss << "\n";
    }
    return ss.str();
}

Result deserialize(const std::string& text)
{
    Result r;
    std::istringstream ss(text);
    std::getline(ss, r.error);
    std::string line;
    std::getline(ss, line);
    std::istringstream(line) >> r.wall_ms >> r.stats.checks >> r.stats.sat >> r.stats.unsat >> r.stats.unknown;
    while (std::getline(ss, line)) {
        ScheduledPlate p;
        std::istringstream ls(line);
        int id;
        coord_t x, y;
        while (ls >> id >> x >> y)
            p.scheduled_objects.emplace_back(id, x, y);
        r.plates.push_back(p);
    }
    return r;
}

// Runs the arrange in a child process. z3 keeps state in the process between solver calls (Z3_reset_memory() does
// not remove it), and the heap layout changes the search order of z3. Thus the second run of a case in one process
// can give a different layout than the first run. In a new process, the same input always gives the same result.
Result run(const SolverConfiguration& config, const PrinterGeometry& printer, const std::vector<ObjectToPrint>& objects,
           bool in_child)
{
#ifndef _WIN32
    if (in_child) {
        int fds[2];
        if (pipe(fds) != 0)
            throw std::runtime_error("pipe failed");
        std::cout.flush();
        const pid_t pid = fork();
        if (pid < 0)
            throw std::runtime_error("fork failed");
        if (pid == 0) {
            close(fds[0]);
            const std::string text = serialize(run_here(config, printer, objects));
            size_t done = 0;
            while (done < text.size()) {
                const ssize_t n = write(fds[1], text.data() + done, text.size() - done);
                if (n <= 0)
                    _exit(1);
                done += size_t(n);
            }
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);
        std::string text;
        char buf[4096];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0)
            text.append(buf, size_t(n));
        close(fds[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        if (! WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            Result r;
            r.error = "child process failed";
            return r;
        }
        return deserialize(text);
    }
#endif
    return run_here(config, printer, objects);
}

// An axis-aligned box in slicer units.
struct Extents
{
    coord_t min_x = std::numeric_limits<coord_t>::max(), min_y = std::numeric_limits<coord_t>::max();
    coord_t max_x = std::numeric_limits<coord_t>::min(), max_y = std::numeric_limits<coord_t>::min();
    bool empty() const { return min_x > max_x; }
    void add(coord_t x, coord_t y)
    {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    void add(const Polygon& pgn, coord_t dx, coord_t dy)
    {
        for (const Point& p : pgn.points)
            add(p.x() + dx, p.y() + dy);
    }
    double area_mm2() const { return empty() ? 0. : (max_x - min_x) * 1e-6 * ((max_y - min_y) * 1e-6); }
};

std::string layout_key(const std::vector<ScheduledPlate>& plates)
{
    std::ostringstream ss;
    for (const ScheduledPlate& p : plates) {
        ss << "|";
        for (const ScheduledObject& o : p.scheduled_objects)
            ss << o.id << ":" << o.x << "," << o.y << ";";
    }
    return ss.str();
}

std::string json_escape(const std::string& s)
{
    std::string out;
    for (char ch : s) {
        if (ch == '"' || ch == '\\')
            out += '\\';
        if (ch == '\n')
            out += "\\n";
        else
            out += ch;
    }
    return out;
}

void usage()
{
    std::cerr << "Usage: seqarrange_bench --geometries FILE [--printers A,B] [--rlimit N] [--timeout MS] [--repeat N] "
                 "FILE_OR_DIR ...\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::string geometries_file;
    std::vector<std::string> printers = default_printers;
    std::string rlimit = "0";
    std::string timeout;
    int repeat = 1;
    std::string decimation = "low";
    bool in_child = true;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--geometries")
            geometries_file = next();
        else if (a == "--printers") {
            printers.clear();
            std::istringstream ss(next());
            std::string p;
            while (std::getline(ss, p, ','))
                printers.push_back(p);
        } else if (a == "--rlimit")
            rlimit = next();
        else if (a == "--timeout")
            timeout = next();
        else if (a == "--no-fork")
            in_child = false;
        else if (a == "--decimation")
            decimation = next();
        else if (a == "--repeat")
            repeat = std::max(1, std::stoi(next()));
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else
            inputs.emplace_back(a);
    }
    if (geometries_file.empty() || inputs.empty()) {
        usage();
        return 2;
    }

    boost::property_tree::ptree tree;
    boost::property_tree::read_json(geometries_file, tree);

    std::vector<fs::path> files;
    for (const fs::path& in : inputs) {
        if (fs::is_directory(in)) {
            for (const auto& e : fs::directory_iterator(in))
                if (e.is_regular_file() && e.path().extension() == ".txt")
                    files.push_back(e.path());
        } else
            files.push_back(in);
    }
    std::sort(files.begin(), files.end());

    bool all_ok = true;
    for (const fs::path& file : files) {
        const Case c = read_case(file);
        for (const std::string& model : printers) {
            if (! c.printers.empty() && std::find(c.printers.begin(), c.printers.end(), model) == c.printers.end())
                continue;
            const std::optional<PrinterGeometry> printer = read_printer_geometry(tree, model);
            if (! printer) {
                std::cerr << "no geometry for " << model << "\n";
                continue;
            }
            const auto objects = objects_for_printer(c.objects, *printer);
            if (! objects) {
                std::cout << "{\"case\":\"" << json_escape(c.name) << "\",\"printer\":\"" << model
                          << "\",\"skipped\":\"heights\"}" << std::endl;
                continue;
            }

            SolverConfiguration config(*printer);
            config.optimization_rlimit = rlimit;
            if (decimation == "high")
                config.set_DecimationPrecision(SEQ_DECIMATION_PRECISION_HIGH);
            else if (decimation == "none")
                config.set_DecimationPrecision(SEQ_DECIMATION_PRECISION_UNDEFINED);
            if (! timeout.empty())
                config.optimization_timeout = timeout;
            else if (rlimit != "0")
                config.optimization_timeout = SEQ_NO_TIMEOUT;

            std::vector<Result> results;
            for (int k = 0; k < repeat; ++k)
                results.push_back(run(config, *printer, *objects, in_child));
            std::vector<double> times;
            for (const Result& r : results)
                times.push_back(r.wall_ms);
            std::sort(times.begin(), times.end());
            bool deterministic = true;
            for (const Result& r : results)
                deterministic &= layout_key(r.plates) == layout_key(results.front().plates) && r.error == results.front().error;
            const Result& r = results.front();

            // Checks: no conflict, each object on a plate once, each object inside the bed.
            bool ok = r.error.empty();
            std::string why = r.error;
            std::map<int, const ObjectToPrint*> by_id;
            for (const ObjectToPrint& o : *objects)
                by_id[o.id] = &o;
            std::map<int, int> seen;
            Extents bed;
            bed.add(printer->plate, 0, 0);
            std::vector<double> areas;
            for (const ScheduledPlate& p : r.plates) {
                Extents layout;
                for (const ScheduledObject& so : p.scheduled_objects) {
                    ++seen[so.id];
                    const auto it = by_id.find(so.id);
                    if (it == by_id.end()) {
                        ok = false;
                        why = "unknown id";
                        continue;
                    }
                    Extents e;
                    e.add(it->second->pgns_at_height.front().second, so.x, so.y);
                    if (e.min_x < bed.min_x || e.min_y < bed.min_y || e.max_x > bed.max_x || e.max_y > bed.max_y) {
                        ok = false;
                        why = "object " + std::to_string(so.id) + " outside the bed";
                    }
                    layout.add(e.min_x, e.min_y);
                    layout.add(e.max_x, e.max_y);
                }
                areas.push_back(layout.area_mm2());
            }
            if (ok) {
                for (const ObjectToPrint& o : *objects)
                    if (seen[o.id] != 1) {
                        ok = false;
                        why = "object " + std::to_string(o.id) + " placed " + std::to_string(seen[o.id]) + " times";
                    }
                if (auto conflict = check_ScheduledObjectsForSequentialConflict(config, *printer, *objects, r.plates)) {
                    ok = false;
                    why = "conflict " + std::to_string(conflict->first) + " " + std::to_string(conflict->second);
                    // The same check with the exact polygons (no decimation) tells if the conflict is only in the
                    // decimated polygons.
                    SolverConfiguration exact = config;
                    exact.set_DecimationPrecision(SEQ_DECIMATION_PRECISION_UNDEFINED);
                    const auto exact_conflict = check_ScheduledObjectsForSequentialConflict(exact, *printer, *objects, r.plates);
                    why += exact_conflict ? ", also with exact polygons" : ", not with exact polygons";
                    // The same check with only the two objects of the conflict, in the same order and positions.
                    for (const ScheduledPlate& p : r.plates) {
                        ScheduledPlate pair;
                        for (const ScheduledObject& so : p.scheduled_objects)
                            if (so.id == conflict->first || so.id == conflict->second)
                                pair.scheduled_objects.push_back(so);
                        if (pair.scheduled_objects.size() == 2) {
                            const bool pair_conflict = check_ScheduledObjectsForSequentialConflict(config, *printer, *objects, {pair}).has_value();
                            why += pair_conflict ? ", also as a pair alone" : ", not as a pair alone";
                        }
                    }
                }
            }
            all_ok &= ok;

            std::ostringstream js;
            js.setf(std::ios::fixed);
            js.precision(1);
            js << "{\"case\":\"" << json_escape(c.name) << "\",\"printer\":\"" << model << "\",\"objects\":" << objects->size()
               << ",\"ok\":" << (ok ? "true" : "false");
            if (! why.empty())
                js << ",\"why\":\"" << json_escape(why) << "\"";
            js << ",\"plates\":" << r.plates.size() << ",\"plate1\":" << (r.plates.empty() ? 0 : r.plates.front().scheduled_objects.size())
               << ",\"areas_mm2\":[";
            for (size_t i = 0; i < areas.size(); ++i)
                js << (i ? "," : "") << areas[i];
            js << "],\"wall_ms\":" << times[times.size() / 2] << ",\"wall_ms_min\":" << times.front()
               << ",\"checks\":" << r.stats.checks << ",\"sat\":" << r.stats.sat << ",\"unsat\":" << r.stats.unsat
               << ",\"unknown\":" << r.stats.unknown << ",\"rlimit\":\"" << rlimit << "\",\"decimation\":\"" << decimation << "\",\"repeat\":" << repeat
               << ",\"deterministic\":" << (deterministic ? "true" : "false") << ",\"layout\":\"" << layout_key(r.plates) << "\"}";
            std::cout << js.str() << std::endl;
        }
    }
    return all_ok ? 0 : 1;
}
