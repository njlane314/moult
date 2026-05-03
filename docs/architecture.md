# Architecture

`moult-core` is built around one invariant: every migration edit should be explainable as a consequence of facts and guard results.

## Data flow

```text
SourceStore
    ↓
SemanticAdapter::analyze
    ↓
FactStore
    ↓
Capsule::register_rules → Rule::run
    ↓
PlanBuilder
    ↓
Plan { EvidenceRecord[], Finding[], EditSet, ChangeGroup[], Diagnostic[] }
```

## Source ranges

All core ranges are byte offsets:

```cpp
struct SourceRange {
    std::string file;
    std::size_t begin; // inclusive
    std::size_t end;   // exclusive
};
```

The core does not impose UTF-16, token, AST-node, or line/column coordinates. A language adapter may preserve those in attributes while emitting byte ranges for patching.

## Facts

Facts are intentionally small and stable:

```cpp
struct Fact {
    std::string kind;
    std::string subject;
    std::string predicate;
    std::string object;
    std::optional<SourceRange> range;
    Attributes attributes;
};
```

Examples:

```text
kind=symbol.reference subject=site:src/a.c:104 predicate=resolves_to object=openssl::EVP_MD_CTX_create
kind=cxx.call         subject=call:17          predicate=callee.usr object=c:@F@EVP_MD_CTX_create
kind=build.target     subject=libcrypto_user   predicate=links_to object=OpenSSL::Crypto
```

A Clang adapter can emit high-fidelity facts with USRs, declaration paths, macro-expansion metadata, template specialization details, and original spelling ranges in attributes.

## Evidence

Evidence records are first-class because migration systems fail when they hide uncertainty.

```cpp
struct EvidenceRecord {
    std::string rule_id;
    std::string subject;
    std::string rationale;
    std::vector<std::string> fact_ids;
    std::vector<GuardResult> guards;
};
```

A rule should create evidence before emitting an edit. If a guard fails, the rule should usually emit a `Finding` rather than an edit.

## Edits

`EditSet` accepts precise `TextEdit` objects and rejects overlapping edits. Identical duplicate edits are ignored. Conflicting edits are recorded rather than silently dropped.

Edits are applied per file in descending byte-offset order, which prevents earlier edits from shifting later ranges.

## Capsules

A capsule is a migration package. It registers one or more rules and declares the targets it supports.

```cpp
class Capsule {
public:
    virtual std::string id() const = 0;
    virtual std::vector<std::string> targets() const = 0;
    virtual void register_rules(RuleRegistry&) const = 0;
};
```

Examples:

```text
openssl-3-readiness
unsafe-c-api
qt6-port
cpp20-compiler-upgrade
cert-c-remediation
```

## Product layering

Do not put everything in the core. Keep these separate:

```text
Core library       migration data model and deterministic planning
Language adapter   Clang/Tree-sitter/CodeQL fact emission
Capsules           domain-specific rules
CLI                repo loading, output paths, verification command execution
Agent layer        LLM-assisted recipe authoring and compiler-error triage
UI                 reviewer workflow and migration reports
```
