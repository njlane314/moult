# Building engines on top of moult-core

An engine built on `moult-core` should usually be three libraries plus a CLI:

```text
moult-core              shared migration substrate
moult-clang-adapter     language/build-system semantic extraction
moult-<domain>          migration capsule rules
moult-cli               repository loading, output, verification, Git integration
```

## 1. Write an adapter

The adapter depends on the parser/analyzer and emits facts.

For a C/C++ Clang adapter, the first implementation should emit facts for:

```text
translation units
compile commands
function declarations
function references
call expressions
macro-origin metadata
include relationships
build targets if available
```

The adapter should not decide migration policy. It should expose enough facts for capsules to decide safely.

## 2. Write capsules

A capsule is domain-specific. Examples:

```text
openssl-3-readiness
qt6-port
unsafe-c-api
cert-c-remediation
```

Capsules register rules. Rules query facts, evaluate guards, and emit either findings or edits.

## 3. Keep guards explicit

Do not silently skip hard cases. A rule should classify each relevant site:

```text
auto-editable
manual-review
blocked-by-macro
blocked-by-missing-fact
blocked-by-low-confidence
```

This classification should appear in findings and evidence.

## 4. Let product code handle side effects

The core does not mutate repos, run builds, or push branches. The CLI/product layer should:

```text
load files
invoke adapter
invoke engine
write outputs
apply accepted edits to disk
run build/test/sanitizer commands
emit verification records
open PRs
```

## 5. LLM integration

Expose the engine to an LLM agent as deterministic tools:

```bash
moult scan --target openssl-3 --format json
moult plan --target openssl-3 --format json
moult apply --plan .moult/plan.json
moult explain --evidence-id evidence_...
```

The LLM should consume facts, plans, evidence, and diagnostics. It should not directly rewrite source when a guarded core rule can perform the edit.
