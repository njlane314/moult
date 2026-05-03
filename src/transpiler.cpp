#include "moult/core/transpiler.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace moult::core {
namespace {

constexpr std::string_view kToolAttr = "moult.transpiler.tool";
constexpr std::string_view kToolVersionAttr = "moult.transpiler.tool_version";
constexpr std::string_view kRuleIdAttr = "moult.transpiler.rule_id";
constexpr std::string_view kProposalKindAttr = "moult.transpiler.proposal_kind";
constexpr std::string_view kRationaleAttr = "moult.transpiler.rationale";
constexpr std::string_view kConfidenceAttr = "moult.transpiler.confidence";
constexpr std::string_view kSeverityAttr = "moult.transpiler.severity";
constexpr std::string_view kTitleAttr = "moult.transpiler.title";
constexpr std::string_view kMessageAttr = "moult.transpiler.message";
constexpr std::string_view kReplacementAttr = "moult.transpiler.replacement";
constexpr std::string_view kOriginalDigestAttr = "moult.transpiler.original_digest";
constexpr std::string_view kReplacementDigestAttr = "moult.transpiler.replacement_digest";
constexpr std::string_view kRewrittenDigestAttr = "moult.transpiler.rewritten_digest";
constexpr std::string_view kAcceptedByAttr = "moult.transpiler.accepted_by";
constexpr std::string_view kGuardCountAttr = "moult.transpiler.guard.count";

std::string key(std::string_view value) {
    return std::string(value);
}

const std::string* attr_ptr(const Attributes& attrs, std::string_view name) {
    const auto it = attrs.find(key(name));
    return it == attrs.end() ? nullptr : &it->second;
}

std::string attr_or(const Attributes& attrs, std::string_view name, std::string fallback = {}) {
    const std::string* value = attr_ptr(attrs, name);
    return value ? *value : std::move(fallback);
}

void set_attr(Attributes& attrs, std::string_view name, std::string value) {
    attrs[key(name)] = std::move(value);
}

void set_attr_if_nonempty(Attributes& attrs, std::string_view name, const std::string& value) {
    if (!value.empty()) set_attr(attrs, name, value);
}

void erase_attr(Attributes& attrs, std::string_view name) {
    attrs.erase(key(name));
}

std::string normalised_tool(std::string value) {
    return value.empty() ? "transpiler" : std::move(value);
}

std::string fallback_rule_id(const std::string& tool) {
    return "transpiler." + tool;
}

std::string text_digest(std::string_view text) {
    return stable_id("text", {text});
}

std::string site_subject(const std::string& tool,
                         const std::string& rule_id,
                         const SourceRange& range,
                         const std::string& replacement_digest) {
    const std::string begin = std::to_string(range.begin);
    const std::string end = std::to_string(range.end);
    return stable_id("transpiler_site", {tool, rule_id, range.file, begin, end, replacement_digest});
}

std::string finding_subject(const std::string& tool,
                            const std::string& rule_id,
                            const std::string& title,
                            const std::string& message,
                            const std::optional<SourceRange>& range) {
    std::vector<std::string_view> parts{tool, rule_id, title, message};
    std::string begin;
    std::string end;
    if (range) {
        begin = std::to_string(range->begin);
        end = std::to_string(range->end);
        parts.push_back(range->file);
        parts.push_back(begin);
        parts.push_back(end);
    }
    return stable_id("transpiler_finding", parts);
}

std::size_t parse_size(std::string_view value) noexcept {
    try {
        return static_cast<std::size_t>(std::stoull(std::string(value)));
    } catch (...) {
        return 0;
    }
}

GuardStatus guard_status_from_string(std::string_view value) noexcept {
    if (value == "passed") return GuardStatus::Passed;
    if (value == "failed") return GuardStatus::Failed;
    if (value == "not_applicable") return GuardStatus::NotApplicable;
    return GuardStatus::Unknown;
}

std::string guard_attr_key(std::size_t index, std::string_view field) {
    return "moult.transpiler.guard." + std::to_string(index) + "." + std::string(field);
}

void encode_guards(Attributes& attrs, const std::vector<GuardResult>& guards) {
    if (guards.empty()) return;
    set_attr(attrs, kGuardCountAttr, std::to_string(guards.size()));
    for (std::size_t i = 0; i < guards.size(); ++i) {
        attrs[guard_attr_key(i, "name")] = guards[i].name;
        attrs[guard_attr_key(i, "status")] = to_string(guards[i].status);
        if (!guards[i].message.empty()) attrs[guard_attr_key(i, "message")] = guards[i].message;
    }
}

std::vector<GuardResult> decode_guards(const Attributes& attrs) {
    const std::string* count_attr = attr_ptr(attrs, kGuardCountAttr);
    if (!count_attr) return {};

    std::vector<GuardResult> out;
    const std::size_t count = parse_size(*count_attr);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        GuardResult guard;
        guard.name = attr_or(attrs, guard_attr_key(i, "name"));
        guard.status = guard_status_from_string(attr_or(attrs, guard_attr_key(i, "status")));
        guard.message = attr_or(attrs, guard_attr_key(i, "message"));
        if (!guard.name.empty()) out.push_back(std::move(guard));
    }
    return out;
}

