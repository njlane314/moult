#pragma once

#include "moult/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace moult::core {

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string code;
    std::string message;
    std::optional<SourceRange> range;
    Attributes attributes;
};

class DiagnosticSink {
public:
    void report(Diagnostic diagnostic);
    void note(std::string code, std::string message, std::optional<SourceRange> range = std::nullopt);
    void warning(std::string code, std::string message, std::optional<SourceRange> range = std::nullopt);
    void error(std::string code, std::string message, std::optional<SourceRange> range = std::nullopt);

    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] bool has_errors() const noexcept;
    void clear() noexcept { diagnostics_.clear(); }

private:
    std::vector<Diagnostic> diagnostics_;
};

std::string diagnostic_to_json(const Diagnostic& diagnostic);
std::string diagnostics_to_json_array(const std::vector<Diagnostic>& diagnostics);

} // namespace moult::core
