# Phase 32 Roadmap — bounded selected-inline scalar materialization

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 32 closes the first value-access gap left by Phases 30 and 31. A selected inline context may now choose the DWARF source/DIE scope used for scalar lookup without becoming a fictional machine frame: all register and memory ownership remains attached to the selected immutable physical snapshot frame.

## Completed acceptance

- A genuine `-O2 -g -gdwarf-4` optimized cross-file inline fixture is compiled independently in the permanent GCC and Clang-large lanes and produces a real kernel core. `tests/core_inline_dwarf_oracle.py` verifies compiler-produced inline DIEs, abstract origins, active ranges, call-site ownership, and the concrete inline-local location metadata before product assertions run. No DWARF location expression or scalar value is hand-authored in production or injected by the test.
- `CoreInspectionSession::inspect_value()` dispatches through the selected inline DIE only when an inline context is active. Physical-frame inspection continues to use the existing physical local-value path.
- Inline selection changes only source/DIE scope. The materializer reads immutable machine state from the selected physical snapshot frame and never invents an inline stack pointer, CFA, register file, unwind frame, or independent runtime PC.
- The first materialization slice is deliberately bounded to exact physical frame zero and bounded integer scalars of at most eight bytes. The genuine GCC/Clang artifact proves the required one-operation register locations, currently `DW_OP_reg1`/RDX and `DW_OP_reg2`/RCX; unsupported expressions, location forms, unavailable registers, and non-scalar types fail explicitly rather than being guessed.
- Location-list ownership reuses the existing bounded `active_location_expression()` machinery when the compiler emits a supported section/list form. Phase 32 does not add a parallel optimizer-expression evaluator.
- Requested names are resolved only within the selected inline DIE and its active lexical descendants. Abstract-origin names are preserved, the deepest active lexical binding wins, equal-depth ambiguity fails closed, and a physical-frame or sibling binding with the same spelling cannot satisfy the inline request.
- The genuine nested fixture proves the selected `inline_inner` shadow binding materializes as exactly `0x141 [4-byte signed]`. Direct `CoreInspectionSession::inspect_value("shadow_value")` and the real `mdbg-core print shadow_value` workflow consume the same selected-inline ownership path.
- `inline physical`, physical frame selection, and thread selection continue to invalidate inline ownership. Existing physical `locals`/`print` behavior remains intact after returning to the physical context.
- Pointer dereference, pointer-member traversal, aggregate traversal, arbitrary expression evaluation, and broader optimized-location coverage remain unavailable while an inline context is selected. These are not implied by scalar materialization and remain separately evidence-gated.
- Test-first compiler evidence at exact head `335c7b6183507fb935b85ab29f84d0ab75fd613e` built successfully in both permanent lanes and then failed because inline selection still reused physical value-materialization semantics. After the ownership path was implemented and the exact shadow-value contract tightened, final implementation head `ffdf6c58888c9847c6cd3e777fcf79cc498b7cca` passed Configure, Build, and Test in both GCC and Clang-large workflow run #1213.

Phase 32 is intentionally sealed here. More `DW_OP_regN` variants, additional scalar widths, pointer conveniences, aggregate traversal, or speculative optimizer-location forms are not reasons to keep farming this phase. A new form requires a genuine compiler artifact and must preserve physical machine-state ownership.

## Phase 33 promotion — bounded caller-frame inline scalar materialization

The next higher-value ownership gap is explicit in the implementation: selected-inline scalar materialization currently rejects every physical inspection frame whose index is not zero. The debugger can already reconstruct immutable historical physical caller frames, and inline source identity can be discovered on a selected physical frame, but Phase 32 deliberately does not claim that a scalar can be materialized from a caller frame's reconstructed machine state.

The first coherent Phase 33 slice must:

1. Start from a genuine GCC and Clang optimized core in which an active inline chain exists in a historical physical caller frame (`frame > 0`) and at least one selected inline local retains a concrete compiler-produced scalar location. Do not manufacture a caller-only inline location to satisfy the test.
2. Keep the physical caller frame as the sole machine-state owner. Inline selection may choose source/DIE scope, but register values, stack/CFA state, memory, and lookup PC must come from the already reconstructed immutable physical caller frame; do not synthesize an inline unwind frame.
3. Accept only a location/storage form that the existing snapshot/caller-frame machinery can prove from that reconstructed frame. If GCC or Clang does not retain a stable supported scalar location, stop and use the artifact as architecture evidence rather than adding guessed register values or widening the expression evaluator speculatively.
4. Preserve the Phase 32 selected-inline name rules: exact inline-DIE ownership, abstract-origin names, active lexical descendants, deepest shadowing, bounded ranges, and fail-closed equal-depth ambiguity. A same-named physical or sibling binding must not satisfy the request.
5. Prove the real workflow `mdbg-core frame <n> -> inline -> inline <index> -> print <name>` returns the exact compiler-owned scalar while the same physical caller frame still supports its ordinary physical locals/value inspection after `inline physical`.
6. Preserve frame/thread invalidation of inline selection and retain all existing module/source ownership from Phases 30 and 31.
7. Keep pointer dereference, aggregate traversal, general DWARF expression evaluation, and unrelated caller-frame register recovery out of the first slice unless the same genuine artifact demonstrates that one of them is strictly required to materialize the selected scalar.

This promotion extends optimized post-mortem value access across reconstructed physical history without weakening the central invariant: inlining creates source/DIE contexts layered on real machine frames, never additional machine frames of its own.
