# Phase 31 Roadmap — cross-file inline call-site ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 31 closes the source-file ownership gap left by Phase 30. Inline source contexts still describe one immutable physical snapshot frame, but their call sites no longer inherit the physical crash-line filename when the compiler says the inline call crosses source files.

## Completed acceptance

- A genuine `-O2 -g -gdwarf-4` optimized fixture places `inline_outer` in the implementation file and `inline_inner` in a header. The permanent GCC and Clang-large lanes independently produce the executable and real kernel core; `tests/core_inline_dwarf_oracle.py` first proves compiler-produced inline DIE/range/abstract-origin/call-site metadata rather than accepting a synthetic DWARF fixture.
- The evidence requires distinct nonzero compiler `DW_AT_call_file` indices and validates the compiler's own inline source chain before mini-debugger product assertions run. No source pathname is injected into production from the test.
- `DwarfLineTable` retains the resolved file table for each parsed line-table unit instead of flattening away that ownership after row construction. Emitted address ranges carry their owning file-table identity.
- File-index resolution reuses the existing line-table semantics: DWARF4 indices remain one-based with zero invalid, while the already-supported DWARF5 table behavior remains direct and bounded. `DW_LNE_define_file` additions are retained before the unit table is sealed.
- `find_virtual_file()` first resolves the physical PC to exactly one owning line-table unit and only then indexes that unit's retained file table. Missing address ownership, out-of-range file indices, or multiple line-table units claiming the address do not fall back to filesystem search or basename matching.
- `discover_inline_call_chain()` consumes each active inline DIE's actual `DW_AT_call_file`, `DW_AT_call_line`, and optional `DW_AT_call_column`. It no longer copies the physical line-table filename into every inline context.
- The direct `CoreInspectionSession::inline_contexts()` integration proves the compiler-owned outer call site resolves to `inline_core_fixture.c` while the nested inner call site resolves to `inline_core_fixture.h`, preserving outer-to-inner depth and physical-frame module ownership.
- The real `mdbg-core inline` workflow is exercised against the genuine kernel core and reports the same compiler-owned cross-file call-site files. API and CLI therefore consume one line-table ownership path rather than parallel rendering heuristics.
- All Phase 30 invariants remain intact: inline contexts do not become physical unwind frames, nesting stays bounded, ambiguous sibling/range ownership fails closed, frame/thread selection invalidates stale inline selection, and scoped local discovery remains tied to the selected inline DIE.
- Test-first compiler evidence failed at the previous implementation because `mdbg-core` reported the physical `.h` source for `inline_outer`; exact implementation head `d67bfe6399991e4c4ce76cd13315af479e95f1d4` then passed the complete permanent GCC + Clang-large matrix in workflow run #1200.

Phase 31 is intentionally sealed here. More file-count variants, arbitrary header nesting, speculative DWARF5 cross-file fixtures, or pathname-normalization heuristics are not reasons to keep farming this phase. New compiler forms require a concrete failing artifact and must preserve owning-unit semantics.

## Phase 32 promotion — bounded inline local value materialization

The next higher-value optimized-core gap is already explicit in the product contract. A selected inline context can enumerate its active locals, but `CoreInspectionSession::inspect_value()` deliberately rejects every inline selection through `require_physical_value_context()`. The debugger therefore knows that an inline-local binding exists without being able to materialize even a compiler-supported scalar value from the immutable core state.

The first coherent Phase 32 slice must:

1. Start from a genuine GCC and Clang optimized inline artifact in which at least one selected inline local has a compiler-produced location expression already within mini-debugger's bounded value machinery. Do not hand-author a convenient location expression or assume an optimized-out value exists.
2. Keep machine-state ownership physical. Inline selection may choose source/DIE scope, but register and memory reads must continue to come from the selected immutable physical snapshot frame; do not invent an inline stack pointer, CFA, register file, or unwind frame.
3. Resolve the requested name only inside the selected inline DIE and its active lexical descendants, preserving abstract-origin names, current shadowing rules, bounded ranges, and fail-closed equal-depth ambiguity. A physical-frame local with the same spelling may not satisfy an inline request.
4. Materialize only compiler-proven scalar values whose location expression and storage source are already supported by the existing snapshot evaluator. Missing/optimized-out locations, unsupported expressions, unavailable registers, and unreadable memory remain explicit errors rather than guessed values.
5. Make `CoreInspectionSession::inspect_value()` and the real `mdbg-core print <name>` workflow consume the selected inline context when one is active. `inline physical`, frame selection, and thread selection must continue to invalidate inline ownership and restore physical materialization semantics.
6. Prove at least one shadowed inline binding produces the value belonging to the selected inline depth, not the physical or sibling binding, while the same genuine core continues to pass physical `locals`/`print` behavior.
7. Keep pointer dereference, aggregate traversal, arbitrary expression evaluation, and broader optimizer-location coverage out of the first slice unless the same compiler artifact requires them to prove scalar materialization.
8. Require permanent GCC + Clang-large evidence only if both compilers stably retain a suitable inline scalar location. If either compiler optimizes the value away or emits an unsupported form, stop and use that artifact as architecture evidence rather than fabricating a value.

This promotion advances optimized post-mortem inspection from source-level inline identity to bounded value access while preserving the central invariant that inlining creates source contexts, not fictional machine frames.
