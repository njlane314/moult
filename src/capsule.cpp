#include "moult/core/capsule.hpp"

#include <algorithm>

namespace moult::core {

RuleContext::RuleContext(const RunOptions& options,
                         const SourceStore& sources,
                         const FactStore& facts,
                         PlanBuilder& plan,
                         DiagnosticSink& diagnostics)
    : options_(options), sources_(sources), facts_(facts), plan_(plan), diagnostics_(diagnostics) {}

void RuleRegistry::add(std::unique_ptr<Rule> rule) {
    if (rule) rules_.push_back(std::move(rule));
}

bool capsule_enabled(const Capsule& capsule, const RunOptions& options) {
    if (!options.enabled_capsules.empty()) {
        return std::find(options.enabled_capsules.begin(), options.enabled_capsules.end(), capsule.id()) != options.enabled_capsules.end();
    }
    const auto targets = capsule.targets();
    if (options.target.empty() || targets.empty()) return true;
    return std::find(targets.begin(), targets.end(), options.target) != targets.end();
}

bool rule_targets_target(const Rule& rule, std::string_view target) {
    const auto targets = rule.targets();
    if (target.empty() || targets.empty()) return true;
    return std::find(targets.begin(), targets.end(), std::string(target)) != targets.end();
}

} // namespace moult::core
