# Phase 34 Roadmap — bounded selected-inline pointer traversal

Status: complete for the current Linux x86-64 genuine-core compiler-evidence milestone.

Phase 34 extends selected inline post-mortem inspection from bounded scalar materialization to one typed pointer reachability step without turning an inline source context into a fictional machine frame.

## Completed acceptance

- A genuine `-O2 -g -gdwarf-4` cross-file caller-inline fixture is compiled independently in the permanent GCC and Clang lanes, as PIE and non-PIE, and produces a real kernel core with `caller_inline_outer -> caller_inline_inner` active inside historical physical frame 1.
- Compiler evidence precedes product behavior. GCC retains `caller_pointer` as direct `DW_OP_fbreg -32`; Clang retains it as direct `DW_OP_fbreg +16`. Both bind the selected-inline local to `DW_TAG_pointer_type`. No hand-authored location expression or new expression evaluator opcode is used.
- The selected inline DIE and its active lexical descendants remain the only source-name owners. Existing abstract-origin, shadowing, equal-depth ambiguity, module, physical-frame, and selected-thread validation stay in force.
- Historical pointer materialization reuses the physical frame's compiler-produced frame base and the existing bounded `DW_OP_fbreg` address path. The pointer object bytes are read through immutable snapshot memory and retain core-memory provenance.
- The selected-inline value model now preserves x86-64 pointer width and bounded pointee metadata. Frame-zero inline pointer materialization remains explicitly outside this phase's compiler evidence; the earlier frame-zero integer-register contract is unchanged.
- The first traversal is deliberately limited to one bounded integer pointee. Null pointers, host-width overflow, missing/unsupported pointee metadata, non-pointer names, and unsupported pointee kinds fail explicitly. Aggregate/member traversal and pointer chains remain outside Phase 34.
- `CoreInspectionSession` can select physical frame 1 and inner inline context 1, materialize `caller_pointer`, then dereference it to the immutable `caller_inline_pointee == 0x02468ace` value while preserving snapshot-memory provenance.
- The real `mdbg-core` workflow exercises `frame 1 -> inline 1 -> locals -> print caller_pointer -> deref caller_pointer`, proving the same selected-inline ownership and one-step pointee traversal through the product surface.
- `inline physical`, frame selection, and thread selection continue to invalidate selected inline ownership and restore ordinary physical pointer behavior. Pointer-member commands remain physical-only.
- The test-first exact tree represented by normal CI #1244 built successfully in both permanent compiler lanes and failed only in the new caller-inline product gate while the existing 54-test CTest suite remained green. Exact production tree `6a4afafac0826f9fa00c1891255529c886d83515` then passed Configure, Build, Test, and the permanent compiler/core pipeline in both lanes; the dedicated compiler-evidence run also passed GCC and Clang.

Phase 34 is intentionally sealed here. More fbreg offsets, integer pointee widths, another pointer spelling, extra caller depths, or pointer-chain variants are not reasons to continue this phase. A new form must start from a concrete compiler-produced artifact and preserve immutable physical-frame machine-state ownership.

## Phase 35 promotion — selected-inline bounded aggregate/member traversal

The next higher-value typed-object gap is explicit in the current product contract. Physical core inspection already has bounded pointer-to-structure materialization plus direct pointer-member inspection/dereference, but `CoreInspectionSession::inspect_pointer_member()` and `dereference_pointer_member()` still require physical value context and reject active inline selection.

The first coherent Phase 35 slice must:

1. Start from a genuine GCC and Clang optimized core in which the selected inline DIE owns a pointer-to-structure local with a compiler-produced location already supported by the immutable selected physical frame. Do not fabricate a convenient structure layout or location expression.
2. Reuse the existing bounded physical `LocalPointeeType` structure/member model and snapshot-memory reader. Inline selection remains source/DIE scope only; no inline register file, CFA, stack, memory image, or synthetic frame may be invented.
3. Preserve selected-inline lexical ownership, abstract-origin resolution, shadowing, ambiguity rejection, module/frame/thread invalidation, and exact member-name ownership before reading any bytes.
4. Prove one bounded direct member read and, only when the genuine compiler artifact supplies it, one existing supported pointer-valued direct member dereference. Do not generalize into recursive object graphs, arrays, arbitrary pointer arithmetic, inheritance, unions, or a DWARF expression VM.
5. Preserve null/unreadable/unsupported/optimized-out failures and immutable core/runtime-artifact provenance exactly as the physical snapshot machinery does.
6. Prove the behavior through both `CoreInspectionSession` and real `mdbg-core`, while `inline physical`, frame selection, and thread selection continue to invalidate inline object ownership.

If the permanent compiler lanes do not retain a stable pointer-to-structure binding within current bounded storage semantics, that is architecture evidence and the phase must stop rather than manufacture a value or weaken the gate.
