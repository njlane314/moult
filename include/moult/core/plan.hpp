#pragma once

#include "moult/core/diagnostics.hpp"
#include "moult/core/edits.hpp"
#include "moult/core/evidence.hpp"
#include "moult/core/facts.hpp"

#include <optional>
#include <string>
#include <vector>

namespace moult::core {

enum class PlanAction {
    Scan,
    Plan,
    Apply
};

struct Finding {
    std::string id;
    std::string rule_id;
    std::string title;
    std::string message;
    Severity severity = Severity::Warning;
    Confidence confidence = Confidence::Medium;
    std::optional<SourceRange> range;
    std::string evidence_id;
    Attributes attributes;
};

struct ChangeGroup {
    std::string id;
    std::string label;
    std::string phase;
    std::vector<std::string> edit_ids;
    std::vector<std::string> finding_ids;
    std::vector<std::string> requires_ids;
    Attributes attributes;
};

class Plan {
public:
    std::string target;
    PlanAction action = PlanAction::Plan;
    EditSet edits;
    std::vector<Finding> findings;
    std::vector<EvidenceRecord> evidence;
    std::vector<ChangeGroup> groups;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] std::size_t accepted_edit_count() const noexcept { return edits.edits().size(); }
    [[nodiscard]] std::size_t conflict_count() const noexcept { return edits.conflicts().size(); }
};

class PlanBuilder {
public:
    PlanBuilder(Plan& plan, const SourceStore* sources = nullptr);

    EvidenceRecord& add_evidence(EvidenceRecord evidence);
    Finding& add_finding(Finding finding);
    EditAddResult add_edit(TextEdit edit);
    ChangeGroup& add_group(ChangeGroup group);

    EvidenceRecord& evidence_for(std::string rule_id,
                                 std::string subject,
                                 std::string rationale,
                                 std::optional<SourceRange> range,
                                 std::vector<std::string> fact_ids,
                                 std::vector<GuardResult> guards,
                                 Attributes attrs = {});

    TextEdit make_edit(std::string rule_id,
                       SourceRange range,
                       std::string replacement,
                       std::string evidence_id,
                       Confidence confidence,
                       Attributes attrs = {}) const;

private:
    Plan& plan_;
    const SourceStore* sources_ = nullptr;
};

std::string to_string(PlanAction action);
std::string finding_to_json(const Finding& finding);
std::string group_to_json(const ChangeGroup& group);
std::string plan_to_json(const Plan& plan, const FactStore* facts = nullptr);

} // namespace moult::core
