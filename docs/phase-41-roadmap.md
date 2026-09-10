# Phase 41 Roadmap — compiler-proven bounded enum identity

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 41 moves selected-inline post-mortem value inspection from physical layout alone to compiler-described symbolic type identity. The implementation remains read-only and snapshot-owned: inline selection continues to choose lexical/source context only, while bytes, registers, module ownership, frame/thread identity, and provenance remain anchored in the selected immutable physical core frame.

## Completed acceptance

- Permanent GCC and Clang optimized-core evidence retains a genuine selected-inline `caller_mode` value through the already-supported caller-frame `DW_OP_fbreg` location. No synthetic DIE, hand-authored location expression, or source-level value injection is used.
- The independent DWARF oracle proves `caller_mode` resolves to one `DW_TAG_enumeration_type` with four-byte unsigned representation and direct compiler-emitted enumerators `CallerInlineIdle = 3`, `CallerInlineReady = 7`, and `CallerInlineBusy = 42` before product assertions run.
- The two compiler lanes exercise different genuine representation spellings without widening the model speculatively: GCC describes the enum with direct integer `DW_AT_encoding`, while Clang describes the compatible four-byte unsigned representation through its referenced base type.
- The typed value model preserves `LocalValueKind::Enumeration` plus bounded `LocalEnumType` metadata instead of silently reclassifying the value as an anonymous integer. The enum table is finite, directly owned by the compiler enum DIE, and bounded to at most 64 direct entries.
- Selected-inline type recovery accepts only the compiler-proven enum shape through existing typedef/const wrappers, requires a non-empty type name, one-to-eight-byte integral storage, representation agreement, supported constant-value forms, unique enumerator names, and a non-empty bounded direct enumerator table. Unsupported or contradictory type descriptions remain fail-closed.
- Caller-frame materialization reuses the existing immutable snapshot-memory path. The enum raw value therefore preserves the same `SnapshotCoreMemory` or exact owner-matched `SnapshotRuntimeArtifact` provenance as other selected-inline caller values; no synthetic inline machine state is introduced.
- Exact symbolic lookup is conservative: one matching raw value renders its enumerator, an unknown raw value remains numeric, and duplicate raw-value aliases are treated as ambiguous rather than choosing an arbitrary name.
- `CoreInspectionSession` exposes the same typed value and real `mdbg-core` renders `CallerInlineMode::CallerInlineBusy (0x2a)` together with `[4-byte enum unsigned]`; the numeric representation remains visible and authoritative.
- Selected-inline lexical ownership, abstract-origin handling, frame/thread/module invalidation, immutable provenance, and existing integer/pointer/structure/union/array/bit-field behavior remain covered by the permanent evidence and normal CI suites.
- The pre-seal cleaned candidate `e70e46bec0692e6001af8ca8389c09be9b81542b` passed both GCC and Clang lanes of the dedicated `core-inline-evidence` gate, including compiler oracle, core build, session assertions, and real CLI rendering; the normal GCC and Clang-large Configure/Build/Test matrix also passed.
- Construction-only patch workflows used to recover the staged implementation are absent from the candidate tree before formal review.

Phase 41 is intentionally sealed here. Additional enumerator counts, alternate positive values, another integer width, signedness variants, `enum class` spelling, scoped-name reconstruction, flags decomposition, enum arithmetic, mutation, or hand-picked compiler switches are not reasons to extend this phase without a new architectural requirement and independent compiler evidence.

## Phase 42 promotion — compiler-proven typed aggregate composition

The next higher-value gap is type-identity composition across an existing aggregate boundary. A direct selected-inline enum can now retain its compiler identity, but aggregate member descriptors still carry only scalar kind/width/sign plus pointer or bit-slice metadata; they cannot carry an enum's bounded name/value identity through member selection. Treating an enum-valued member as a plain integer would recreate the information-loss problem one layer deeper.

The first coherent Phase 42 slice must:

1. Start from genuine permanent-lane GCC and Clang optimized-core artifacts in which an active selected inline context retains one bounded aggregate containing an enum-valued direct member through compiler-produced locations already supported by the immutable snapshot model. If both compilers do not retain a stable member/type shape, stop and record the evidence instead of manufacturing one.
2. Independently prove the aggregate DIE, member ownership/offset, member enum type, exact byte width/signedness representation, and direct enumerator table before product assertions run.
3. Extend the typed member descriptor only enough to preserve one compiler-proven enum leaf across aggregate materialization and member selection. Prefer a reusable bounded scalar-type composition boundary over a second parallel enum subsystem.
4. Materialize the aggregate from immutable core/artifact bytes, select the enum member through the existing `CoreInspectionSession` aggregate-member path, and preserve `LocalValueKind::Enumeration`, enum metadata, normalized raw value, exact symbolic lookup, and original snapshot provenance at the selected leaf.
5. Render the selected member through real `mdbg-core` with both symbolic and numeric value. Unknown or duplicate raw mappings must remain conservative exactly as in Phase 41.
6. Preserve existing aggregate bounds, bit-field semantics, pointer/member behavior, lexical/inline ownership, frame/thread/module invalidation, and artifact ownership; no recursive general type graph, expression evaluator, mutation, variant/discriminant inference, or ABI packing guess is a prerequisite.
7. If the genuine artifact requires nested recursive aggregates, unsupported dynamic member locations, type units/templates, discriminated variants, or compiler-specific assumptions not independently represented in DWARF, stop and promote that blocker rather than broadening the slice speculatively.

This promotion tests whether symbolic type identity composes through an already-supported value boundary instead of farming more top-level enum variants.