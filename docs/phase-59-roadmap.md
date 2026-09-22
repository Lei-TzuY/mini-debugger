# Phase 59 Roadmap — compiler-proven frame-zero selected-inline bit-field structures

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 59 composes canonical bit-field semantics with genuine frame-zero selected-inline ownership. The original hypothesis expected register/register-piece ownership, but both permanent compiler lanes instead retained the active four-byte bit-field structure on the physical crash stack through one exact compiler-produced `DW_OP_fbreg` location. The implementation follows that evidence rather than forcing a register representation.

## Completed acceptance

- A dedicated optimized fixture retains one active four-byte `inline_bit_fields` structure in the selected frame-zero inline scope at the exact crash probe.
- An independent DWARF oracle proves the inline lexical chain, exact four-byte structure extent, signed five-bit `signed_bits`, unsigned six-bit `unsigned_bits`, exact compiler bit-location spelling, and the crash-PC-active location before product assertions run.
- Clang evidence is `DW_OP_fbreg: -4` with `DW_AT_data_bit_offset = 0 / 5`.
- GCC evidence is `DW_OP_fbreg: -28` with legacy `DW_AT_bit_offset = 27 / 21` plus `DW_AT_data_member_location = 0`; both spellings resolve to aggregate-relative little-endian bit positions 0 and 5.
- Initial test-first head `6869dac1180d3f5d5bd1e16ddf771abd4836834b` proved the genuine compiler stack-owned shape and failed before product support. Follow-up head `6e4265c1e03e35ec6d22851e5365cb3d678d5def` tightened direct frame-zero `DW_OP_fbreg` acceptance and remained red until the product path was composed.
- Production head `cffa458247e53e9c36a2fffee236e0b316130f62` adds only a bounded frame-zero bit-field-structure gate to the existing selected-inline `DW_OP_fbreg` path. It does not add a new bit parser, decoder, frame base, register file, or memory reader.
- Type recovery reuses canonical `resolve_bounded_bit_field_member_type()` / `LocalBitSlice` metadata. Immutable bytes come from the physical frame-zero snapshot frame base plus existing snapshot memory. Materialization reuses the generic immutable structure path and shared bounded bit-field decoder.
- API evidence recovers exact signed `-7` normalization and unsigned `41`, preserves immutable snapshot-memory provenance, supports direct aggregate-member selection, and invalidates the selected-inline value after returning to the physical context.
- Real `mdbg-core` exercises the same frame-zero selected-inline print/member workflow for PIE and non-PIE artifacts.
- The fixture/oracle/integration is permanently wired into normal GCC / Clang-large CI and the dedicated compiler-evidence workflow.
- Exact production head `cffa458247e53e9c36a2fffee236e0b316130f62` passes full GCC / Clang-large CI plus both dedicated GCC / Clang evidence lanes and is based directly on main `34a3fe1f80dff8b45163b32f1c6c130bba90c06e`, ahead three / behind zero.

Phase 59 is sealed here. More bit widths, packing examples, fbreg offsets, endian variants, register-forcing fixture tricks, writes, or arbitrary bit slicing are not new milestones.

## Phase 60 promotion — compiler-proven frame-zero selected-inline nested aggregate composition

The next meaningful executable composition gap is a one-hop nested by-value structure at frame zero. Canonical nested-structure type recovery and immutable one-hop materialization already exist from Phase 48, and frame-zero selected-inline ownership now has proven register-piece and stack-backed aggregate paths. Those capabilities have not yet been composed for a nested aggregate.

The first coherent Phase 60 slice must:

1. Start from a genuine optimized GCC and Clang frame-zero inline artifact containing one bounded outer structure with one direct scalar member and one by-value inner structure containing one terminal integer member.
2. Independently prove lexical ownership, exact outer/inner byte extents and offsets, terminal scalar type, crash-PC-active compiler location, and physical register/stack ownership before product assertions run.
3. Follow the compiler-produced ownership form actually emitted by both permanent lanes. Accept only evidence-proven RDX/RCX register pieces or exact bounded `DW_OP_fbreg`; do not force a representation merely to reuse an existing evaluator.
4. Reuse canonical `resolve_bounded_nested_structure_member_type()`, the existing one-hop nested metadata, and shared immutable nested byte materialization. Do not create a frame-zero nested parser or recursive object graph.
5. Preserve exact `SnapshotCoreRegister` or snapshot-memory/runtime-artifact provenance according to the proven physical owner, and keep nested terminal traversal context-neutral.
6. Prove real API and `mdbg-core` behavior for `inline <active> -> print <outer> -> nested-aggregate-member <outer> <inner> <terminal>`, including exact values and physical/inline invalidation.
7. Keep deeper nesting, arrays/unions inside the nested member, recursive graphs, mutation, arbitrary expressions, and unsupported ownership forms out of scope.
8. Require full GCC / Clang-large CI plus permanent GCC / Clang compiler-oracle/core/session/CLI evidence on the exact candidate before integration.

This promotion composes the already-proven nested-value model with frame-zero selected-inline machine ownership rather than farming another bit-field layout.
