#include "moult/core/diagnostics.hpp"
#include "moult/core/json.hpp"

#include <algorithm>
#include <sstream>

namespace moult::core {

void DiagnosticSink::report(Diagnostic diagnostic) {
    diagnostics_.push_back(std::move(diagnostic));
}

void DiagnosticSink::note(std::string code, std::string message, std::optional<SourceRange> range) {
    report(Diagnostic{Severity::Note, std::move(code), std::move(message), std::move(range), {}});
}

void DiagnosticSink::warning(std::string code, std::string message, std::optional<SourceRange> range) {
    report(Diagnostic{Severity::Warning, std::move(code), std::move(message), std::move(range), {}});
}

void DiagnosticSink::error(std::string code, std::string message, std::optional<SourceRange> range) {
    report(Diagnostic{Severity::Error, std::move(code), std::move(message), std::move(range), {}});
}

bool DiagnosticSink::has_errors() const noexcept {
    return std::any_of(diagnostics_.begin(), diagnostics_.end(), [](const Diagnostic& d) {
        return d.severity == Severity::Error;
    });
}

static std::string range_to_json(const SourceRange& r) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("file", r.file);
    obj.number_field("begin", r.begin);
    obj.number_field("end", r.end);
    obj.finish();
    return os.str();
}

std::string diagnostic_to_json(const Diagnostic& diagnostic) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("severity", to_string(diagnostic.severity));
    obj.string_field("code", diagnostic.code);
    obj.string_field("message", diagnostic.message);
    if (diagnostic.range) obj.raw_field("range", range_to_json(*diagnostic.range));
    if (!diagnostic.attributes.empty()) obj.object_string_map_field("attributes", diagnostic.attributes);
    obj.finish();
    return os.str();
}

std::string diagnostics_to_json_array(const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& diagnostic : diagnostics) {
        if (!first) os << ",";
        first = false;
        os << diagnostic_to_json(diagnostic);
    }
    os << "]";
    return os.str();
}

} // namespace moult::core
