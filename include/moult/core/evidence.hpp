#pragma once

#include "moult/core/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace moult::core {

struct GuardResult {
    std::string name;
    GuardStatus status = GuardStatus::Unknown;
    std::string message;
    Attributes attributes;
};

struct EvidenceRecord {
    std::string id;
    std::string rule_id;
    std::string subject;
    std::string rationale;
    std::optional<SourceRange> range;
    std::vector<std::string> fact_ids;
    std::vector<GuardResult> guards;
    Attributes attributes;
};

[[nodiscard]] bool all_guards_passed(const std::vector<GuardResult>& guards) noexcept;
[[nodiscard]] bool any_guard_failed(const std::vector<GuardResult>& guards) noexcept;

std::string guard_to_json(const GuardResult& guard);
std::string evidence_to_json(const EvidenceRecord& evidence);
std::string evidence_to_jsonl(const std::vector<EvidenceRecord>& records);

} // namespace moult::core
