#pragma once

#include "moult/core/engine.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moult::core {

inline constexpr std::string_view transpiler_edit_fact_kind = "transpiler.edit";
inline constexpr std::string_view transpiler_finding_fact_kind = "transpiler.finding";

// A precise candidate edit emitted by an external or in-process transpiler.
// The sink records it as a fact; TranspilerCapsule later decides whether it is
// safe enough to become a Plan edit.
struct TranspilerEdit {
    std::string tool;
    std::string rule_id;
    std::string subject;
    SourceRange range;
    std::string replacement;
    std::string rationale;
    Confidence confidence = Confidence::Medium;
    std::vector<GuardResult> guards;
    Attributes attributes;
};

// Convenience form for opaque source-to-source compilers. The sink compares the
// rewritten file to the original source and emits one minimal byte-range edit by
// trimming the common prefix and suffix.
struct TranspilerFileRewrite {
    std::string tool;
    std::string rule_id;
    std::string subject;
    std::string file;
    std::string rewritten_text;
    std::string rationale;
    Confidence confidence = Confidence::Medium;
    std::vector<GuardResult> guards;
    Attributes attributes;
};

// A review item emitted by a transpiler when it can identify a site but should
// not automatically rewrite it.
struct TranspilerFinding {
    std::string tool;
    std::string rule_id;
    std::string subject;
    std::string title;
    std::string message;
    Severity severity = Severity::Warning;
    Confidence confidence = Confidence::Medium;
    std::optional<SourceRange> range;
    std::string rationale;
    std::vector<GuardResult> guards;
    Attributes attributes;
};

class TranspilerSink {
public:
    TranspilerSink(const SourceStore& sources,
                   FactStore& facts,
                   DiagnosticSink& diagnostics,
                   std::string default_tool = {},
                   std::string default_tool_version = {});

    // Returns nullptr when the proposal cannot be represented safely, for
    // example because its range does not belong to the current SourceStore.
    const Fact* propose_edit(TranspilerEdit proposal);
    const Fact* propose_file_rewrite(TranspilerFileRewrite proposal);
    const Fact* propose_finding(TranspilerFinding proposal);

private:
    const SourceStore& sources_;
    FactStore& facts_;
    DiagnosticSink& diagnostics_;
    std::string default_tool_;
    std::string default_tool_version_;
};

class Transpiler {
public:
    virtual ~Transpiler() = default;

    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string name() const { return id(); }
    [[nodiscard]] virtual std::string version() const { return {}; }

    // Implementations may call other tools, run in-process parsers, or render
    // source directly. They must not mutate files; they report only proposals.
    virtual void run(const SourceStore& sources,
                     TranspilerSink& sink,
                     DiagnosticSink& diagnostics,
                     const RunOptions& options) const = 0;
};

// Runs one or more transpilers as a SemanticAdapter. This deliberately emits
// facts instead of edits so normal Moult capsules still own evidence, guards,
// conflict detection, and review classification.
class TranspilerAdapter final : public SemanticAdapter {
public:
    TranspilerAdapter() = default;
    explicit TranspilerAdapter(std::vector<std::shared_ptr<const Transpiler>> transpilers);

    void add_transpiler(std::shared_ptr<const Transpiler> transpiler);

    void analyze(const SourceStore& sources,
                 FactStore& facts,
                 DiagnosticSink& diagnostics,
                 const RunOptions& options) const override;

private:
    std::vector<std::shared_ptr<const Transpiler>> transpilers_;
};

// Converts transpiler proposal facts into Plan evidence, edits, and findings.
// Add this capsule whenever a TranspilerAdapter is installed.
class TranspilerCapsule final : public Capsule {
public:
    [[nodiscard]] std::string id() const override;
    [[nodiscard]] std::string name() const override;
    [[nodiscard]] std::string version() const override;
    [[nodiscard]] std::vector<std::string> targets() const override;
    void register_rules(RuleRegistry& registry) const override;
};

} // namespace moult::core
