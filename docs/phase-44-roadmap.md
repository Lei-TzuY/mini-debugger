# Phase 44 Roadmap — physical historical stack aggregate ownership

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 44 removes a physical/selected-inline ownership asymmetry in immutable post-mortem value inspection. A recovered historical physical frame can now own and materialize one bounded stack-resident by-value structure through genuine compiler-produced `DW_OP_fbreg` evidence, without borrowing crash-frame state or introducing a second location evaluator.

## Completed acceptance

- The existing permanent `xmm_core_value_fixture` now retains a historical caller-owned `caller_stack_aggregate` with two unsigned 64-bit direct members. The same fixture still exercises caller scalar ownership, crash-frame values, threads, pointers, typed objects, and real core generation.
- An independent readelf-based oracle proves the exact compiler-produced historical location and layout before the product assertion runs. GCC uses the existing `DW_OP_fbreg + DW_OP_call_frame_cfa` ownership family; Clang uses the existing `DW_OP_fbreg + DW_OP_reg7 (rsp)` family. Both compiler lanes prove the 16-byte structure plus direct `first@0` and `second@8` members.
- Test-first evidence failed in both permanent compiler lanes at the deliberate production boundary: the physical snapshot `DW_OP_fbreg` evaluator accepted only bounded integer locals even though the same immutable frame-base and snapshot-memory machinery was already sufficient to own the compiler-proven structure.
- The physical `DW_OP_fbreg` path now accepts either an already-bounded integer scalar or an already-bounded `LocalValueKind::Structure`. It still reuses `snapshot_frame_base()`, checked signed address arithmetic, `read_snapshot_memory()`, and the existing structure decoder; no parallel DWARF expression evaluator was added.
- Physical structure materialization now preserves compiler-owned direct-member byte offsets in the ordinary `LocalStructMember` representation, so artifact-backed member provenance can compose from the containing value instead of collapsing every member to offset zero.
- `CoreInspectionSession::inspect_aggregate_member()` is now context-neutral for the proven direct by-value structure operation: selected-inline contexts still use selected-inline value lookup, while a physical context uses ordinary snapshot value lookup. Both feed the same bounded materialized-value member selector.
- Real API evidence proves historical `frame 1 -> inline physical -> print caller_stack_aggregate -> aggregate-member caller_stack_aggregate second` behavior, immutable core provenance, exact direct-member values, and invalidation when thread/frame ownership changes.
- The real `mdbg-core` subprocess proves the same physical aggregate and direct-member selection. Existing selected-inline aggregate, nested aggregate, enum, bit-field, union, array, pointer, signal-restored, local-discovery, and frame/thread regressions remain covered.
- The exact production head passes the full permanent GCC and Clang-large `ci` matrix plus the dedicated GCC/Clang `core-inline-evidence` workflow.

Phase 44 is sealed here. Additional scalar widths, member counts, offsets, or another stack structure with the same ownership shape are not new milestones. Nested physical aggregates, arbitrary member paths, inheritance, dynamic member locations, and a general C/C++ expression parser remain outside this phase.

## Phase 45 promotion — context-neutral bounded typed aggregate traversal

The next architectural gap is no longer whether a physical historical frame can own one structure. The project now has several typed aggregate operations whose lookup dispatch differs by source context even though traversal acts on the same materialized bounded value model. Phase 45 should remove that architectural duplication rather than add another isolated type variant.

The first coherent Phase 45 slice must:

1. Define one context-neutral bounded aggregate traversal contract over an already-materialized `LocalScalarValue`, with provenance composition, offset checks, member-kind validation, and error semantics independent of whether the root came from a physical or selected-inline lookup.
2. Reuse that contract from both physical and selected-inline session paths. Inline selection remains lexical/DIE ownership only; physical/inline lookup still resolves source ownership separately before traversal begins.
3. Prove one cross-context typed operation that currently exposes the split in a meaningful way, preferably a direct pointer-valued member on a by-value structure followed by exactly one bounded integer dereference. Start from genuine GCC/Clang core evidence; do not manufacture a member kind merely to exercise an API.
4. Preserve exact immutable provenance across the containing aggregate, selected member, and optional one-hop pointee read. Runtime-artifact file offsets must advance by compiler-owned member offsets; the pointee read must obtain its own snapshot-memory provenance.
5. Keep traversal depth explicit and finite. No recursive generic object graph, arbitrary dotted-path parser, pointer arithmetic, array-expression syntax, mutation, or implicit dereference is introduced.
6. Expose the same behavior through `CoreInspectionSession` and real `mdbg-core`, with physical and selected-inline coverage sharing the same traversal primitive rather than duplicating implementation.
7. Retain deterministic invalidation across thread/frame/inline-context changes and all existing structure/member count, byte-width, null-pointer, host-address-width, unreadable-memory, ambiguity, and module-ownership bounds.
8. Require permanent GCC and Clang-large integration plus the relevant dedicated compiler-evidence lanes on the exact candidate head before integration.

This promotion turns Phase 44's new physical capability into a cleaner typed-value architecture instead of continuing to grow separate physical and selected-inline feature ladders.
