# Phase 25 roadmap — signal-restored FP/SIMD source-value ownership

## Status

Phase 25 is complete for the current evidence-bounded Linux x86-64 genuine-core milestone.

The phase began from the genuine signal-handler core path established by Phases 23–24. It did not infer historical SIMD state from the crashed handler's `NT_FPREGSET`. Instead, both permanent compiler lanes at the existing `-O1 -g -gdwarf-4` signal-core build keep one real interrupted `double interrupted_fp_local` live in `DW_OP_reg17 (xmm0)` at the saved application RIP, while the kernel independently preserves that interrupted XMM0 value in the signal `ucontext_t` FP state.

## Completed capability

- the signal fixture keeps `interrupted_fp_local = 1234.5` live through an XMM compiler constraint without naming a concrete XMM register; GCC and Clang both choose the compiler-produced `DW_OP_reg17 (xmm0)` location at the interrupted probe;
- the real SA_SIGINFO handler records the kernel-provided `ucontext_t::uc_mcontext.fpregs` pointer and an independent low-64-bit XMM0 oracle before modifying any FP state;
- the handler deliberately writes `-4321.25` to live XMM0 before crashing, so the top-frame `NT_FPREGSET` XMM0 differs from the interrupted application's signal-saved XMM0 and cannot accidentally satisfy the historical-frame test;
- the core integration independently reads the raw Linux x86-64 `fpregs` pointer from the genuine ucontext, requires it to remain inside the evidence-proven same-stack signal interval, and reads the legacy FXSAVE XMM0 bytes at offset 160;
- `recover_linux_x86_signal_frame()` transports only the newly proven 16-byte XMM0 state together with the previously established signal cursor/R12 state; null, truncated, overflowing, or out-of-interval FP-state evidence remains an explicit failure;
- `InspectionRegisterState` can own one optional XMM0 value, and snapshot frame construction assigns it only to the exact interrupted application frame restored across the signal boundary;
- later ordinary `.eh_frame` caller frames are explicitly required to have no inherited XMM0 ownership;
- snapshot source-value evaluation keeps frame-zero XMM reads on the selected thread's crash-time `NT_FPREGSET`, while historical frames may materialize `DW_OP_reg17` only from explicit frame-owned restored XMM0 state; there is no fallback from a historical frame to top-frame FPREGSET;
- `CoreInspectionSession::inspect_value("interrupted_fp_local")` recovers the expected IEEE-754 value with `SnapshotCoreRegister` provenance;
- the genuine-core integration launches the real sibling `mdbg-core`, selects the restored frame, executes `print interrupted_fp_local`, and requires `1234.5` in CLI output;
- GCC and Clang-large, PIE and non-PIE signal-core workflows cover the same compiler/kernel/core/restore/source-value path.

## Explicit bounds

Phase 25 does **not** generalize historical FP/SIMD recovery to arbitrary XMM registers, AVX/YMM/ZMM state, arbitrary XSAVE component layouts, or non-signal historical callers.

The bounded Linux x86-64 signal decoder consumes only the compiler- and kernel-proven legacy XMM0 bytes required by this phase. Unsupported XMM locations, missing or malformed signal FP state, ambiguous ownership, non-signal frames, and unsupported floating widths remain fail-closed.

The signal-restored XMM0 value belongs only to the exact interrupted frame. Ordinary CFI does not propagate it into callers, and a historical source-value lookup never reuses the crashed handler's `NT_FPREGSET` as a substitute.

## Phase 26 promotion — signal-restored composite register-piece ownership

The next architectural boundary is not another XMM register number. The existing live debugger already has a compiler-proven bounded register-piece aggregate model, but historical signal-restored source values still transport individual special-case registers. The next useful step is therefore to prove that the signal-frame ownership model can compose multiple independently saved registers into one source value without collapsing provenance.

Phase 26 must remain evidence-gated:

1. Start from a genuine GCC/Clang signal-handler core and demonstrate one stable interrupted aggregate/formal value whose active DWARF location at the saved application RIP is a multi-register `DW_OP_piece` shape.
2. Independently validate every required register piece from the real kernel `ucontext_t`; do not infer historical pieces from crash-time `NT_PRSTATUS` or `NT_FPREGSET`.
3. Generalize signal-restored register ownership only enough to carry the exact compiler-produced pieces, with explicit per-frame provenance and no propagation into later ordinary CFI frames.
4. Reuse the existing bounded register-piece source-value decoder rather than introducing a parallel signal-specific aggregate renderer.
5. Prove `CoreInspectionSession` and real `mdbg-core frame <index>` / `print <name>` reconstruct the aggregate under both permanent compiler lanes and PIE/non-PIE where deterministic.
6. Missing pieces, unsupported registers, malformed signal contexts, ambiguous ownership, and unsupported piece layouts remain explicit failures.

If no stable compiler-produced signal-interrupted register-piece aggregate can be demonstrated under both permanent compiler lanes, Phase 26 must no-op and the next architectural frontier must be audited instead of enumerating saved registers.

## Selection rule

Phase 26 is the next frontier. Work begins with compiler/kernel evidence, not by widening `InspectionRegisterState` or the signal decoder speculatively. A bounded PR may proceed only after a real failing workflow identifies the exact register pieces that are required.
