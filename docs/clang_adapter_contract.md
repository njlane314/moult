# Clang adapter contract

The Clang adapter should be a separate library that depends on both Clang and `moult-core`.

Its job is to turn C/C++ compilation facts into stable `Fact` records.

## Minimum emitted facts

For each relevant translation unit, emit facts like:

```text
translation_unit tu:<path> has_compile_command <id>
symbol.reference site:<file>:<offset> resolves_to <USR>
symbol.reference site:<file>:<offset> spelling <token-text>
symbol.declaration <USR> named <qualified-name>
symbol.declaration <USR> declared_in <header-path>
cxx.call call:<file>:<offset> callee <USR>
cxx.call call:<file>:<offset> has_callee_range <range-id>
macro.expansion site:<file>:<offset> spelling_file <path>
macro.expansion site:<file>:<offset> expansion_file <path>
```

Store detailed, adapter-specific metadata in `Fact::attributes`:

```text
clang_usr
qualified_name
declaration_file
is_macro_id
spelling_begin
spelling_end
expansion_begin
expansion_end
translation_unit
compile_command_hash
language_standard
target_triple
```

## Editing policy

The adapter should normalize source locations before constructing `SourceRange`:

```text
1. Prefer spelling ranges for token replacement.
2. Reject edits inside macro bodies unless the rule explicitly permits them.
3. Preserve expansion metadata in attributes for evidence.
4. Emit manual-review findings for ambiguous macro-origin sites.
```

## Rule behaviour

A capsule rule should not inspect Clang AST classes directly. It should inspect facts.

That keeps this separation:

```text
Clang adapter: semantic extraction
Capsule: migration decision
Core: evidence, plan, edits, conflicts, serialization
```