void report_transpiler_warning(DiagnosticSink& diagnostics,
                               std::string code,
                               std::string message,
                               const std::string& tool,
                               std::optional<SourceRange> range = std::nullopt) {
    Diagnostic diagnostic;
    diagnostic.severity = Severity::Warning;
    diagnostic.code = std::move(code);
    diagnostic.message = std::move(message);
    diagnostic.range = std::move(range);
    diagnostic.attributes["tool"] = tool;
    diagnostics.report(std::move(diagnostic));
}

Confidence confidence_from_fact(const Fact& fact, std::vector<GuardResult>& guards) {
    const std::string* confidence = attr_ptr(fact.attributes, kConfidenceAttr);
    if (!confidence) return Confidence::Medium;
    try {
        return confidence_from_string(*confidence);
    } catch (const std::exception&) {
        guards.push_back(GuardResult{"confidence_parse", GuardStatus::Failed, "invalid proposal confidence", {}});
        return Confidence::Medium;
    }
}

Severity severity_from_fact(const Fact& fact, std::vector<GuardResult>& guards) {
    const std::string* severity = attr_ptr(fact.attributes, kSeverityAttr);
    if (!severity) return Severity::Warning;
    try {
        return severity_from_string(*severity);
    } catch (const std::exception&) {
        guards.push_back(GuardResult{"severity_parse", GuardStatus::Failed, "invalid proposal severity", {}});
        return Severity::Warning;
    }
}

std::string proposal_rule_id(const Fact& fact) {
    const std::string tool = attr_or(fact.attributes, kToolAttr, "transpiler");
    return attr_or(fact.attributes, kRuleIdAttr, fallback_rule_id(tool));
}

Attributes public_proposal_attrs(const Fact& fact) {
    Attributes attrs = fact.attributes;
    erase_attr(attrs, kReplacementAttr);
    set_attr(attrs, kAcceptedByAttr, "moult.transpiler.accept-proposal");
    return attrs;
}

Attributes evidence_attrs(const Fact& fact) {
    Attributes attrs;
    set_attr_if_nonempty(attrs, kToolAttr, attr_or(fact.attributes, kToolAttr));
    set_attr_if_nonempty(attrs, kToolVersionAttr, attr_or(fact.attributes, kToolVersionAttr));
    set_attr_if_nonempty(attrs, kRuleIdAttr, attr_or(fact.attributes, kRuleIdAttr));
    set_attr_if_nonempty(attrs, kProposalKindAttr, attr_or(fact.attributes, kProposalKindAttr));
    return attrs;
}

std::string guard_summary(const std::vector<GuardResult>& guards) {
    std::ostringstream os;
    bool first = true;
    for (const auto& guard : guards) {
        if (guard.status != GuardStatus::Failed && guard.status != GuardStatus::Unknown) continue;
        if (!first) os << "; ";
        first = false;
        os << guard.name;
        if (!guard.message.empty()) os << ": " << guard.message;
    }
    if (first) return "candidate edit was not automatically accepted";
    return os.str();
}

