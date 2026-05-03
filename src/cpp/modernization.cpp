#include "moult/cpp/modernization.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <string>

namespace moult::cpp_modernization {
namespace {

struct Opportunity {
    std::string_view kind;
    std::string_view token;
    std::string_view replacement;
    core::Confidence confidence = core::Confidence::High;
    bool edit_capable = true;
    std::string_view title;
    std::string_view message;
    std::string_view rationale;
};

constexpr Opportunity use_nullptr{
    "use-nullptr",
    "NULL",
    "nullptr",
    core::Confidence::High,
    true,
    "replace NULL with nullptr",
    "Use nullptr for null pointer constants in modern C++.",
    "NULL was found in C++ source and can usually be represented more precisely as nullptr."};

constexpr Opportunity use_noexcept{
    "use-noexcept",
    "throw()",
    "noexcept",
    core::Confidence::High,
    true,
    "replace empty exception specification with noexcept",
    "Use noexcept instead of the deprecated throw() exception specification.",
    "An empty dynamic exception specification has the same non-throwing intent as noexcept."};

constexpr Opportunity remove_register{
    "remove-register",
    "register",
    "",
    core::Confidence::High,
    true,
    "remove obsolete register keyword",
    "Remove the obsolete register storage-class specifier.",
    "The register storage-class specifier is obsolete in modern C++."};

constexpr Opportunity replace_auto_ptr{
    "replace-auto-ptr",
    "std::auto_ptr",
    "std::unique_ptr",
    core::Confidence::Medium,
    false,
    "review std::auto_ptr usage",
    "std::auto_ptr is removed in C++17; migrate ownership semantics to std::unique_ptr.",
    "std::auto_ptr transfer semantics require manual review before an automatic replacement is safe."};

constexpr Opportunity prefer_using_alias{
    "prefer-using-alias",
    "typedef",
    "",
    core::Confidence::Medium,
    false,
    "review typedef alias",
    "Prefer using aliases over typedef declarations in modern C++.",
    "typedef declarations often read more clearly as using aliases, but the exact rewrite needs declaration context."};

constexpr Opportunity review_raw_new{
    "review-raw-new",
    "new",
    "",
    core::Confidence::Medium,
    false,
    "review raw new expression",
    "Review raw new usage for replacement with ownership types or factory helpers.",
    "Raw allocation can often be replaced with std::make_unique, std::make_shared, or value ownership."};

constexpr Opportunity review_raw_delete{
    "review-raw-delete",
    "delete",
    "",
    core::Confidence::Medium,
    false,
    "review raw delete expression",
    "Review raw delete usage for replacement with RAII ownership.",
    "Manual deletion is often a sign that ownership should move to a smart pointer or value type."};

bool is_identifier_char(char c) noexcept {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

bool has_identifier_boundaries(std::string_view text, std::size_t begin, std::size_t end) noexcept {
    const bool before_ok = begin == 0 || !is_identifier_char(text[begin - 1]);
    const bool after_ok = end >= text.size() || !is_identifier_char(text[end]);
    return before_ok && after_ok;
}

bool starts_with(std::string_view text, std::size_t pos, std::string_view token) noexcept {
    return pos <= text.size() && token.size() <= text.size() - pos && text.substr(pos, token.size()) == token;
}

std::string confidence_string(core::Confidence confidence) {
    return core::to_string(confidence);
}

std::optional<core::Confidence> parse_confidence(const core::Attributes& attrs) {
    const auto it = attrs.find("confidence");
    if (it == attrs.end()) return std::nullopt;
    try {
        return core::confidence_from_string(it->second);
    } catch (...) {
        return std::nullopt;
    }
}

bool attr_bool(const core::Attributes& attrs, std::string_view key) {
    const auto it = attrs.find(std::string(key));
    return it != attrs.end() && it->second == "true";
}

std::string attr_or(const core::Attributes& attrs, std::string_view key, std::string fallback = {}) {
    const auto it = attrs.find(std::string(key));
    return it == attrs.end() ? std::move(fallback) : it->second;
}

void add_opportunity(core::FactStore& facts,
                     std::string_view path,
                     std::size_t begin,
                     std::size_t end,
                     const Opportunity& opportunity) {
    core::Attributes attrs{
        {"token", std::string(opportunity.token)},
        {"replacement", std::string(opportunity.replacement)},
        {"title", std::string(opportunity.title)},
        {"message", std::string(opportunity.message)},
        {"rationale", std::string(opportunity.rationale)},
        {"confidence", confidence_string(opportunity.confidence)},
        {"edit_capable", opportunity.edit_capable ? "true" : "false"},
        {"scanner", "textual-cpp-modernization"}};

    facts.add("cpp.modernization.opportunity",
              "site:" + std::string(path) + ":" + std::to_string(begin),
              "opportunity",
              std::string(opportunity.kind),
              core::SourceRange{std::string(path), begin, end},
              std::move(attrs));
}

std::size_t skip_raw_string(std::string_view text, std::size_t pos) {
    const std::size_t delimiter_begin = pos + 2;
    const std::size_t open_paren = text.find('(', delimiter_begin);
    if (open_paren == std::string_view::npos || open_paren - delimiter_begin > 16) return pos + 2;

    const std::string delimiter(text.substr(delimiter_begin, open_paren - delimiter_begin));
    const std::string close = ")" + delimiter + "\"";
    const std::size_t close_pos = text.find(close, open_paren + 1);
    return close_pos == std::string_view::npos ? text.size() : close_pos + close.size();
}

std::size_t skip_quoted(std::string_view text, std::size_t pos, char quote) {
    ++pos;
    while (pos < text.size()) {
        if (text[pos] == '\\') {
            pos += pos + 1 < text.size() ? 2 : 1;
            continue;
        }
        if (text[pos] == quote) return pos + 1;
        ++pos;
    }
    return pos;
}

void scan_source(const core::SourceBuffer& source, core::FactStore& facts) {
    const std::string_view text = source.text();
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (starts_with(text, pos, "//")) {
            const std::size_t end = text.find('\n', pos + 2);
            pos = end == std::string_view::npos ? text.size() : end + 1;
            continue;
        }
        if (starts_with(text, pos, "/*")) {
            const std::size_t end = text.find("*/", pos + 2);
            pos = end == std::string_view::npos ? text.size() : end + 2;
            continue;
        }
        if (starts_with(text, pos, "R\"")) {
            pos = skip_raw_string(text, pos);
            continue;
        }
        if (text[pos] == '"') {
            pos = skip_quoted(text, pos, '"');
            continue;
        }
        if (text[pos] == '\'') {
            pos = skip_quoted(text, pos, '\'');
            continue;
        }

        if (starts_with(text, pos, use_noexcept.token) && has_identifier_boundaries(text, pos, pos + 5)) {
            add_opportunity(facts, source.path(), pos, pos + use_noexcept.token.size(), use_noexcept);
            pos += use_noexcept.token.size();
            continue;
        }
        if (starts_with(text, pos, prefer_using_alias.token) &&
            has_identifier_boundaries(text, pos, pos + prefer_using_alias.token.size())) {
            add_opportunity(facts, source.path(), pos, pos + prefer_using_alias.token.size(), prefer_using_alias);
            pos += prefer_using_alias.token.size();
            continue;
        }
        if (starts_with(text, pos, replace_auto_ptr.token) &&
            has_identifier_boundaries(text, pos, pos + replace_auto_ptr.token.size())) {
            add_opportunity(facts, source.path(), pos, pos + replace_auto_ptr.token.size(), replace_auto_ptr);
            pos += replace_auto_ptr.token.size();
            continue;
        }
        if (starts_with(text, pos, review_raw_delete.token) &&
            has_identifier_boundaries(text, pos, pos + review_raw_delete.token.size())) {
            add_opportunity(facts, source.path(), pos, pos + review_raw_delete.token.size(), review_raw_delete);
            pos += review_raw_delete.token.size();
            continue;
        }
        if (starts_with(text, pos, review_raw_new.token) &&
            has_identifier_boundaries(text, pos, pos + review_raw_new.token.size())) {
            add_opportunity(facts, source.path(), pos, pos + review_raw_new.token.size(), review_raw_new);
            pos += review_raw_new.token.size();
            continue;
        }
        if (starts_with(text, pos, use_nullptr.token) &&
            has_identifier_boundaries(text, pos, pos + use_nullptr.token.size())) {
            add_opportunity(facts, source.path(), pos, pos + use_nullptr.token.size(), use_nullptr);
            pos += use_nullptr.token.size();
            continue;
        }
        if (starts_with(text, pos, remove_register.token) &&
            has_identifier_boundaries(text, pos, pos + remove_register.token.size())) {
            std::size_t end = pos + remove_register.token.size();
            if (end < text.size() && (text[end] == ' ' || text[end] == '\t')) ++end;
            add_opportunity(facts, source.path(), pos, end, remove_register);
            pos = end;
            continue;
        }

        ++pos;
    }
}

class ModernizationRule final : public core::Rule {
public:
    ModernizationRule(std::string kind, std::string id) : kind_(std::move(kind)), id_(std::move(id)) {}

