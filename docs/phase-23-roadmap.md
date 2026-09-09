# Phase 23 roadmap — genuine-core signal-frame recovery

Status: **complete for the current bounded Linux x86-64 milestone**.

Phase 23 extends immutable post-mortem inspection across one genuine Linux signal-delivery frame without treating a signal frame as ordinary call/return CFI and without scanning core memory heuristically for `ucontext_t`-like bytes.

## Evidence gate

The permanent GCC and Clang-large lanes build a real `sigaction` fixture in PIE and non-PIE form. Ordinary application code is interrupted inside an explicit probe, the kernel constructs the signal-delivery frame, the handler records the kernel-provided `ucontext_t` address and saved RIP/RSP/RBP/RBX as an oracle, and the handler crashes to produce a genuine `ET_CORE` artifact.

Across both compiler lanes and both executable layouts the observed ownership sequence is stable:

1. handler crash instruction;
2. signal-handler frame;
3. libc signal-restorer frame whose stack pointer is the recorded kernel `ucontext_t` boundary;
4. the exact interrupted application RIP/RSP recovered from that `ucontext_t`;
5. ordinary application caller frames recovered again through normal `.eh_frame` CFI.

The integration independently reads the saved register slots from immutable core memory and requires them to match the fixture's kernel oracle before accepting recovery. It also requires the restored interrupted frame to own an exact instruction PC, not a historical return PC, so Phase 22's checked call-site normalization is not applied to signal-restored execution state.

The real `mdbg-core` CLI consumes the same generated core and `bt` must show the signal handler, the restored `signal_core_interrupted_probe`, and `main` under GCC/Clang-large and PIE/non-PIE. This keeps the user-facing post-mortem path behind the same evidence gate as the API.

## Bounded implementation

- `.eh_frame` CIE/FDE metadata is parsed only far enough to identify an explicit `S` signal-frame augmentation owner for the current x86-64 objects. Unknown/malformed metadata remains a hard failure.
- Signal restoration is entered only when normal module resolution identifies the current frame as an explicit signal-frame FDE. There is no generic stack scanner and no guess based on libc symbol names.
- The current Linux x86-64 recovery reads the evidence-proven `ucontext_t` general-register slots needed for unwind continuity: RIP, RSP, RBP, and RBX.
- Saved RIP and RSP must be non-zero, and the restored RSP must satisfy the same-stack relationship proven by the permanent runner evidence. Ambiguous or malformed layouts fail explicitly.
- The restored interrupted frame is tagged `SnapshotFramePcOwnership::ExactInstruction`; ordinary recovered callers remain `ReturnAddress` owners. Symbol/source lookup therefore preserves the raw interrupted PC while historical call-site frames continue to use the checked `resume_pc - 1` lookup identity.
- Core bytes, thread registers, and runtime artifacts remain immutable. Signal recovery only derives inspection state; it does not mutate the snapshot or synthesize a resumable process.

## What Phase 23 deliberately does not claim

This milestone is not a generic Linux signal ABI decoder, not nested-signal/alternate-stack conformance, not cross-architecture recovery, and not a replacement for libc/kernel unwind libraries. Those surfaces require their own stable external evidence and must not be farmed as variants of this milestone.

## Phase 24 promotion — signal-restored source-value register context

Phase 23 can now recover the interrupted frame and continue ordinary CFI, but its signal restoration transports only the small register subset required for unwind continuity. `CoreInspectionSession::inspect_value()` evaluates compiler-owned DWARF locations from `SnapshotInspectionFrameContext::registers`; therefore a local that was genuinely live in another interrupted GPR can remain unavailable even though the kernel saved that GPR in the same validated `ucontext_t`.

Phase 24 is the next architectural frontier: preserve evidence-proven Linux x86-64 GPR state for the signal-restored exact frame so source-value inspection can consume the interrupted register file without pretending later ordinary caller frames own those registers.

Acceptance criteria:

1. Start from a genuine GCC/Clang signal-handler core, not a synthetic `ucontext_t` byte fixture.
2. Add one compiler-produced interrupted local whose DWARF location is demonstrably register-backed at the saved RIP; record the actual emitted register/location evidence before production changes.
3. Extend the explicit signal-frame recovery result to carry only the validated Linux x86-64 GPR slots needed by that real case into the restored frame's immutable `InspectionRegisterState`.
4. Do not copy the interrupted register file into frames produced later by ordinary CFI unless those registers are independently recovered by CFI rules; frame ownership must stay explicit.
5. Prove `CoreInspectionSession::inspect_value()` and real `mdbg-core frame <index> / print <name>` recover the compiler-produced interrupted value under both permanent compiler lanes and PIE/non-PIE where deterministic.
6. Malformed/truncated signal contexts, unsupported registers/locations, non-signal frames, and ambiguous ownership must continue to fail explicitly.

If no stable compiler-produced register-backed interrupted local can be demonstrated in the permanent lanes, Phase 24 must no-op rather than add speculative ucontext register plumbing.
