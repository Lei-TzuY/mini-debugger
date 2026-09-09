# Phase 15 — Evidence-Gated Process Startup Metadata

Status: complete for the current bounded Linux x86-64 core-forensics milestone.

## P15-A — Kernel-recorded auxiliary-vector startup evidence

Complete.

This slice starts from a genuine Linux core emitted by the existing deterministic SIGSEGV fixture. The integration test independently inspects the kernel artifact before consuming production startup metadata and requires a bounded native x86-64 `CORE/NT_AUXV` record terminated by `AT_NULL`.

- The bounded startup consumer accepts only a non-empty native `Elf64_auxv_t` array within the demonstrated 4096-byte evidence window and rejects malformed sizing or a missing `AT_NULL` terminator.
- Every non-null auxiliary-vector entry is preserved verbatim as immutable `(type, value)` evidence. Unknown entries are not discarded, reinterpreted, dereferenced, or promoted merely because Linux assigns them a symbolic `AT_*` name.
- The current semantic surface is deliberately limited to `AT_ENTRY`, `AT_PHDR`, `AT_PHNUM`, `AT_PAGESZ`, and `AT_BASE`, because the genuine artifact demonstrates them and they materially improve post-mortem startup ownership.
- Recognized entries are single-valued. Duplicate recognized keys fail closed; `AT_PHNUM` must be nonzero and `AT_PAGESZ` must be a nonzero power of two.
- `AT_ENTRY` and `AT_PHDR` must be nonzero and covered by the already-authoritative `NT_FILE` mapping catalogue. A nonzero `AT_BASE` must also be mapping-owned. Contradictory pointer evidence fails closed instead of being repaired from host state.
- The genuine-core integration cross-checks `AT_ENTRY` and `AT_PHDR` against the crashed executable mapping, compares the production session record with an independent parse of the kernel note, proves every non-null entry is preserved exactly, and exercises ownership contradictions, duplicate recognized keys, and a missing terminator.
- `CoreInspectionSession` exposes one immutable optional startup record. If `NT_AUXV` is absent, the core remains valid, startup metadata stays absent, and independent crash/process evidence remains available.
- `mdbg-core startup` renders only the bounded recorded fields. It does not consult the crashed process's former `/proc`, infer missing values, or dereference pointer-valued entries.

This seals the bounded startup-metadata milestone. Additional `AT_*` keys are not a checklist; they require a new executable post-mortem workflow and independent evidence.

## Phase 16 promotion — evidence-gated extended thread machine state

The next architectural hypothesis is immutable non-GPR thread machine state recorded in genuine Linux core artifacts. Floating-point, SIMD, and extended x86 state are high-value debugging candidates because the current core thread catalogue preserves general-purpose register contexts but does not yet expose those execution values.

A first Phase 16 slice must begin by proving which relevant note family the controlled kernel core actually emits for the deterministic fixture. `NT_FPREGSET` and x86 extended-state notes such as `NT_X86_XSTATE` are candidates only; neither is assumed present or supported until genuine artifact evidence establishes its owner, descriptor layout, thread association, and bounded size.

Any accepted slice must keep the core bytes authoritative, associate state with the correct `NT_PRSTATUS` thread without guessing note order beyond demonstrated kernel evidence, fail closed on malformed or contradictory descriptors, remain inert when optional state is absent, and expose a concrete post-mortem inspection workflow rather than a generic note dump.

Do not enumerate x87/SSE/AVX fields from ABI headers before the genuine core demonstrates the relevant note, copy host CPU state, infer unsupported vector widths, or build a generic ELF-note browser.

## Selection rule

Choose the next Phase 16 slice only after a genuine kernel core demonstrates the missing machine-state evidence and the current implementation cannot expose it safely. Once one coherent note family is covered for a demonstrated debugging workflow, seal it rather than farming register or note variants.