    [[nodiscard]] std::string id() const override { return id_; }
    [[nodiscard]] std::string summary() const override { return "Report C++ modernization opportunity: " + kind_; }
    [[nodiscard]] std::vector<std::string> targets() const override { return {std::string(target_name)}; }

    void run(core::RuleContext& ctx) const override {
        for (const core::Fact* fact : ctx.facts().where([&](const core::Fact& f) {
                 return f.kind == "cpp.modernization.opportunity" && f.predicate == "opportunity" &&
                        f.object == kind_;
             })) {
            const auto confidence = parse_confidence(fact->attributes).value_or(core::Confidence::Medium);
            const auto token = attr_or(fact->attributes, "token");
            const auto replacement = attr_or(fact->attributes, "replacement");

            std::vector<core::GuardResult> guards{
                core::GuardResult{"fact_has_source_range",
                                  fact->range ? core::GuardStatus::Passed : core::GuardStatus::Failed,
                                  "",
                                  {}},
                core::GuardResult{attr_or(fact->attributes, "scanner", "modernization_scan"),
                                  core::GuardStatus::Passed,
                                  "",
                                  {}}};

            if (fact->range) {
                const auto* source = ctx.sources().get(fact->range->file);
                const bool token_matches =
                    source && source->contains(*fact->range) && source->slice(*fact->range).substr(0, token.size()) == token;
                guards.push_back(core::GuardResult{"range_starts_with_expected_token",
                                                   token_matches ? core::GuardStatus::Passed : core::GuardStatus::Failed,
                                                   "",
                                                   {}});
            }

            auto& evidence = ctx.plan().evidence_for(id_,
                                                     fact->subject,
                                                     attr_or(fact->attributes, "rationale"),
                                                     fact->range,
                                                     {fact->id},
                                                     guards);

            core::Finding finding;
            finding.rule_id = id_;
            finding.title = attr_or(fact->attributes, "title", "C++ modernization opportunity");
            finding.message = attr_or(fact->attributes, "message");
            finding.severity = confidence == core::Confidence::Medium ? core::Severity::Warning : core::Severity::Note;
            finding.confidence = confidence;
            finding.range = fact->range;
            finding.evidence_id = evidence.id;
            finding.attributes = fact->attributes;
            finding.attributes["opportunity"] = kind_;

            const bool can_edit = attr_bool(fact->attributes, "edit_capable") && fact->range &&
                                  core::all_guards_passed(evidence.guards);
            if (!can_edit) {
                finding.attributes["edit_status"] = "manual_review";
            } else if (!core::confidence_at_least(confidence, ctx.options().minimum_confidence)) {
                finding.attributes["edit_status"] = "below_minimum_confidence";
            } else {
                auto edit = ctx.plan().make_edit(id_, *fact->range, replacement, evidence.id, confidence);
                const auto outcome = ctx.plan().add_edit(std::move(edit));
                finding.attributes["edit_status"] = outcome.outcome == core::EditAddOutcome::Accepted ? "planned" : "not_planned";
            }

            ctx.plan().add_finding(std::move(finding));
        }
    }

private:
    std::string kind_;
    std::string id_;
};

} // namespace

void TextualCppModernizationAdapter::analyze(const core::SourceStore& sources,
                                             core::FactStore& facts,
                                             core::DiagnosticSink&,
                                             const core::RunOptions&) const {
    for (const auto& [_, source] : sources.files()) {
        scan_source(source, facts);
    }
}

std::string CppModernizationCapsule::id() const {
    return "moult.cpp-modernization";
}

std::string CppModernizationCapsule::name() const {
    return "C++ Modernization";
}

std::string CppModernizationCapsule::version() const {
    return "0.1.0";
}

std::vector<std::string> CppModernizationCapsule::targets() const {
    return {std::string(target_name)};
}

void CppModernizationCapsule::register_rules(core::RuleRegistry& registry) const {
    registry.add(std::make_unique<ModernizationRule>("use-nullptr", "modernize.use-nullptr"));
    registry.add(std::make_unique<ModernizationRule>("use-noexcept", "modernize.use-noexcept"));
    registry.add(std::make_unique<ModernizationRule>("remove-register", "modernize.remove-register"));
    registry.add(std::make_unique<ModernizationRule>("replace-auto-ptr", "modernize.replace-auto-ptr"));
    registry.add(std::make_unique<ModernizationRule>("prefer-using-alias", "modernize.prefer-using-alias"));
    registry.add(std::make_unique<ModernizationRule>("review-raw-new", "modernize.review-raw-new"));
    registry.add(std::make_unique<ModernizationRule>("review-raw-delete", "modernize.review-raw-delete"));
    registry.add(std::make_unique<ModernizationRule>("review-c-style-cast", "modernize.review-c-style-cast"));
}

} // namespace moult::cpp_modernization
