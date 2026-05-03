#include "moult/core/plan.hpp"
#include "moult/core/json.hpp"

#include <sstream>

namespace moult::core {

std::string to_string(PlanAction action) {
    switch (action) {
        case PlanAction::Scan: return "scan";
        case PlanAction::Plan: return "plan";
        case PlanAction::Apply: return "apply";
    }
    return "plan";
}

bool Plan::has_errors() const noexcept {
    if (edits.has_conflicts()) return true;
    for (const auto& d : diagnostics) {
        if (d.severity == Severity::Error) return true;
    }
    return false;
}

PlanBuilder::PlanBuilder(Plan& plan, const SourceStore* sources) : plan_(plan), sources_(sources) {}

EvidenceRecord& PlanBuilder::add_evidence(EvidenceRecord evidence) {
    if (evidence.id.empty()) {
        std::string begin;
        std::string end;
        std::vector<std::string_view> parts{evidence.rule_id, evidence.subject, evidence.rationale};
        if (evidence.range) {
            begin = std::to_string(evidence.range->begin);
            end = std::to_string(evidence.range->end);
            parts.push_back(evidence.range->file);
            parts.push_back(begin);
            parts.push_back(end);
        }
        evidence.id = stable_id("evidence", parts);
    }
    plan_.evidence.push_back(std::move(evidence));
    return plan_.evidence.back();
}

Finding& PlanBuilder::add_finding(Finding finding) {
    if (finding.id.empty()) {
        std::string begin;
        std::string end;
        std::vector<std::string_view> parts{finding.rule_id, finding.title, finding.message, finding.evidence_id};
        if (finding.range) {
            begin = std::to_string(finding.range->begin);
            end = std::to_string(finding.range->end);
            parts.push_back(finding.range->file);
            parts.push_back(begin);
            parts.push_back(end);
        }
        finding.id = stable_id("finding", parts);
    }
    plan_.findings.push_back(std::move(finding));
    return plan_.findings.back();
}

EditAddResult PlanBuilder::add_edit(TextEdit edit) {
    return plan_.edits.add(std::move(edit), sources_);
}

ChangeGroup& PlanBuilder::add_group(ChangeGroup group) {
    if (group.id.empty()) {
        group.id = stable_id("group", {group.label, group.phase});
    }
    plan_.groups.push_back(std::move(group));
    return plan_.groups.back();
}

EvidenceRecord& PlanBuilder::evidence_for(std::string rule_id,
                                           std::string subject,
                                           std::string rationale,
                                           std::optional<SourceRange> range,
                                           std::vector<std::string> fact_ids,
                                           std::vector<GuardResult> guards,
                                           Attributes attrs) {
    EvidenceRecord evidence;
    evidence.rule_id = std::move(rule_id);
    evidence.subject = std::move(subject);
    evidence.rationale = std::move(rationale);
    evidence.range = std::move(range);
    evidence.fact_ids = std::move(fact_ids);
    evidence.guards = std::move(guards);
    evidence.attributes = std::move(attrs);
    return add_evidence(std::move(evidence));
}

TextEdit PlanBuilder::make_edit(std::string rule_id,
                                SourceRange range,
                                std::string replacement,
                                std::string evidence_id,
                                Confidence confidence,
                                Attributes attrs) const {
    TextEdit edit;
    edit.rule_id = std::move(rule_id);
    edit.range = std::move(range);
    edit.replacement = std::move(replacement);
    edit.evidence_id = std::move(evidence_id);
    edit.confidence = confidence;
    edit.attributes = std::move(attrs);
    return edit;
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

std::string finding_to_json(const Finding& finding) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("id", finding.id);
    obj.string_field("rule_id", finding.rule_id);
    obj.string_field("title", finding.title);
    obj.string_field("message", finding.message);
    obj.string_field("severity", to_string(finding.severity));
    obj.string_field("confidence", to_string(finding.confidence));
    if (finding.range) obj.raw_field("range", range_to_json(*finding.range));
    if (!finding.evidence_id.empty()) obj.string_field("evidence_id", finding.evidence_id);
    if (!finding.attributes.empty()) obj.object_string_map_field("attributes", finding.attributes);
    obj.finish();
    return os.str();
}

std::string group_to_json(const ChangeGroup& group) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("id", group.id);
    obj.string_field("label", group.label);
    obj.string_field("phase", group.phase);
    obj.string_array_field("edit_ids", group.edit_ids);
    obj.string_array_field("finding_ids", group.finding_ids);
    obj.string_array_field("requires", group.requires_ids);
    if (!group.attributes.empty()) obj.object_string_map_field("attributes", group.attributes);
    obj.finish();
    return os.str();
}

static std::string findings_to_json_array(const std::vector<Finding>& findings) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& finding : findings) {
        if (!first) os << ",";
        first = false;
        os << finding_to_json(finding);
    }
    os << "]";
    return os.str();
}

static std::string evidence_to_json_array(const std::vector<EvidenceRecord>& evidence) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& record : evidence) {
        if (!first) os << ",";
        first = false;
        os << evidence_to_json(record);
    }
    os << "]";
    return os.str();
}

static std::string groups_to_json_array(const std::vector<ChangeGroup>& groups) {
    std::ostringstream os;
    os << "[";
    bool first = true;
    for (const auto& group : groups) {
        if (!first) os << ",";
        first = false;
        os << group_to_json(group);
    }
    os << "]";
    return os.str();
}

std::string plan_to_json(const Plan& plan, const FactStore* facts) {
    std::ostringstream os;
    JsonObjectWriter obj(os);
    obj.string_field("target", plan.target);
    obj.string_field("action", to_string(plan.action));
    obj.bool_field("has_errors", plan.has_errors());
    obj.number_field("accepted_edit_count", plan.accepted_edit_count());
    obj.number_field("conflict_count", plan.conflict_count());
    obj.raw_field("edits", edits_to_json_array(plan.edits.sorted()));
    obj.raw_field("edit_conflicts", conflicts_to_json_array(plan.edits.conflicts()));
    obj.raw_field("findings", findings_to_json_array(plan.findings));
    obj.raw_field("evidence", evidence_to_json_array(plan.evidence));
    obj.raw_field("groups", groups_to_json_array(plan.groups));
    obj.raw_field("diagnostics", diagnostics_to_json_array(plan.diagnostics));
    if (facts) obj.raw_field("facts", facts_to_json_array(facts->all()));
    obj.finish();
    return os.str();
}

} // namespace moult::core
