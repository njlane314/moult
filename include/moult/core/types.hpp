#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace moult::core {

using Attributes = std::map<std::string, std::string>;

struct SourceRange {
    std::string file;
    std::size_t begin = 0; // byte offset, inclusive
    std::size_t end = 0;   // byte offset, exclusive

    [[nodiscard]] bool valid() const noexcept { return !file.empty() && begin <= end; }
    [[nodiscard]] std::size_t size() const noexcept { return end >= begin ? end - begin : 0; }
};

struct LineColumn {
    std::size_t line = 1;   // 1-based
    std::size_t column = 1; // 1-based, byte column
};

enum class Severity {
    Note,
    Warning,
    Error
};

enum class Confidence {
    Low = 25,
    Medium = 50,
    High = 75,
    Proven = 100
};

enum class GuardStatus {
    Passed,
    Failed,
    Unknown,
    NotApplicable
};

std::string to_string(Severity severity);
std::string to_string(Confidence confidence);
std::string to_string(GuardStatus status);

Severity severity_from_string(std::string_view value);
Confidence confidence_from_string(std::string_view value);

[[nodiscard]] bool confidence_at_least(Confidence value, Confidence minimum) noexcept;

// Stable, deterministic non-cryptographic identifier used for local evidence/change IDs.
// It deliberately avoids std::hash because std::hash is not guaranteed stable across processes.
std::string stable_id(std::string_view prefix, const std::vector<std::string_view>& parts);

std::string join_path_parts(std::string_view a, std::string_view b);

} // namespace moult::core