void append_current_source_guards(const SourceStore& sources,
                                  const Fact& fact,
                                  std::vector<GuardResult>& guards) {
    if (!fact.range) {
        guards.push_back(GuardResult{"range_present", GuardStatus::Failed, "proposal has no source range", {}});
        return;
    }

    const SourceBuffer* source = sources.get(fact.range->file);
    if (!source || !source->contains(*fact.range)) {
        guards.push_back(
            GuardResult{"range_in_current_sources", GuardStatus::Failed, "proposal range is not valid in current sources", {}});
        return;
    }
    guards.push_back(GuardResult{"range_in_current_sources", GuardStatus::Passed, "proposal range is valid", {}});

    const std::string* expected_digest = attr_ptr(fact.attributes, kOriginalDigestAttr);
    if (!expected_digest) {
        guards.push_back(GuardResult{"source_digest_supplied", GuardStatus::Unknown,
                                     "proposal does not carry an original source digest", {}});
        return;
    }
    const std::string actual_digest = text_digest(source->slice(*fact.range));
    guards.push_back(GuardResult{"source_digest_matches",
                                 actual_digest == *expected_digest ? GuardStatus::Passed : GuardStatus::Failed,
                                 actual_digest == *expected_digest ? "current source matches proposal input"
                                                                   : "current source no longer matches proposal input",
                                 {}});
}

class TranspilerProposalRule final : public Rule {
public:
    std::string id() const override { return "moult.transpiler.accept-proposal"; }
    std::string summary() const override { return "Promote safe transpiler proposals into migration plans."; }

    void run(RuleContext& ctx) const override {
        for (const Fact* fact : ctx.facts().by_kind(transpiler_edit_fact_kind)) {
            accept_edit(ctx, *fact);
        }
        for (const Fact* fact : ctx.facts().by_kind(transpiler_finding_fact_kind)) {
            accept_finding(ctx, *fact);
        }
    }

private:
    static void accept_edit(RuleContext& ctx, const Fact& fact) {
        std::vector<GuardResult> guards = decode_guards(fact.attributes);
        const Confidence confidence = confidence_from_fact(fact, guards);
        const std::string* replacement = attr_ptr(fact.attributes, kReplacementAttr);
        guards.push_back(GuardResult{"replacement_present",
                                     replacement ? GuardStatus::Passed : GuardStatus::Failed,
                                     replacement ? "replacement text is present" : "proposal has no replacement text",
                                     {}});
        append_current_source_guards(ctx.sources(), fact, guards);
        guards.push_back(GuardResult{"minimum_confidence",
                                     confidence_at_least(confidence, ctx.options().minimum_confidence)
                                         ? GuardStatus::Passed
                                         : GuardStatus::Failed,
                                     "proposal confidence is " + to_string(confidence) + ", minimum is " +
                                         to_string(ctx.options().minimum_confidence),
                                     {}});

        const std::string rule_id = proposal_rule_id(fact);
        const std::string rationale = attr_or(fact.attributes, kRationaleAttr, "transpiler proposed a source replacement");
        auto& evidence = ctx.plan().evidence_for(rule_id, fact.subject, rationale, fact.range, {fact.id}, guards,
                                                 evidence_attrs(fact));

        if (replacement && fact.range && all_guards_passed(guards)) {
            auto edit = ctx.plan().make_edit(rule_id, *fact.range, *replacement, evidence.id, confidence,
                                             public_proposal_attrs(fact));
            ctx.plan().add_edit(std::move(edit));
            return;
        }

        Finding finding;
        finding.rule_id = rule_id;
        finding.title = "Transpiler candidate requires review";
        finding.message = guard_summary(guards);
        finding.severity = any_guard_failed(guards) ? Severity::Warning : Severity::Note;
        finding.confidence = confidence;
        finding.range = fact.range;
        finding.evidence_id = evidence.id;
        finding.attributes = public_proposal_attrs(fact);
        ctx.plan().add_finding(std::move(finding));
    }

    static void accept_finding(RuleContext& ctx, const Fact& fact) {
        std::vector<GuardResult> guards = decode_guards(fact.attributes);
        const Confidence confidence = confidence_from_fact(fact, guards);
        const Severity severity = severity_from_fact(fact, guards);
        const std::string rule_id = proposal_rule_id(fact);
        const std::string title = attr_or(fact.attributes, kTitleAttr, fact.object.empty() ? "Transpiler finding" : fact.object);
        const std::string message = attr_or(fact.attributes, kMessageAttr, title);
        const std::string rationale = attr_or(fact.attributes, kRationaleAttr, "transpiler reported a review finding");

        auto& evidence = ctx.plan().evidence_for(rule_id, fact.subject, rationale, fact.range, {fact.id}, guards,
                                                 evidence_attrs(fact));

        Finding finding;
        finding.rule_id = rule_id;
        finding.title = title;
        finding.message = message;
        finding.severity = severity;
        finding.confidence = confidence;
        finding.range = fact.range;
        finding.evidence_id = evidence.id;
        finding.attributes = public_proposal_attrs(fact);
        ctx.plan().add_finding(std::move(finding));
    }
};

} // namespace

