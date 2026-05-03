#include "moult/core/evidence.hpp"
#include "moult/core/json.hpp"

#include <algorithm>
#include <sstream>

namespace moult::core {

bool all_guards_passed(const std::vector<GuardResult>& guards) noexcept {
    return std::all_of(guards.begin(), guards.end(), [](const GuardResult& g) {
        return g.status == GuardStatus::Passed || g.status == GuardStatus::NotApplicable;
    });
}

bool any_guard_failed(const std::vector<GuardResult>& guards) noexcept {
    return std::any_of(guards.begin(), guards.end(), [](const GuardResult& g) {
        return g.status == GuardStatus::Failed;
    });
}

std::string guard_to_json(const GuardResult& guard) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("name", guard.name);
    obj.string_field("status", to_string(guard.status));
    if (!guard.message.empty()) obj.string_field("message", guard.message);
    if (!guard.attributes.empty()) obj.object_string_map_field("attributes", guard.attributes);
    obj.finish();
    return os.str();
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

static std::string guards_to_json_array(const std::vector<GuardResult>& guards) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& guard : guards) {
        if (!first) os << ",";
        first = false;
        os << guard_to_json(guard);
    }
    os << "]";
    return os.str();
}

std::string evidence_to_json(const EvidenceRecord& evidence) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("id", evidence.id);
    obj.string_field("rule_id", evidence.rule_id);
    obj.string_field("subject", evidence.subject);
    if (!evidence.rationale.empty()) obj.string_field("rationale", evidence.rationale);
    if (evidence.range) obj.raw_field("range", range_to_json(*evidence.range));
    obj.string_array_field("fact_ids", evidence.fact_ids);
    obj.raw_field("guards", guards_to_json_array(evidence.guards));
    if (!evidence.attributes.empty()) obj.object_string_map_field("attributes", evidence.attributes);
    obj.finish();
    return os.str();
}

std::string evidence_to_jsonl(const std::vector<EvidenceRecord>& records) {
    std::ostringstream os;
    for (const auto& record : records) {
        os << evidence_to_json(record) << "\n";
    }
    return os.str();
}

} // namespace moult::core
