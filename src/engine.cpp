#include "moult/core/engine.hpp"

namespace moult::core {

void Engine::set_adapter(std::shared_ptr<const SemanticAdapter> adapter) {
    adapter_ = std::move(adapter);
}

void Engine::add_capsule(std::shared_ptr<const Capsule> capsule) {
    if (capsule) capsules_.push_back(std::move(capsule));
}

EngineRunResult Engine::run(const SourceStore& sources, const RunOptions& options) const {
    EngineRunResult result;
    result.plan.target = options.target;
    result.plan.action = options.action;

    if (adapter_) {
        adapter_->analyze(sources, result.facts, result.diagnostics, options);
    }

    PlanBuilder builder(result.plan, &sources);
    RuleRegistry registry;
    for (const auto& capsule : capsules_) {
        if (capsule_enabled(*capsule, options)) capsule->register_rules(registry);
    }

    for (const auto& rule : registry.rules()) {
        if (!rule_targets_target(*rule, options.target)) continue;
        RuleContext ctx(options, sources, result.facts, builder, result.diagnostics);
        rule->run(ctx);
    }

    result.plan.diagnostics = result.diagnostics.diagnostics();
    return result;
}

} // namespace moult::core