TranspilerSink::TranspilerSink(const SourceStore& sources,
                               FactStore& facts,
                               DiagnosticSink& diagnostics,
                               std::string default_tool,
                               std::string default_tool_version)
    : sources_(sources),
      facts_(facts),
      diagnostics_(diagnostics),
      default_tool_(std::move(default_tool)),
      default_tool_version_(std::move(default_tool_version)) {}

const Fact* TranspilerSink::propose_edit(TranspilerEdit proposal) {
    const std::string tool = proposal.tool.empty() ? normalised_tool(default_tool_) : normalised_tool(std::move(proposal.tool));
    const std::string rule_id = proposal.rule_id.empty() ? fallback_rule_id(tool) : proposal.rule_id;

    if (!proposal.range.valid()) {
        report_transpiler_warning(diagnostics_, "transpiler.invalid_edit", "transpiler proposed an invalid edit range", tool);
        return nullptr;
    }

    const SourceBuffer* source = sources_.get(proposal.range.file);
    if (!source || !source->contains(proposal.range)) {
        report_transpiler_warning(diagnostics_, "transpiler.invalid_edit",
                                  "transpiler proposed an edit range outside the current source", tool, proposal.range);
        return nullptr;
    }

    const std::string original_digest = text_digest(source->slice(proposal.range));
    const std::string replacement_digest = text_digest(proposal.replacement);
    std::string subject = proposal.subject.empty() ? site_subject(tool, rule_id, proposal.range, replacement_digest)
                                                   : std::move(proposal.subject);

    Attributes attrs = std::move(proposal.attributes);
    set_attr(attrs, kToolAttr, tool);
    set_attr(attrs, kRuleIdAttr, rule_id);
    if (!attr_ptr(attrs, kProposalKindAttr)) set_attr(attrs, kProposalKindAttr, "edit");
    if (!default_tool_version_.empty() && !attr_ptr(attrs, kToolVersionAttr)) {
        set_attr(attrs, kToolVersionAttr, default_tool_version_);
    }
    set_attr(attrs, kConfidenceAttr, to_string(proposal.confidence));
    set_attr(attrs, kOriginalDigestAttr, original_digest);
    set_attr(attrs, kReplacementDigestAttr, replacement_digest);
    set_attr(attrs, kReplacementAttr, std::move(proposal.replacement));
    set_attr_if_nonempty(attrs, kRationaleAttr, proposal.rationale);
    encode_guards(attrs, proposal.guards);

    return &facts_.add(std::string(transpiler_edit_fact_kind), std::move(subject), "proposes_replacement",
                       replacement_digest, proposal.range, std::move(attrs));
}

const Fact* TranspilerSink::propose_file_rewrite(TranspilerFileRewrite proposal) {
    const std::string tool = proposal.tool.empty() ? normalised_tool(default_tool_) : normalised_tool(std::move(proposal.tool));
    const SourceBuffer* source = sources_.get(proposal.file);
    if (!source) {
        report_transpiler_warning(diagnostics_, "transpiler.missing_file",
                                  "transpiler proposed a rewrite for a file that is not in SourceStore", tool);
        return nullptr;
    }

    const std::string& original = source->text();
    if (original == proposal.rewritten_text) return nullptr;

    std::size_t prefix = 0;
    while (prefix < original.size() && prefix < proposal.rewritten_text.size() &&
           original[prefix] == proposal.rewritten_text[prefix]) {
        ++prefix;
    }

    std::size_t original_end = original.size();
    std::size_t rewritten_end = proposal.rewritten_text.size();
    while (original_end > prefix && rewritten_end > prefix &&
           original[original_end - 1] == proposal.rewritten_text[rewritten_end - 1]) {
        --original_end;
        --rewritten_end;
    }

    TranspilerEdit edit;
    edit.tool = tool;
    edit.rule_id = std::move(proposal.rule_id);
    edit.subject = std::move(proposal.subject);
    edit.range = SourceRange{proposal.file, prefix, original_end};
    edit.replacement = proposal.rewritten_text.substr(prefix, rewritten_end - prefix);
    edit.rationale = std::move(proposal.rationale);
    edit.confidence = proposal.confidence;
    edit.guards = std::move(proposal.guards);
    edit.attributes = std::move(proposal.attributes);
    set_attr(edit.attributes, kProposalKindAttr, "file_rewrite");
    set_attr(edit.attributes, kRewrittenDigestAttr, text_digest(proposal.rewritten_text));

    return propose_edit(std::move(edit));
}

