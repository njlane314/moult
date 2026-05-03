#include "moult/core/api.hpp"

#include <iostream>
#include <memory>

using namespace moult::core;

// Example adapter: a real adapter would be Clang, CodeQL export, tree-sitter, etc.
// This one emits a fact for each textual "gets(" occurrence so the example is dependency-free.
class TextualGetsAdapter final : public SemanticAdapter {
public:
    void analyze(const SourceStore& sources,
                 FactStore& facts,
                 DiagnosticSink&,
                 const RunOptions&) const override {
        for (const auto& [path, buffer] : sources.files()) {
            std::size_t pos = 0;
            while ((pos = buffer.text().find("gets(", pos)) != std::string::npos) {
                facts.add("c.call", "site:" + path + ":" + std::to_string(pos), "callee.name", "gets",
                          SourceRange{path, pos, pos + 4}, {{"adapter", "textual-example"}});
                pos += 4;
            }
        }
    }
};

class ReportGetsRule final : public Rule {
public:
    std::string id() const override { return "example.report-gets"; }
    std::string summary() const override { return "Report gets() call sites."; }
    std::vector<std::string> targets() const override { return {"unsafe-c-api"}; }

    void run(RuleContext& ctx) const override {
        for (const Fact* fact : ctx.facts().where([](const Fact& f) {
                 return f.kind == "c.call" && f.predicate == "callee.name" && f.object == "gets";
             })) {
            std::vector<GuardResult> guards{
                GuardResult{"fact_has_source_range", fact->range ? GuardStatus::Passed : GuardStatus::Failed, "", {}}};

            auto& ev = ctx.plan().evidence_for(id(), fact->subject, "gets() is unsafe and requires migration.",
                                               fact->range, {fact->id}, guards);
            Finding finding;
            finding.rule_id = id();
            finding.title = "unsafe gets() call";
            finding.message = "Replace gets() with a bounded input API and explicit buffer length.";
            finding.severity = Severity::Warning;
            finding.confidence = Confidence::High;
            finding.range = fact->range;
            finding.evidence_id = ev.id;
            ctx.plan().add_finding(std::move(finding));
        }
    }
};

class ExampleCapsule final : public Capsule {
public:
    std::string id() const override { return "example.unsafe-c-api"; }
    std::string name() const override { return "Unsafe C API example capsule"; }
    std::string version() const override { return "0.1.0"; }
    std::vector<std::string> targets() const override { return {"unsafe-c-api"}; }
    void register_rules(RuleRegistry& registry) const override {
        registry.add(std::make_unique<ReportGetsRule>());
    }
};

int main() {
    SourceStore sources;
    sources.add("demo.c", "#include <stdio.h>\nvoid f(char* p) { gets(p); }\n");

    Engine engine;
    engine.set_adapter(std::make_shared<TextualGetsAdapter>());
    engine.add_capsule(std::make_shared<ExampleCapsule>());

    RunOptions options;
    options.target = "unsafe-c-api";
    auto result = engine.run(sources, options);
    std::cout << plan_to_json(result.plan, &result.facts) << "\n";
    return result.plan.has_errors() ? 1 : 0;
}
