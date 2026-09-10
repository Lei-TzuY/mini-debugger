# Phase 29 Roadmap — bounded scoped local discovery

Status: complete for the current Linux x86-64 genuine-core milestone.

Phase 29 closes the post-mortem discoverability gap promoted from Phase 28. `mdbg-core` no longer requires the user to know every source-level name in advance: the selected immutable thread/frame can expose a bounded catalogue of active variables and formal parameters without materializing their values.

## Completed acceptance

- Discovery runs on genuine GCC and Clang-large core artifacts in the permanent CI lanes, for both PIE and non-PIE executables. The implementation consumes compiler-produced DWARF4/5 DIEs; it does not introduce synthetic DIEs or a parallel symbol database.
- `discover_local_values()` resolves the selected frame lookup PC to its owning module and compilation unit, then enumerates named `DW_TAG_variable` and `DW_TAG_formal_parameter` DIEs only inside the uniquely covering `DW_TAG_subprogram`.
- Existing abstract-origin and lexical-scope machinery remains authoritative. A nested compiler-produced `shadow_value` fixture proves that only one name is discovered at the crash PC and that independent `inspect_value("shadow_value")` resolution selects the deeper active binding. Equal-depth duplicate names remain fail-closed rather than being chosen arbitrarily.
- Discovery and materialization remain separate contracts. `LocalDiscoveryEntry` carries only the source name and bounded declaration kind (`Variable` or `FormalParameter`); `locals` does not evaluate a location expression, read snapshot memory/registers for the value, dereference pointers, or recursively expand objects.
- The active catalogue is capped at 64 unique names and is emitted in deterministic name order. Once the owning subprogram is found, discovery does not recursively walk unrelated compilation units or object graphs.
- `CoreInspectionSession::locals()` derives every result from the currently selected immutable frame. Genuine-core integration proves that selecting the caller frame replaces callee-only locals with `caller_stack_local`, and selecting the sibling thread resets to frame zero and produces a different catalogue without stale crash-thread names.
- The same sibling compiler artifact proves formal-parameter discovery: `seed` is classified as `FormalParameter`, while `xmm_value` remains a variable.
- The real `mdbg-core` product surface exposes `locals`. It delegates directly to `CoreInspectionSession::locals()` and prints only `parameter <name>` / `variable <name>`. A subprocess integration runs `locals -> frame 1 -> locals -> thread <sibling> -> locals` and proves the three catalogues follow selection changes.
- The large historical snapshot lookup implementation is isolated into `source_lookup_snapshot_impl.inc`; this is a source-organization extraction, not a second lookup engine. The existing source/value behavior remains covered by the full regression suite.

Phase 29 is intentionally sealed here. Adding availability guesses, eager value materialization, recursive object expansion, or a general expression evaluator would change the contract and requires an independent architecture decision rather than more scoped-name variants.

## Phase 30 promotion — bounded inlined-callsite reconstruction

The next higher-value post-mortem gap is optimized-code source identity. Current snapshot inspection and scoped discovery anchor an immutable machine frame to one physical `DW_TAG_subprogram`; the codebase does not yet model compiler-produced `DW_TAG_inlined_subroutine` instances. Real optimized cores can therefore contain an inline call chain whose source scope and locals are not represented as first-class immutable inspection contexts.

The first coherent Phase 30 slice must:

1. Start from a genuine GCC and Clang optimized fixture whose crash PC is covered by a compiler-produced `DW_TAG_inlined_subroutine`. Use `readelf`/equivalent compiler-artifact evidence for ranges, abstract origin, and call-site metadata; do not hand-author DWARF.
2. Represent the inline chain as bounded source-level contexts attached to one physical snapshot frame. Do not invent ptrace frames, stack pointers, or register states for inline calls.
3. Resolve abstract-origin names and source locations through existing DWARF machinery, with a hard nesting bound and fail-closed handling for ambiguous or unsupported inline ranges.
4. Keep immutable thread/frame ownership explicit. Switching physical frame or thread must invalidate any previously selected inline context rather than leaking stale source scope.
5. Make scoped local discovery consume the selected inline context when one is selected, so caller/inlined locals obey the same lexical-shadowing rules instead of being merged into one flat catalogue.
6. Expose one bounded `mdbg-core` workflow that makes the inline chain visible and selectable without turning `bt` into a fake physical unwind.
7. Preserve all Phase 29 bounds: no eager local materialization, recursive object traversal, arbitrary expression evaluation, or unbounded compilation-unit scanning.
8. Require permanent GCC + Clang-large evidence only if both compilers produce a stable inline scenario. If they do not, stop and audit the next architectural frontier rather than fabricating metadata.

This promotes immutable core inspection from physical-frame scoped discovery to compiler-proven optimized source contexts while preserving the distinction between machine unwind state and source-level inline structure.
