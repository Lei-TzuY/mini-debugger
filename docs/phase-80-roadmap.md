# Phase 80 Roadmap — current-frame live pointer-valued aggregate-member dereference

Status: complete for the current Linux x86-64 genuine-live compiler-evidence milestone.

Phase 80 closes the product-surface gap between an already-materialized current-frame live structure, an already-canonical pointer-valued direct member, and the existing one-hop stopped-tracee pointer dereference primitive.

## Completed acceptance

- The permanent optimized live fixture now retains a genuine sixteen-byte `LivePointerAggregate` local in `inspect_live_pointer_aggregate` with:
  - signed `int32_t direct = 0x11223344` at offset 0;
  - `int32_t *linked` at offset 8;
  - a real runtime target symbol `live_pointer_target = 0x02468ace`.
- An independent readelf oracle proves under GCC and Clang, PIE and non-PIE, before product assertions:
  - compiler-produced `DW_OP_fbreg` ownership at `live_pointer_aggregate_probe`;
  - exact sixteen-byte outer structure extent;
  - exact direct-member offsets 0 and 8;
  - signed-int32 scalar identity for `direct`;
  - x86-64 pointer identity for `linked`;
  - signed-int32 pointee metadata for `linked`.
- Test-first exact head `2b88026c20c058879aeb1f950f6789e4cbe3f3a4` leaves production unchanged. Both new compiler oracles pass in both normal lanes, and the direct API composes existing primitives all the way through a real stopped-tracee pointee read.
- The API test independently resolves the ELF symbol `live_pointer_target`, computes its runtime address, and reads `0x02468ace` directly from the stopped tracee before invoking source-value inspection. Product output therefore does not serve as its own correctness oracle.
- On that red head, 84/86 normal tests remain green; only `dwarf_line_integration_{pie,nopie}` fail. The CLI already renders the exact aggregate and pointer member but treats `deref-aggregate-member` as an unknown command. This proves the missing boundary is presentation/orchestration rather than DWARF typing, live byte ownership, member selection, or pointer dereference.
- Production head `6e72c788a2b12b8cac39a84854197178328d1cdd` adds only the live `deref-aggregate-member <name> <member>` command surface:
  - `inspect_local_value(debugger, elf, name)` resolves the current-frame root;
  - `inspect_local_aggregate_member()` selects the canonical pointer member;
  - `current_inspection_frame(debugger, elf)` supplies current stopped-frame freshness/ownership;
  - `dereference_local_pointer(debugger, frame, member)` performs exactly one existing stopped-tracee read.
- No new parser, DWARF location evaluator, pointer/member descriptor, aggregate materializer, memory reader, or dereference engine is introduced.
- The Phase-80 command intentionally accepts only the compiler-proven bounded integer terminal result. Pointer chains, structure pointees through this command, implicit dereference, casts, pointer arithmetic, recursive paths, mutation, and general expressions remain outside scope.
- API evidence proves exact root structure identity, runtime pointer address, signed-int32 pointee metadata, and one-hop dereference to `0x02468ace`.
- Real `mdbg` proves:
  `break live_pointer_aggregate_probe -> continue -> print live_pointer_aggregate -> aggregate-member live_pointer_aggregate linked -> deref-aggregate-member live_pointer_aggregate linked -> continue`.
- Exact production head `6e72c788a2b12b8cac39a84854197178328d1cdd` passes full GCC / Clang-large normal CI plus both dedicated GCC / Clang compiler/core/session/CLI evidence lanes, based directly on main `1cba871b2669b8bbdeb61a01a30c446f65cf71da`.

Phase 80 is sealed here. Additional scalar widths, another pointer member, alternate member offsets, or merely allowing another already-supported pointee kind are not new milestones.

## Phase 81 promotion — live CLI historical-frame source inspection

The next architectural gap is ownership selection, not another type shape. Phases 71–75 already prove historical live-frame enum/pointer/structure-pointer/pointer-member behavior through `InspectionFrameContext` recovered from CFI. The live `mdbg` CLI, however, still resolves `print` and aggregate commands only against the current stopped frame; unlike `mdbg-core`, it has no explicit source-inspection `frame <index>` selection.

The first coherent Phase 81 slice must:

1. Start from an existing permanent historical-live fixture and exact stopped breakpoint where `build_inspection_frames()` already recovers at least one compiler-proven caller frame. Prefer a Phase-72/74 pointer fixture so selection can prove both scalar/pointer identity and one one-hop dereference without inventing a new typed-value surface.
2. Add an explicit live inspection-frame selection state to `mdbg`, distinct from execution control. `frame <index>` must select only from a freshly rebuilt CFI inspection-frame snapshot owned by the current stop sequence.
3. Route source-value commands through the selected `InspectionFrameContext` when one is selected, while current-frame behavior remains the default. Do not copy caller registers into the live top frame or mutate debugger machine state.
4. At minimum prove `bt -> frame 1 -> print <historical-pointer> -> deref <historical-pointer>` or the equivalent existing historical fixture path through real `mdbg`.
5. Selection must be invalidated whenever execution advances, process/thread ownership changes, or a new stop sequence is observed. Reusing a stale historical frame must remain an explicit failure rather than silently falling back to current registers.
6. Keep execution commands (`step`, `next`, `finish`, `continue`) operating on the real current machine frame. Historical source inspection must never imply historical execution control.
7. Reuse `build_inspection_frames()`, existing CFI register ownership, existing `inspect_local_value(..., InspectionFrameContext, ...)`, and existing bounded live dereference primitives. No second unwind engine, frame model, register cache, or source evaluator is allowed.
8. Keep arbitrary frame mutation, write operations against historical state, synthetic caller registers, recursive object graphs, and expression-language expansion out of scope.
9. Require full GCC / Clang-large CI plus independent CFI/compiler evidence and real live CLI regression on the exact candidate before integration.

This promotion moves the debugger from current-frame-only source inspection to an explicit multi-frame live inspection model while preserving strict stop-generation ownership.
