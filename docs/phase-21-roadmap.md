# Phase 21 Roadmap — historical caller-frame stack ownership

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 21 extends the immutable post-mortem source-value model from Phase 20 frame-zero stack ownership to one CFI-recovered historical caller frame. The completed slice is compiler-produced, core-backed, and deliberately reuses the existing bounded `DW_OP_fbreg` evaluator instead of adding a generic DWARF expression engine.

## Completed executable slice

- the permanent genuine-core fixture now contains a noinline caller with deterministic `uint64_t caller_stack_local = 0xcafebabedeadbeef`, and the crash occurs in its callee so frame #1 is recovered from the real kernel `ET_CORE` artifact rather than synthesized test state;
- a readelf oracle runs before production inspection and proves the caller local is genuinely compiler-owned at the historical return PC: the GCC lane emits `DW_OP_fbreg -32` with `DW_OP_call_frame_cfa`, while the Clang-large lane emits `DW_OP_fbreg 0` with `DW_OP_reg7 (rsp)`; PIE and non-PIE artifacts are produced in both permanent lanes;
- test-first evidence reached the genuine caller frame and failed only at the previous snapshot evaluator boundary that accepted historical `DW_OP_breg3`/`DW_OP_addr` ownership but rejected caller-frame `DW_OP_fbreg`; all normal 54 CTest cases remained green;
- `snapshot_frame_base()` no longer treats frame index zero as the ownership criterion. `DW_OP_reg7 (rsp)` uses only the selected immutable `SnapshotInspectionFrameContext` and rejects an RSP/stack-pointer mismatch; `DW_OP_call_frame_cfa` evaluates `.eh_frame` from that exact historical frame's runtime PC, stack pointer, frame pointer, recovered RBX, and `read_snapshot_memory()` bytes;
- the existing `DW_OP_fbreg` path is now available to validated historical frames without broadening its expression or type surface: one fbreg operation, checked signed address arithmetic, bounded one-to-eight-byte integer values, and the existing snapshot-memory materializer only;
- historical XMM register ownership remains frame-zero-only, unsupported frame-base/location operations remain explicit failures, and no live ptrace register or crash-frame register is substituted for missing caller evidence;
- the core API selects frame #1 and recovers `caller_stack_local == 0xcafebabedeadbeef` with `SnapshotCoreMemory` provenance in GCC and Clang-large genuine cores;
- switching to the sibling thread resets selection to its own frame #0 and cannot revive `caller_stack_local`; switching back to the crash thread again resets to frame #0 and requires an explicit frame #1 selection before the caller local is recoverable again;
- the same integration launches the real `mdbg-core` executable and proves `frame 1` followed by `print caller_stack_local` renders the historical value with immutable core-memory provenance.

## Explicit boundary

Phase 21 proves historical stack ownership for one compiler-produced bounded integer local. It does **not** broaden historical XMM ownership, add pointer/aggregate variants, enumerate new DWARF opcodes, interpret arbitrary frame-base expressions, or reconstruct mutable/live process state. Further source-value shapes require independent active evidence and are not a reason to keep Phase 21 open.

The historical frame's `runtime_pc` remains the immutable unwind resume PC. Phase 21 does not redefine that identity merely to make source lookup convenient.

## Phase 22 promotion — historical return-PC lookup ownership

The Phase 21 evidence work exposed the next architectural boundary before production code was changed: a recovered caller frame owns a **resume PC** (the address after the call), while DWARF subprogram/source/location ranges use half-open code ownership. A genuine caller whose call sits at the end of its emitted range can therefore have a valid unwind frame whose raw return PC is exactly at the caller `high_pc`, causing source/subprogram ownership lookup to disappear even though unwinding itself is correct.

The first Phase 22 slice must keep those two identities distinct rather than rewriting the historical frame:

1. obtain a real GCC/Clang core where a recovered non-top frame's return/resume PC lands outside or on the boundary of the source/subprogram range that owns the call site; do not hand-write DWARF or fabricate a frame;
2. preserve the immutable unwind resume PC exactly as recorded/recovered for CFI progression and frame identity;
3. derive a separate x86-64 historical **lookup PC** only when compiler/core evidence proves the adjustment is required, with checked underflow and no adjustment of frame zero;
4. route symbol/source/DWARF lexical ownership for that historical frame through the lookup PC while keeping memory/register/frame-base reconstruction anchored to the immutable frame context;
5. prove stale thread/frame validation still rejects mismatched ownership and that lookup-PC normalization cannot silently change unwind sequencing;
6. demonstrate the behavior through the core API and real `mdbg-core` under both permanent compiler lanes, with PIE/non-PIE coverage where deterministic.

Do not turn Phase 22 into generic instruction decoding or blanket `pc - 1` folklore. If the permanent compiler lanes cannot produce a stable genuine-core boundary case, audit a different post-mortem subsystem instead of manufacturing one.

## Selection rule

Choose the smallest genuine historical-frame scenario where unwind ownership is already correct but raw return-PC source/symbol/DWARF ownership is demonstrably wrong. The slice is complete only when resume-PC identity and lookup-PC identity remain explicit, separately validated, and executable through the immutable core workflow.
