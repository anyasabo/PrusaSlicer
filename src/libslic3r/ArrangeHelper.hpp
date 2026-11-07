#ifndef libslic3r_Arrange_Helper_hpp
#define libslic3r_Arrange_Helper_hpp

#include "libseqarrange/seq_interface.hpp"



namespace Slic3r {

	class Model;
	class ConfigBase;

	class ExceptionCannotAttemptSeqArrange : public std::exception {};
	class ExceptionCannotApplySeqArrange : public std::exception {};

	void arrange_model_sequential(Model& model, const ConfigBase& config);

	std::optional<std::pair<std::string, std::string>> check_seq_conflict(const Model& model, const ConfigBase& config);

	// Building blocks of SeqArrange, exposed for tests and benchmarks so they
	// can run the scheduler with a modified SolverConfiguration and inspect
	// the resulting plates without going through a Model roundtrip.
	Sequential::PrinterGeometry get_printer_geometry(const ConfigBase& config);
	Sequential::SolverConfiguration get_solver_config(const Sequential::PrinterGeometry& printer_geometry);
	std::vector<Sequential::ObjectToPrint> get_objects_to_print(const Model& model, const Sequential::PrinterGeometry& printer_geometry, int selected_bed = -1);

	// This is just a helper class to collect data for seq. arrangement, running the arrangement
	// and applying the results to model. It is here so the processing itself can be offloaded
	// into a separate thread without copying the Model or sharing it with UI thread.
	class SeqArrange {
	public:
		explicit SeqArrange(const Model& model, const ConfigBase& config, bool current_bed_only);
		void process_seq_arrange(std::function<void(int)> progress_fn);
		void apply_seq_arrange(Model& model) const;

		// Result of process_seq_arrange, one entry per bed.
		const std::vector<Sequential::ScheduledPlate>& get_plates() const { return m_plates; }

	private:
		// Following three are inputs, filled in by the constructor.
		Sequential::PrinterGeometry m_printer_geometry;
		Sequential::SolverConfiguration m_solver_configuration;
		std::vector<Sequential::ObjectToPrint> m_objects;
		int m_selected_bed = -1;

		// This is the output, filled in by process_seq_arrange.
		std::vector<Sequential::ScheduledPlate> m_plates;
	};
	
}

#endif // slic3r_Arrange_Helper_hpp
