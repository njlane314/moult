#include "moult/core/api.hpp"

#include <cassert>
#include <iostream>
#include <memory>

using namespace moult::core;

static void test_source_line_columns() {
    SourceBuffer b("a.c", "one\ntwo\nthree");
    assert(b.line_column(0).line == 1);
    assert(b.line_column(0).column == 1);
    assert(b.line_column(4).line == 2);
    assert(b.line_column(4).column == 1);
    assert(b.slice(0, 3) == "one");
}

static void test_edit_apply() {
    SourceStore s;
    s.add("a.c", "int main(){ return 0; }\n");
    EditSet edits;
    TextEdit e;
    e.rule_id = "test";
    e.range = SourceRange{"a.c", 12, 18};
    e.replacement = "return 1";
    assert(edits.add(e, &s).outcome == EditAddOutcome::Accepted);
    auto out = edits.apply_to_memory(s);
    assert(out.at("a.c") == "int main(){ return 1; }\n");
}

static void test_edit_conflict() {
    SourceStore s;
    s.add("a.c", "abcdef");
    EditSet edits;
    TextEdit a;
    a.rule_id = "r1";
    a.range = SourceRange{"a.c", 1, 3};
    a.replacement = "XX";
    TextEdit b;
    b.rule_id = "r2";
    b.range = SourceRange{"a.c", 2, 4};
    b.replacement = "YY";
    assert(edits.add(a, &s).outcome == EditAddOutcome::Accepted);
    assert(edits.add(b, &s).outcome == EditAddOutcome::Conflict);
    assert(edits.has_conflicts());
}

class TestAdapter final : public SemanticAdapter {
public:
    void analyze(const SourceStore& sources, FactStore& facts, DiagnosticSink&, const RunOptions&) const override {
        const auto* f = sources.get("a.c");
        assert(f);
        facts.add("symbol.reference", "site:a.c:0", "resolves_to", "old_symbol", SourceRange{"a.c", 0, 3});
    }
};

class RenameRule final : public Rule {
public:
    std::string id() const override { return "test.rename"; }
    std::string summary() const override { return "Rename old_symbol to new_symbol."; }
    std::vector<std::string> targets() const override { return {"test"}; }
    void run(RuleContext& ctx) const override {
        for (const Fact* fact : ctx.facts().where([](const Fact& f) {
                 return f.kind == "symbol.reference" && f.predicate == "resolves_to" && f.object == "old_symbol";
             })) {
            assert(fact->range);
            auto& ev = ctx.plan().evidence_for(id(), fact->subject, "resolved symbol can be renamed", fact->range,
                                               {fact->id}, {GuardResult{"semantic_resolution", GuardStatus::Passed, "", {}}});
            auto edit = ctx.plan().make_edit(id(), *fact->range, "new_symbol", ev.id, Confidence::High);
            auto outcome = ctx.plan().add_edit(std::move(edit));
            assert(outcome.outcome == EditAddOutcome::Accepted);
        }
    }
};

class TestCapsule final : public Capsule {
public:
    std::string id() const override { return "test.capsule"; }
    std::string name() const override { return "Test Capsule"; }
    std::string version() const override { return "0.0.1"; }
    std::vector<std::string> targets() const override { return {"test"}; }
    void register_rules(RuleRegistry& registry) const override {
        registry.add(std::make_unique<RenameRule>());
    }
};

static void test_engine() {
    SourceStore sources;
    sources.add("a.c", "old_symbol();\n");
    Engine engine;
    engine.set_adapter(std::make_shared<TestAdapter>());
    engine.add_capsule(std::make_shared<TestCapsule>());
    RunOptions options;
    options.target = "test";
    auto result = engine.run(sources, options);
    assert(result.plan.accepted_edit_count() == 1);
    assert(result.plan.evidence.size() == 1);
    auto applied = result.plan.edits.apply_to_memory(sources);
    assert(applied.at("a.c") == "new_symbol();\n");
    auto json = plan_to_json(result.plan, &result.facts);
    assert(json.find("test.rename") != std::string::npos);
}

int main() {
    test_source_line_columns();
    test_edit_apply();
    test_edit_conflict();
    test_engine();
    std::cout << "moult-core tests passed\n";
}
