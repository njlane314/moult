#pragma once

#include "moult/core/diagnostics.hpp"
#include "moult/core/facts.hpp"
#include "moult/core/plan.hpp"
#include "moult/core/source.hpp"

#include <memory>
#include <string>
#include <vector>

namespace moult::core {

struct RunOptions {
    std::string target;
    PlanAction action = PlanAction::Plan;
    Confidence minimum_confidence = Confidence::High;
    std::vector<std::string> enabled_capsules;
    Attributes attributes;
};

class RuleContext {
public:
    RuleContext(const RunOptions& options,
                const SourceStore& sources,
                const FactStore& facts,
                PlanBuilder& plan,
                DiagnosticSink& diagnostics);

    [[nodiscard]] const RunOptions& options() const noexcept { return options_; }
    [[nodiscard]] const SourceStore& sources() const noexcept { return sources_; }
    [[nodiscard]] const FactStore& facts() const noexcept { return facts_; }
    [[nodiscard]] PlanBuilder& plan() noexcept { return plan_; }
    [[nodiscard]] DiagnosticSink& diagnostics() noexcept { return diagnostics_; }

private:
    const RunOptions& options_;
    const SourceStore& sources_;
    const FactStore& facts_;
    PlanBuilder& plan_;
    DiagnosticSink& diagnostics_;
};

class Rule {
public:
    virtual ~Rule() = default;
    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string summary() const = 0;
    [[nodiscard]] virtual std::vector<std::string> targets() const { return {}; }
    virtual void run(RuleContext& ctx) const = 0;
};

class RuleRegistry {
public:
    void add(std::unique_ptr<Rule> rule);
    [[nodiscard]] const std::vector<std::unique_ptr<Rule>>& rules() const noexcept { return rules_; }

private:
    std::vector<std::unique_ptr<Rule>> rules_;
};

class Capsule {
public:
    virtual ~Capsule() = default;
    [[nodiscard]] virtual std::string id() const = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::string version() const = 0;
    [[nodiscard]] virtual std::vector<std::string> targets() const = 0;
    virtual void register_rules(RuleRegistry& registry) const = 0;
};

[[nodiscard]] bool capsule_enabled(const Capsule& capsule, const RunOptions& options);
[[nodiscard]] bool rule_targets_target(const Rule& rule, std::string_view target);

} // namespace moult::core
