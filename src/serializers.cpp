#include "moult/core/serializers.hpp"
#include "moult/core/json.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace moult::core {

static bool write_file(const std::filesystem::path& path, std::string_view text, std::string& error) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error = "failed to open " + path.string();
        return false;
    }
    out << text;
    if (!out) {
        error = "failed to write " + path.string();
        return false;
    }
    return true;
}

std::string sarif_from_plan(const Plan& plan) {
    // Minimal SARIF-like output. Enough for CI ingestion scaffolding; a production adapter
    // can expand rules, locations, and fingerprints.
    std::ostringstream results;
    results << "[";
    bool first = true;
    for (const auto& finding : plan.findings) {
        if (!first) results << ",";
        first = false;
        std::ostringstream loc;
        if (finding.range) {
            JsonObjectWriter phys(loc);
            phys.string_field("uri", finding.range->file);
            phys.raw_field("region", "{\"startColumn\":" + std::to_string(finding.range->begin + 1) + "}");
            phys.finish();
        }
        std::ostringstream result;
        JsonObjectWriter obj(result);
        obj.string_field("ruleId", finding.rule_id);
        obj.string_field("level", finding.severity == Severity::Error ? "error" : finding.severity == Severity::Warning ? "warning" : "note");
        obj.raw_field("message", "{\"text\":" + std::string("\"") + json_escape(finding.message) + "\"}");
        if (finding.range) {
            obj.raw_field("locations", "[{\"physicalLocation\":" + loc.str() + "}]");
        }
        obj.finish();
        results << result.str();
    }
    results << "]";

    std::ostringstream run;
    JsonObjectWriter run_obj(run);
    run_obj.raw_field("tool", "{\"driver\":{\"name\":\"moult-core\",\"informationUri\":\"https://example.invalid/moult-core\",\"rules\":[]}} ");
    run_obj.raw_field("results", results.str());
    run_obj.finish();

    std::ostringstream os;
    JsonObjectWriter root(os);
    root.string_field("version", "2.1.0");
    root.string_field("$schema", "https://json.schemastore.org/sarif-2.1.0.json");
    root.raw_field("runs", "[" + run.str() + "]");
    root.finish();
    return os.str();
}

WriteOutputsResult write_outputs(const EngineRunResult& result,
                                  const std::filesystem::path& output_dir,
                                  bool include_facts_in_plan) {
    WriteOutputsResult out;
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        out.ok = false;
        out.errors.push_back("failed to create output directory: " + ec.message());
        return out;
    }

    const auto plan_path = output_dir / "plan.json";
    const auto evidence_path = output_dir / "evidence.jsonl";
    const auto facts_path = output_dir / "facts.json";
    const auto sarif_path = output_dir / "findings.sarif";

    std::string error;
    if (write_file(plan_path, plan_to_json(result.plan, include_facts_in_plan ? &result.facts : nullptr), error)) {
        out.written_files.push_back(plan_path.string());
    } else {
        out.ok = false;
        out.errors.push_back(error);
    }
    if (write_file(evidence_path, evidence_to_jsonl(result.plan.evidence), error)) {
        out.written_files.push_back(evidence_path.string());
    } else {
        out.ok = false;
        out.errors.push_back(error);
    }
    if (write_file(facts_path, facts_to_json_array(result.facts.all()), error)) {
        out.written_files.push_back(facts_path.string());
    } else {
        out.ok = false;
        out.errors.push_back(error);
    }
    if (write_file(sarif_path, sarif_from_plan(result.plan), error)) {
        out.written_files.push_back(sarif_path.string());
    } else {
        out.ok = false;
        out.errors.push_back(error);
    }
    return out;
}

} // namespace moult::core
