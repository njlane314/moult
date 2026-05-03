#include "moult/core/api.hpp"

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

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

class RenameTranspiler final : public Transpiler {
public:
    std::string id() const override { return "test.transpiler.rename"; }
    std::string name() const override { return "Test rename transpiler"; }
    std::string version() const override { return "0.0.1"; }

    void run(const SourceStore& sources, TranspilerSink& sink, DiagnosticSink&, const RunOptions&) const override {
        const SourceBuffer* source = sources.get("a.c");
        assert(source);
        const std::size_t pos = source->text().find("old_api");
        assert(pos != std::string::npos);

        TranspilerEdit edit;
        edit.tool = id();
        edit.rule_id = "test.transpiler.old-api-to-new-api";
        edit.subject = "site:a.c:old_api";
        edit.range = SourceRange{"a.c", pos, pos + 7};
        edit.replacement = "new_api";
        edit.rationale = "transpiler resolved old_api and rendered new_api";
        edit.confidence = Confidence::Proven;
        sink.propose_edit(std::move(edit));
    }
};

static void test_transpiler_bridge_accepts_precise_edit() {
    SourceStore sources;
    sources.add("a.c", "old_api();\n");

    auto adapter = std::make_shared<TranspilerAdapter>();
    adapter->add_transpiler(std::make_shared<RenameTranspiler>());

    Engine engine;
    engine.set_adapter(adapter);
    engine.add_capsule(std::make_shared<TranspilerCapsule>());

    RunOptions options;
    options.target = "test";
    options.minimum_confidence = Confidence::High;
    auto result = engine.run(sources, options);

    assert(result.facts.by_kind(transpiler_edit_fact_kind).size() == 1);
    assert(result.plan.accepted_edit_count() == 1);
    assert(result.plan.findings.empty());
    assert(result.plan.evidence.size() == 1);

    auto applied = result.plan.edits.apply_to_memory(sources);
    assert(applied.at("a.c") == "new_api();\n");
}

class FileRewriteTranspiler final : public Transpiler {
public:
    std::string id() const override { return "test.transpiler.file-rewrite"; }

    void run(const SourceStore& sources, TranspilerSink& sink, DiagnosticSink&, const RunOptions&) const override {
        assert(sources.get("b.txt"));
        TranspilerFileRewrite rewrite;
        rewrite.tool = id();
        rewrite.rule_id = "test.transpiler.rewrite-token";
        rewrite.file = "b.txt";
        rewrite.rewritten_text = "alpha BETA gamma\n";
        rewrite.rationale = "opaque transpiler returned a full rewritten file";
        rewrite.confidence = Confidence::Proven;
        sink.propose_file_rewrite(std::move(rewrite));
    }
};

static void test_transpiler_bridge_reduces_file_rewrite_to_edit() {
    SourceStore sources;
    sources.add("b.txt", "alpha beta gamma\n");

    auto adapter = std::make_shared<TranspilerAdapter>();
    adapter->add_transpiler(std::make_shared<FileRewriteTranspiler>());

    Engine engine;
    engine.set_adapter(adapter);
    engine.add_capsule(std::make_shared<TranspilerCapsule>());

    RunOptions options;
    options.target = "test";
    options.minimum_confidence = Confidence::High;
    auto result = engine.run(sources, options);

    assert(result.plan.accepted_edit_count() == 1);
    auto applied = result.plan.edits.apply_to_memory(sources);
    assert(applied.at("b.txt") == "alpha BETA gamma\n");
}

class LowConfidenceTranspiler final : public Transpiler {
public:
    std::string id() const override { return "test.transpiler.low-confidence"; }

    void run(const SourceStore& sources, TranspilerSink& sink, DiagnosticSink&, const RunOptions&) const override {
        assert(sources.get("c.c"));

        TranspilerEdit edit;
        edit.tool = id();
        edit.rule_id = "test.transpiler.review-only";
        edit.range = SourceRange{"c.c", 0, 3};
        edit.replacement = "new";
        edit.rationale = "translator could not prove semantic equivalence";
        edit.confidence = Confidence::Medium;
        sink.propose_edit(std::move(edit));
    }
};

static void test_transpiler_bridge_demotes_low_confidence_edit() {
    SourceStore sources;
    sources.add("c.c", "old();\n");

    auto adapter = std::make_shared<TranspilerAdapter>();
    adapter->add_transpiler(std::make_shared<LowConfidenceTranspiler>());

    Engine engine;
    engine.set_adapter(adapter);
    engine.add_capsule(std::make_shared<TranspilerCapsule>());

    RunOptions options;
    options.target = "test";
    options.minimum_confidence = Confidence::High;
    auto result = engine.run(sources, options);

    assert(result.plan.accepted_edit_count() == 0);
    assert(result.plan.findings.size() == 1);
    assert(result.plan.findings.front().rule_id == "test.transpiler.review-only");
    assert(result.plan.findings.front().message.find("minimum_confidence") != std::string::npos);
}

int main() {
    test_source_line_columns();
    test_edit_apply();
    test_edit_conflict();
    test_engine();
    test_transpiler_bridge_accepts_precise_edit();
    test_transpiler_bridge_reduces_file_rewrite_to_edit();
    test_transpiler_bridge_demotes_low_confidence_edit();
    std::cout << "moult-core tests passed\n";
}
