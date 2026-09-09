# Phase 22 Roadmap — historical return-PC lookup ownership

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 22 separates two identities that Phase 21 intentionally kept unresolved: the immutable historical **resume PC** recovered by CFI and the code-ownership **lookup PC** used for symbol/source/DWARF half-open ranges. The completed slice is driven by real GCC and Clang cores and does not rewrite unwind state or introduce a generic instruction decoder.

## Genuine compiler/core evidence

The permanent `xmm_core_value_fixture` keeps `caller_with_stack_local()` as an ordinary compiler-produced caller but removes the artificial instruction after its indirect call. A zero-length `snapshot_caller_resume_probe` label records the genuine return address without extending the function range.

Before production changed, the readelf oracle proved the boundary under both permanent compiler lanes:

- GCC emits `caller_with_stack_local` as `[0x122d, 0x125a)` with `resume_pc = 0x125a` and the call-site lookup candidate at `0x1259`; the caller local remains compiler-owned there through `DW_OP_fbreg -32` with `DW_OP_call_frame_cfa`;
- Clang-large emits the analogous `[0x1240, 0x1258)` boundary with `resume_pc = 0x1258`, lookup candidate `0x1257`, and `DW_OP_fbreg 0` with `DW_OP_reg7 (rsp)`;
- all 54 ordinary CTest cases remained green while the genuine-core source-value integration failed only because the raw historical resume PC no longer belonged to the caller subprogram/location range.

No DWARF bytes, synthetic stack frame, or fabricated PC is part of the evidence.

## Completed executable slice

- `SnapshotInspectionFrameContext::runtime_pc` remains the immutable unwind/resume PC; Phase 22 does not subtract from, rewrite, or replace it;
- `snapshot_frame_lookup_pc()` gives frame zero its exact runtime PC and gives only a recovered non-top x86-64 frame the checked `resume_pc - 1` call-site identity; zero underflow is rejected explicitly;
- `validate_snapshot_frame_lookup_pc()` is part of inspection-session frame validation, so frame-zero identity, historical normalization, stale-frame ownership, and thread/frame selection remain fail-closed;
- `CoreInspectionSession` exposes frame-aware symbol/source lookups that validate the selected frame before using its lookup PC, while generic raw-address lookup remains available as a distinct exact-address operation;
- snapshot local/subprogram/lexical/location ownership resolves through the lookup PC and requires the resolved module to remain identical to the immutable frame's recorded module;
- `DW_OP_call_frame_cfa`, stack/register ownership, CFI cursor progression, and snapshot-memory materialization continue to use the original immutable frame context and raw resume PC;
- frame-aware CLI rendering (`bt`, `frame`, and `list`) uses the lookup identity for symbol/source ownership but still displays the raw historical frame address, so presentation does not erase unwind identity;
- the permanent API test proves frame zero is never normalized, frame #1 has `lookup_pc + 1 == resume_pc`, caller symbol/source ownership returns, and switching to another thread rejects the previously saved historical frame;
- selecting the crash thread again rebuilds frame #1 with the exact same raw resume PC, proving lookup normalization cannot silently mutate unwind sequencing;
- the genuine caller stack local still resolves to `0xcafebabedeadbeef` with immutable `SnapshotCoreMemory` provenance, and the real `mdbg-core` subprocess renders caller symbol/source plus the historical value;
- GCC and Clang-large permanent lanes exercise the real Linux `ET_CORE` workflow in PIE and non-PIE artifacts.

## Explicit boundary

Phase 22 is not a blanket address-normalization policy. It does not adjust frame zero, arbitrary raw address lookups, live debugger stops, breakpoint PCs, or CFI cursor identity. It does not decode the instruction preceding an arbitrary PC. The `resume_pc - 1` rule exists only for CFI-recovered non-top x86-64 frames and is backed by the permanent genuine-core compiler evidence above.

The phase does not broaden DWARF opcode support, source-value types, or historical register reconstruction. A future architecture that cannot establish return-address semantics from an already recovered historical frame must not reuse this helper by analogy.

## Phase 23 promotion — genuine-core signal-frame recovery

The next architectural gap is not another historical return-PC variant. The current snapshot backtrace progresses only through ordinary module-owned `.eh_frame` cursors: every current/caller instruction pointer must resolve through an NT_FILE mapping to a module whose normal CFI can recover the next frame. There is no explicit model for an x86-64 Linux signal-delivery frame or `rt_sigreturn` trampoline that restores an interrupted machine context.

A first Phase 23 slice must therefore start from evidence, not from a hand-authored `ucontext_t` parser:

1. build a real GCC/Clang fixture that installs a `sigaction` handler, enters the handler from ordinary application code, and crashes inside the handler so the kernel writes a genuine `ET_CORE` containing the signal-delivery frame;
2. use the permanent core workflow to prove exactly where current snapshot unwinding stops or loses ownership when trying to cross from the handler/trampoline back to the interrupted application context;
3. record the kernel/compiler-produced stack/register/trampoline evidence before production changes, and reject the slice if the permanent lanes do not yield a stable bounded x86-64 layout;
4. if evidence is stable, recover exactly one interrupted context through an explicit signal-frame ownership path while preserving immutable core bytes/registers and the existing ordinary CFI path on both sides;
5. validate restored RIP/RSP and module ownership before resuming ordinary `.eh_frame` unwinding; malformed, ambiguous, missing, or non-x86-64 signal-frame evidence must remain explicit failures;
6. prove API and real `mdbg-core` backtrace behavior under both permanent compiler lanes, with PIE/non-PIE coverage where deterministic;
7. do not turn the milestone into a generic kernel ABI decoder, libc unwinder, or speculative signal-frame scanner.

If a genuine signal-handler core does not expose a stable recoverable boundary in the permanent lanes, Phase 23 must no-op and audit another post-mortem subsystem rather than manufacture one.

## Selection rule

Phase 22 is sealed after the one compiler/core-proven return-address ownership boundary. Phase 23 begins only from a genuine signal-delivery core whose ordinary unwind path demonstrably fails at the signal-frame boundary; repeated caller-boundary, source-location, or local-value variants are not roadmap progress.
