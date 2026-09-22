# Phase 43 Roadmap — compiler-proven bounded nested aggregate composition

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 43 extends selected-inline post-mortem typed-value inspection across exactly one additional by-value structure boundary. The selected inline context remains lexical/source ownership only; bytes, frame/thread identity, module ownership, and provenance remain anchored in the selected immutable physical snapshot frame.

## Completed acceptance

- Permanent GCC and Clang optimized-core evidence retains a genuine selected-inline `caller_nested_aggregate` in both PIE and non-PIE artifacts through the already-supported caller-frame `DW_OP_fbreg` ownership path.
- An independent DWARF oracle proves an eight-byte `CallerInlineNestedOuter` with `prefix@0` and a four-byte structure-valued `inner@4`, plus `CallerInlineNestedInner::terminal@0` as a signed 32-bit scalar. No synthetic DIE graph, hand-authored location expression, or product-side value injection is used.
- The existing bounded member descriptor carries exactly one structure-valued direct member with a finite direct terminal-member set. It does not become a recursive generic object graph.
- Nested materialization reads the outer aggregate once through immutable snapshot memory, decodes the inner structure strictly from the byte range owned by the outer aggregate, and retains compiler-owned relative member offsets.
- `SnapshotRuntimeArtifact` provenance composes through both member hops: selecting `outer.inner` advances the artifact file offset by the compiler-owned outer-member offset, and selecting `outer.inner.terminal` advances by the terminal's relative offset without performing an unrelated memory search.
- `CoreInspectionSession::inspect_nested_aggregate_member()` and real `mdbg-core nested-aggregate-member` expose exactly `outer -> inner -> terminal`. Unknown members, non-structure intermediate members, and a deeper structure terminal are rejected deterministically.
- Existing integer, pointer, enum, bit-field, array, union, lexical/abstract-origin, frame/thread/module invalidation, and immutable-provenance behavior remains covered by the permanent suites.
- Exact candidate evidence requires the normal GCC / Clang-large Configure-Build-Test matrix and the dedicated GCC / Clang `core-inline-evidence` compiler/oracle/core/session/CLI gate to pass on the same head before integration.

Phase 43 is intentionally sealed here. More inner member counts, alternate offsets, additional integer widths, enum/bit-field leaves, or another nesting depth are not reasons to extend this phase. Arbitrary recursive paths, inheritance, dynamic member locations, flexible arrays, discriminated variants, cycles, and a general C/C++ expression evaluator remain outside this milestone.

## Phase 44 promotion — physical historical stack aggregate ownership

The next higher-value gap is no longer structural depth. It is an ownership asymmetry between physical and selected-inline snapshot frames: selected-inline historical `DW_OP_fbreg` values can now own bounded by-value structures, while the physical-frame snapshot `DW_OP_fbreg` path still explicitly accepts only bounded integer locals.

The first coherent Phase 44 slice must:

1. Start from genuine permanent-lane GCC and Clang core artifacts where a selected historical physical frame, with no inline context selected, owns one stack-resident by-value structure through a compiler-produced `DW_OP_fbreg` location. If both compilers do not retain a stable artifact, stop and record that evidence instead of manufacturing one.
2. Independently prove the physical subprogram, exact structure byte size, direct member names/types/constant offsets, selected frame lookup PC, and frame-base form before product assertions run.
3. Extend the existing physical snapshot `DW_OP_fbreg` path only enough to admit the already-bounded `LocalValueKind::Structure` model. Reuse `snapshot_frame_base()`, checked signed address arithmetic, `read_snapshot_memory()`, and existing structure materialization; do not add a parallel location evaluator.
4. Preserve physical selected-thread/frame ownership and immutable core/runtime-artifact provenance. No crash-frame register may substitute for a historical caller register or CFA.
5. Make bounded by-value aggregate member selection context-neutral for the proven physical value, reusing the existing member metadata and offset/provenance rules rather than duplicating the selected-inline traversal engine.
6. Prove the behavior through `CoreInspectionSession` and real `mdbg-core` with a workflow such as `frame <historical> -> inline physical -> print <aggregate> -> aggregate-member <aggregate> <member>`, including deterministic invalidation after frame/thread changes.
7. Keep the first slice bounded to one direct aggregate and direct terminal members. Nested aggregates, recursive paths, arbitrary DWARF expressions, mutation, and new ABI assumptions require separate evidence and are not implied by physical-frame parity.

This promotion moves the project from an inline-specialized typed-object capability toward one coherent immutable physical-frame ownership model rather than farming more terminal type variants.
