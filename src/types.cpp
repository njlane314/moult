#include "moult/core/types.hpp"

#include <iomanip>
#include <stdexcept>

namespace moult::core {

std::string to_string(Severity severity) {
    switch (severity) {
        case Severity::Note: return "note";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "warning";
}

std::string to_string(Confidence confidence) {
    switch (confidence) {
        case Confidence::Low: return "low";
        case Confidence::Medium: return "medium";
        case Confidence::High: return "high";
        case Confidence::Proven: return "proven";
    }
    return "medium";
}

std::string to_string(GuardStatus status) {
    switch (status) {
        case GuardStatus::Passed: return "passed";
        case GuardStatus::Failed: return "failed";
        case GuardStatus::Unknown: return "unknown";
        case GuardStatus::NotApplicable: return "not_applicable";
    }
    return "unknown";
}

Severity severity_from_string(std::string_view value) {
    if (value == "note") return Severity::Note;
    if (value == "warning") return Severity::Warning;
    if (value == "error") return Severity::Error;
    throw std::invalid_argument("unknown severity: " + std::string(value));
}

Confidence confidence_from_string(std::string_view value) {
    if (value == "low") return Confidence::Low;
    if (value == "medium") return Confidence::Medium;
    if (value == "high") return Confidence::High;
    if (value == "proven") return Confidence::Proven;
    throw std::invalid_argument("unknown confidence: " + std::string(value));
}

bool confidence_at_least(Confidence value, Confidence minimum) noexcept {
    return static_cast<int>(value) >= static_cast<int>(minimum);
}

std::string stable_id(std::string_view prefix, const std::vector<std::string_view>& parts) {
    // 64-bit FNV-1a. Stable, cheap, and good enough for local identifiers.
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](std::string_view s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        h ^= 0xff;
        h *= 1099511628211ULL;
    };
    mix(prefix);
    for (std::string_view part : parts) mix(part);

    std::ostringstream os;
    os << prefix << "_" << std::hex << std::setw(16) << std::setfill('0') << h;
    return os.str();
}

std::string join_path_parts(std::string_view a, std::string_view b) {
    if (a.empty()) return std::string(b);
    if (b.empty()) return std::string(a);
    const bool a_sep = a.back() == '/' || a.back() == '\\';
    const bool b_sep = b.front() == '/' || b.front() == '\\';
    if (a_sep && b_sep) return std::string(a.substr(0, a.size() - 1)) + std::string(b);
    if (a_sep || b_sep) return std::string(a) + std::string(b);
    return std::string(a) + "/" + std::string(b);
}

} // namespace moult::core
