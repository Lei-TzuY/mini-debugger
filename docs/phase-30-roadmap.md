# Phase 30 Roadmap — bounded inline call-site reconstruction

Status: complete for the current same-translation-unit Linux x86-64 genuine-core milestone.

Phase 30 promotes optimized source identity above one physical immutable machine frame without inventing ptrace frames, stack pointers, register files, or unwind state for compiler inlining. The implemented surface is evidence-gated by genuine GCC and Clang optimized artifacts and keeps source-level inline ownership distinct from physical CFI unwind state.

## Completed acceptance

- A genuine `-O2 -g -gdwarf-4` fixture is compiled independently in the permanent GCC and Clang-large lanes, for PIE and non-PIE forms. `tests/core_inline_dwarf_oracle.py` verifies compiler-produced `DW_TAG_inlined_subroutine`, abstract-origin, range, and call-site metadata before product assertions run; no synthetic DIEs are used.
- `discover_inline_call_chain()` attaches a bounded source-level inline chain to exactly one selected immutable snapshot frame. It does not create physical frames or infer separate stack/register state for inline calls.
- Active inline instances are resolved through compiler-produced low/high-PC or bounded DWARF4 `.debug_ranges` ownership. Chains are capped at eight contexts and ambiguous sibling/non-contiguous ownership fails closed.
- Inline names are resolved through existing abstract-origin machinery. Call-site line/column metadata is retained on each `InlineCallsiteContext`; module ownership remains that of the selected physical frame.
- `CoreInspectionSession` exposes the discovered chain and explicit inline selection. Selecting a physical frame or thread invalidates stale inline selection; selecting `physical` returns to the physical-frame scope without modifying unwind state.
- Scoped `locals()` dispatches to the selected inline DIE when an inline context is active. The optimized fixture proves separate physical/outer/inner catalogues, nested ownership, and one deepest active `shadow_value` binding rather than flattening inline descendants into the physical scope.
- Inline local discovery remains discovery-only. `inspect_value()` while an inline context is selected is deliberately rejected in this milestone; Phase 30 does not fabricate value materialization from physical-frame state without a separately proven ownership rule.
- The real `mdbg-core` workflow exposes `inline`, `inline <index>`, and `inline physical`, and the integration proves that visible context/local state follows inline, frame, and thread selection changes.
- The complete permanent GCC + Clang-large matrix passed at exact implementation head `8c052c4ba8ffb154837b4246859840ea6c6d8722` before this phase seal.

## Explicit boundary discovered during seal

The current compiler fixture keeps both inline functions and their callers in one source file. Therefore this milestone does **not** claim cross-file/header inline call-site file ownership. An attempted follow-up added line-table file-index API declarations without the corresponding parser/storage implementation and failed the compiler gate; that incomplete change was removed rather than being papered over with defaults.

For the current same-file artifact, the physical line-table source file and compiler call-site file coincide, so the demonstrated call-site source identity is valid within this bounded milestone. A header-defined inline function whose `DW_AT_call_file` points at a different line-table file entry requires independent compiler evidence and a real line-table file-index mapping path.

Phase 30 is intentionally sealed here. More inline depths, sibling variants, speculative DWARF5 range forms, or arbitrary inline value evaluation are not reasons to keep farming this phase.

## Phase 31 promotion — cross-file inline call-site ownership

The next concrete optimized-debugging gap is source-file ownership when an inline chain crosses translation-unit source files, such as always-inlined functions defined in a header and called from a separate implementation file.

The first coherent Phase 31 slice must:

1. Start from genuine GCC and Clang optimized evidence where at least one active `DW_TAG_inlined_subroutine` has `DW_AT_call_file` different from the physical crash line's file entry; do not hand-author DWARF.
2. Resolve `DW_AT_call_file` through the exact line-table unit/file table that owns the physical PC, preserving DWARF4 versus DWARF5 index semantics explicitly rather than guessing from path names.
3. Keep the current bounded inline chain, module ownership, frame/thread invalidation, and local-scope rules unchanged.
4. Prove through API and real `mdbg-core inline` output that each inline call site reports the compiler-owned file/line rather than copying the physical line-table file.
5. Fail closed on missing, out-of-range, ambiguous, or unsupported file-table ownership. No filesystem search or basename matching may substitute for DWARF ownership.
6. Require the permanent GCC + Clang-large evidence only if both compilers produce a stable cross-file inline artifact; otherwise stop and audit the next architecture frontier rather than fabricating metadata.

This promotion keeps optimized source identity evidence-driven while making the next limitation explicit and executable.