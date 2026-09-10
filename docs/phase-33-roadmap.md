# Phase 33 Roadmap — bounded caller-frame inline scalar materialization

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 33 closes the machine-state ownership gap left by Phase 32. A selected inline context may now materialize one compiler-proven bounded integer scalar from an immutable historical physical caller frame without inventing an inline register file, stack pointer, CFA, or unwind frame.

## Completed acceptance

- A genuine `-O2 -g -gdwarf-4` optimized cross-file fixture is compiled independently in the permanent GCC and Clang-large lanes. It produces a real kernel core with an active `caller_inline_outer -> caller_inline_inner` source chain inside historical physical frame 1.
- Compiler evidence is required before product assertions. GCC emits `caller_shadow` as a direct `DW_OP_fbreg -28` binding and Clang emits a direct `DW_OP_fbreg +4` binding for the selected `caller_inline_inner` DIE. No hand-authored DWARF location expression is used.
- The fixture makes the selected value deterministic: `argc == 1` yields physical seed `21`, `caller_inline_outer` derives `64`, and `caller_inline_inner` derives `caller_shadow == 107 (0x6b)` before the noinline crash leaf observes its address and the genuine core is produced.
- Inline selection remains source/DIE scope only. The selected physical caller frame remains the sole owner of reconstructed registers, stack pointer, frame pointer, lookup PC, module identity, CFA, and memory provenance.
- Caller-frame selected-inline materialization reuses the existing physical snapshot `DW_OP_fbreg` machinery: the owning physical subprogram's compiler-produced `DW_AT_frame_base` is resolved from immutable caller state, the signed fbreg offset is decoded through the existing bounded SLEB path, and the value bytes are read through `read_snapshot_memory()`.
- Phase 32 frame-zero behavior remains bounded to the compiler-proven one-operation register forms already accepted there. Phase 33 does not widen frame-zero register coverage or general DWARF expression evaluation.
- Historical selected-inline materialization is deliberately bounded to direct `DW_OP_fbreg` integer scalars of at most 8 bytes. Caller-frame register locations, location-piece composition, pointers, aggregates, arbitrary expressions, and speculative optimizer forms remain unsupported unless a later genuine compiler artifact requires them.
- The real `mdbg-core` workflow selects `frame 1`, lists the compiler-proven inline chain, selects the inner inline context, retains `caller_shadow` in its scoped local catalogue, and materializes `caller_shadow = 0x6b [4-byte signed]` from the caller frame's immutable stack state.
- `inline physical` returns to ordinary physical caller ownership and preserves the existing physical locals. Frame and thread selection continue to invalidate stale inline selection.
- The previous exact evidence head `8e224984497df406ef693598bb3596168a7385ed` failed the permanent GCC and Clang-large lanes at the explicit frame-zero selected-inline materialization guard. Exact implementation head `fa4b005cde0125ceb2a288bccd61e20f50a5a8dc` then passed Configure, Build, the 54-test CTest suite, and the compiler-evidence/core pipeline in both lanes.

Phase 33 is intentionally sealed here. More fbreg offsets, integer widths, extra caller depths, or compiler-location variants are not reasons to farm this phase. New scalar forms require a concrete compiler-produced failure and must preserve physical-frame machine-state ownership.

## Phase 34 promotion — bounded selected-inline pointer value traversal

The next higher-value ownership gap is explicit in the product contract. Selected inline contexts can now materialize bounded integer scalars from frame zero and from a compiler-proven historical caller stack slot, but pointer traversal remains categorically physical-only: `CoreInspectionSession` rejects dereference and member traversal whenever an inline context is active, and the selected-inline value materializer does not yet carry pointer/pointee metadata.

The first coherent Phase 34 slice must:

1. Start from a genuine GCC and Clang optimized core in which an active selected inline context retains a pointer-valued local with a compiler-produced location already provable from the selected immutable physical frame. Do not manufacture a convenient pointer location or widen the evaluator before the artifact exists.
2. Keep machine-state ownership physical. Inline selection chooses only source/DIE scope; register values, caller reconstruction, CFA, stack addresses, pointee bytes, module ownership, and provenance must continue to come from the selected physical snapshot frame and existing snapshot memory reader.
3. Resolve the pointer name only inside the selected inline DIE and its active lexical descendants, preserving Phase 32/33 abstract-origin, shadowing, ambiguity, module, frame, and thread invalidation rules. A physical or sibling pointer with the same spelling may not satisfy an inline request.
4. Materialize only a bounded x86-64 pointer value whose compiler location and pointee type are already supported by existing physical snapshot machinery. Null, unreadable, unsupported, optimized-out, or ambiguous locations must fail explicitly.
5. Prove one bounded pointee traversal through both `CoreInspectionSession` and the real `mdbg-core` selected-inline workflow. The first slice should prefer one integer pointee; aggregate/member traversal stays out unless the same genuine compiler artifact strictly requires it.
6. `inline physical`, frame selection, and thread selection must restore ordinary physical pointer semantics and invalidate stale inline ownership.
7. Do not use Phase 34 as a vehicle for general DWARF expression evaluation, synthetic inline frames, arbitrary pointer arithmetic, unrelated caller-register recovery, or speculative optimizer-location coverage.
8. If either permanent compiler lane does not retain a stable pointer binding within existing bounded storage semantics, stop and treat that artifact as architecture evidence rather than fabricating a value or weakening the gate.

This promotion advances optimized post-mortem inspection from bounded inline scalar access to typed object reachability while preserving the invariant that inline contexts never become fictional machine frames.
