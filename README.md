# moult-core

`moult-core` is a small C++20 library for building semantic code-migration engines.

It is deliberately **not** tied to Clang, Tree-sitter, CodeQL, an LLM provider, GitHub, or a particular language. Those components should sit above or beside this core as adapters. The core owns the migration data model: facts, guarded evidence, findings, precise text edits, conflict detection, plans, and serialization.

The intended architecture is:

```text
language/build adapter
  Clang, CodeQL export, tree-sitter, proprietary analyzer
        ↓ emits
FactStore
        ↓ consumed by
Capsules and Rules
        ↓ produce
EvidenceRecord + Finding + TextEdit
        ↓ assembled into
Plan
        ↓ serialized/applied/verified by product layer
```

This lets you build several migration engines over the same substrate:

```text
OpenSSL migration engine
unsafe C API remediation engine
Qt migration engine
compiler-upgrade engine
MISRA/CERT repair engine
C-to-Rust extraction planner
```

The core is intentionally deterministic. An LLM agent can propose capsules, interpret failed builds, or draft reports, but the actual planned edits should flow through facts, guards, and conflict-checked edit sets.

## What the library provides

```text
SourceStore       in-memory source buffers with byte ranges and line/column mapping
FactStore         language-neutral semantic facts emitted by adapters
EvidenceRecord    proof records with guard results and provenance fact IDs
Finding           reviewable issue/report item
TextEdit/EditSet  precise byte-range replacements with conflict detection
Plan              complete migration plan: edits, findings, evidence, groups, diagnostics
Capsule/Rule      plugin-style migration API
Engine            adapter + capsule execution host
Serializers       plan.json, evidence.jsonl, facts.json, basic SARIF
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## C++ modernization helper

The `moult` CLI includes an initial `cpp-modernization` target:

```bash
build/moult plan --target cpp-modernization path/to/file.cpp
build/moult plan --target cpp-modernization --out .moult path/to/src
build/moult plan --adapter clang --clang-arg -Iinclude path/to/file.cpp
build/moult plan --adapter clang --compile-commands build path/to/src
build/moult plan --adapter textual path/to/file.cpp
build/moult apply --backup .moult/plan.json
```

When libclang is available at configure time, the CLI builds a Clang-based adapter
and uses it by default. The Clang adapter parses each source file, emits semantic
facts for translation units, functions, declarations, calls, macro expansions, and
selected C++ constructs, then emits modernization opportunity facts consumed by
the shared rule capsule. Extra parser arguments can be passed with repeated
`--clang-arg` options.

The fallback textual adapter is still available with `--adapter textual`. It skips
comments and string/character literals, emits modernization opportunity facts,
and then builds the same reviewable plan with evidence, findings, and
conflict-checked edits.

`apply` reads a Moult-generated `plan.json` and applies accepted, conflict-free
edits to disk. Use `--dry-run` to preview the files that would change, or
`--backup` to write `<file>.moult.bak` before each file is modified.

Currently supported checks:

```text
NULL           planned edit to nullptr
throw()        planned edit to noexcept
register       planned removal of the obsolete storage-class specifier
std::auto_ptr  manual-review finding for migration to std::unique_ptr
typedef        manual-review finding for migration to using aliases
new/delete     manual-review findings for RAII ownership
C-style casts  manual-review finding when using the Clang adapter
```

`std::auto_ptr` is intentionally reported without an automatic edit because its
ownership-transfer behavior often requires manual review.

## Minimal usage

```cpp
#include "moult/core/api.hpp"

using namespace moult::core;

class MyAdapter final : public SemanticAdapter {
public:
    void analyze(const SourceStore& sources,
                 FactStore& facts,
                 DiagnosticSink& diagnostics,
                 const RunOptions& options) const override {
        // A real adapter would use Clang, CodeQL, tree-sitter, etc.
        // Emit stable facts that rules can consume.
        (void)sources; (void)diagnostics; (void)options;
        facts.add("symbol.reference", "site:1", "resolves_to", "old_api");
    }
};
```

A rule consumes facts and emits evidence plus edits:

```cpp
class RenameRule final : public Rule {
public:
    std::string id() const override { return "vendor.old-api-to-new-api"; }
    std::string summary() const override { return "Rename old_api to new_api."; }

    void run(RuleContext& ctx) const override {
        for (const Fact* fact : ctx.facts().where([](const Fact& f) {
            return f.predicate == "resolves_to" && f.object == "old_api";
        })) {
            if (!fact->range) continue;

            auto& ev = ctx.plan().evidence_for(
                id(), fact->subject, "semantic reference resolves to old_api", fact->range,
                {fact->id}, {GuardResult{"semantic_resolution", GuardStatus::Passed, "", {}}});

            auto edit = ctx.plan().make_edit(
                id(), *fact->range, "new_api", ev.id, Confidence::High);
            ctx.plan().add_edit(std::move(edit));
        }
    }
};
```

## Why this split matters

For C/C++ migration, the Clang layer should be an adapter rather than the whole product. It is responsible for compile-command ingestion, AST traversal, symbol resolution, macro/source-location normalization, and fact emission. The core library remains stable while individual engines evolve.

That yields this shape:

```text
moult-core/             stable library
moult-clang-adapter/    Clang-specific fact emitter
moult-openssl/          OpenSSL capsule
moult-unsafe-c/         unsafe API capsule
moult-agent/            LLM/tool orchestration layer
moult-cli/              user-facing command runner
```

## Output model

`Plan` is the central artifact. It can be serialized to:

```text
plan.json        full plan: edits, conflicts, findings, evidence, diagnostics
evidence.jsonl   one proof/evidence record per line
facts.json       facts emitted by adapters
findings.sarif   static-analysis-compatible findings scaffold
```

The core does not run builds, talk to Git, call LLMs, or mutate files on disk by default. Those are product-layer concerns.
