# moult-core

`moult-core` is a small C++20 library for building semantic code-migration engines.

It is deliberately **not** tied to Clang, Tree-sitter, CodeQL, an LLM provider, GitHub, or a particular language. Those components should sit above or beside this core as adapters. The core owns the migration data model: facts, guarded evidence, findings, precise text edits, conflict detection, plans, and serialization.

The intended architecture is:

```text
language/build adapter
  Clang, CodeQL export, tree-sitter, proprietary analyser
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
TranspilerBridge  wraps external source translators as guarded Moult proposals
Serializers       plan.json, evidence.jsonl, facts.json, basic SARIF
```

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## C++ modernisation helper

The `moult` CLI includes an initial `cpp-modernisation` target:

```bash
build/moult plan --target cpp-modernisation path/to/file.cpp
build/moult plan --target cpp-modernisation --out .moult path/to/src
build/moult plan --adapter clang --clang-arg -Iinclude path/to/file.cpp
build/moult plan --adapter clang --compile-commands build path/to/src
build/moult plan --compile-commands build --out .moult
build/moult plan path/to/src --exclude 'third_party/**' --include '**/*.cpp'
build/moult plan path/to/src --exclude-vendored --out .moult
build/moult plan --adapter textual path/to/file.cpp
build/moult report .moult/plan.json
build/moult report --summary-only --limit 20 .moult/plan.json
build/moult report --rule 'modernise.use-nullptr' --file 'src/util/**' .moult/plan.json
build/moult slice --rule 'modernise.use-nullptr' --file 'src/util/**' --out util-nullptr.plan.json .moult/plan.json
build/moult diff .moult/plan.json
build/moult review .moult
build/moult apply --backup .moult/plan.json
```

When libclang is available at configure time, the CLI builds a Clang-based adapter
and uses it by default. The Clang adapter parses each source file, emits semantic
facts for translation units, functions, declarations, calls, macro expansions, and
selected C++ constructs, then emits modernisation opportunity facts consumed by
the shared rule capsule. Extra parser arguments can be passed with repeated
`--clang-arg` options. `--compile-commands` accepts either a
`compile_commands.json` file or a build directory containing one. When source
paths are omitted, Moult uses the translation units listed in the compile
database as its input set and normalises relative include/source paths against
each entry's compile directory.

Use repeated `--include <glob>` and `--exclude <glob>` options to narrow large
scans. Exclude patterns win over include patterns. Relative patterns match at
any depth, so `src/vendor/**`, `vendor/**`, and `*.cpp` are practical forms for
repo-scale scoping. `--exclude-vendored` adds a conservative built-in set of
common vendored dependency directories such as `third_party/**`, `vendor/**`,
`external/**`, `deps/**`, and known C/C++ library subtrees such as
`leveldb/**`, `secp256k1/**`, and `univalue/**`.

The fallback textual adapter is still available with `--adapter textual`. It skips
comments and string/character literals, emits modernisation opportunity facts,
and then builds the same reviewable plan with evidence, findings, and
conflict-checked edits.

`apply` reads a Moult-generated `plan.json` and applies accepted, conflict-free
edits to disk. Use `--dry-run` to preview the files that would change, or
`--backup` to write `<file>.moult.bak` before each file is modified.

`report` reads the same `plan.json` and prints a human-readable summary of
planned edits, manual-review findings, conflicts, and diagnostics. When the
referenced source files are still present, report locations include line and
column numbers as well as byte ranges. Reports also include grouped summaries by
rule, directory, and file, plus a recommended next action for reviewing,
scoping, or applying the plan. Use `--summary-only` for large repositories,
`--limit <count>` to control grouped summary length, and repeated `--rule` or
`--file` globs to focus the report on a subsystem or migration rule.

`slice` makes report filtering actionable by writing a new focused `plan.json`
from an existing plan. The sliced plan preserves plan metadata, keeps only
matching edits, findings, conflicts, and diagnostics, and prunes unreferenced
evidence and facts. Use it before `diff`, `review`, or `apply` when you want to
migrate one rule or subsystem at a time.

`diff` renders accepted edits as a git-style unified diff and appends
comment-prefixed manual-review suggestions. Only the accepted-edit portion is a
patch; manual-review entries are advisory because Moult has not produced safe
replacement text for them.

`review` opens an interactive terminal review UI for a `plan.json` path or a
Moult output directory. It lets you move through edits, manual-review findings,
conflicts, diagnostics, evidence, and guard results; mark items accepted,
rejected, or unset; save `review.json`; and export `plan.reviewed.json` with
rejected edits removed. For automatic edits, the selected item includes a
focused git-style patch hunk, with colour used for item types, confidence,
decisions, and diff additions/removals.

When scanning directories or compile databases, Moult classifies source
language before loading files into the C++ modernisation target. Plain C files
such as `.c`, C ABI headers guarded with `extern "C"`, and plain C-looking
headers are skipped by default. During directory scans, `.h` files in C-only
directories are also treated as C-side headers. Compile commands that explicitly
compile a `.c` file as C++ still opt that file into C++ analysis.

Currently supported checks:

```text
NULL           planned edit to nullptr
throw()        planned edit to noexcept
register       planned removal of the obsolete storage-class specifier
std::auto_ptr  manual-review finding for migration to std::unique_ptr
typedef        manual-review finding for migration to using aliases
new/delete     manual-review findings for RAII ownership
C-style casts  manual-review finding for replacement with named C++ casts
```

`std::auto_ptr` is intentionally reported without an automatic edit because its
ownership-transfer behaviour often requires manual review.

## Transpiler bridge

Moult can now host in-process or external source translators through the core
`Transpiler` API. A translator reports candidate rewrites to `TranspilerSink`
instead of mutating files directly. `TranspilerCapsule` then turns those
proposals into normal Moult evidence, findings, and conflict-checked edits.

This is useful for staged migrations such as Fortran-to-C++ or C-to-Rust, where
some rewrites can be accepted mechanically but other translated sites need
manual review. A bridge can submit either precise `TranspilerEdit` ranges or a
full `TranspilerFileRewrite`; full-file rewrites are reduced to the smallest
single byte-range edit before they enter the plan.

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

For C/C++ migration, the Clang layer should be an adapter rather than the whole product. It is responsible for compile-command ingestion, AST traversal, symbol resolution, macro/source-location normalisation, and fact emission. The core library remains stable while individual engines evolve.

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