const Fact* TranspilerSink::propose_finding(TranspilerFinding proposal) {
    const std::string tool = proposal.tool.empty() ? normalised_tool(default_tool_) : normalised_tool(std::move(proposal.tool));
    const std::string rule_id = proposal.rule_id.empty() ? fallback_rule_id(tool) : proposal.rule_id;
    std::optional<SourceRange> range = proposal.range;

    if (range) {
        const SourceBuffer* source = sources_.get(range->file);
        if (!range->valid() || !source || !source->contains(*range)) {
            report_transpiler_warning(diagnostics_, "transpiler.invalid_finding_range",
                                      "transpiler finding range is not valid in current sources", tool, range);
            range = std::nullopt;
        }
    }

    const std::string title = proposal.title.empty() ? "Transpiler finding" : proposal.title;
    const std::string message = proposal.message.empty() ? title : proposal.message;
    std::string subject = proposal.subject.empty() ? finding_subject(tool, rule_id, title, message, range)
                                                   : std::move(proposal.subject);

    Attributes attrs = std::move(proposal.attributes);
    set_attr(attrs, kToolAttr, tool);
    set_attr(attrs, kRuleIdAttr, rule_id);
    set_attr(attrs, kProposalKindAttr, "finding");
    if (!default_tool_version_.empty() && !attr_ptr(attrs, kToolVersionAttr)) {
        set_attr(attrs, kToolVersionAttr, default_tool_version_);
    }
    set_attr(attrs, kConfidenceAttr, to_string(proposal.confidence));
    set_attr(attrs, kSeverityAttr, to_string(proposal.severity));
    set_attr(attrs, kTitleAttr, title);
    set_attr(attrs, kMessageAttr, message);
    set_attr_if_nonempty(attrs, kRationaleAttr, proposal.rationale);
    if (range) {
        if (const SourceBuffer* source = sources_.get(range->file)) {
            set_attr(attrs, kOriginalDigestAttr, text_digest(source->slice(*range)));
        }
    }
    encode_guards(attrs, proposal.guards);

    return &facts_.add(std::string(transpiler_finding_fact_kind), std::move(subject), "reports_finding", title,
                       range, std::move(attrs));
}

TranspilerAdapter::TranspilerAdapter(std::vector<std::shared_ptr<const Transpiler>> transpilers)
    : transpilers_(std::move(transpilers)) {}

void TranspilerAdapter::add_transpiler(std::shared_ptr<const Transpiler> transpiler) {
    if (transpiler) transpilers_.push_back(std::move(transpiler));
}

void TranspilerAdapter::analyze(const SourceStore& sources,
                                FactStore& facts,
                                DiagnosticSink& diagnostics,
                                const RunOptions& options) const {
    for (const auto& transpiler : transpilers_) {
        if (!transpiler) continue;
        TranspilerSink sink(sources, facts, diagnostics, transpiler->id(), transpiler->version());
        try {
            transpiler->run(sources, sink, diagnostics, options);
        } catch (const std::exception& ex) {
            Diagnostic diagnostic;
            diagnostic.severity = Severity::Error;
            diagnostic.code = "transpiler.exception";
            diagnostic.message = "transpiler " + transpiler->id() + " failed: " + ex.what();
            diagnostic.attributes["tool"] = transpiler->id();
            diagnostics.report(std::move(diagnostic));
        }
    }
}

std::string TranspilerCapsule::id() const {
    return "moult.transpiler";
}

std::string TranspilerCapsule::name() const {
    return "Transpiler Proposal Bridge";
}

std::string TranspilerCapsule::version() const {
    return "0.1.0";
}

std::vector<std::string> TranspilerCapsule::targets() const {
    return {};
}

void TranspilerCapsule::register_rules(RuleRegistry& registry) const {
    registry.add(std::make_unique<TranspilerProposalRule>());
}

} // namespace moult::core
