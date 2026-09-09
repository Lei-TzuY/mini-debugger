# Phase 14 — Immutable Process Identity Metadata

Status: complete for the current bounded Linux x86-64 core-forensics milestone.

## P14-A — Kernel-recorded process identity

Complete.

This slice is driven by a genuine Linux core emitted by the existing deterministic SIGSEGV fixture. The test inspects the kernel artifact before production parsing and requires a `CORE/NT_PRPSINFO` note whose descriptor matches the demonstrated native x86-64 `elf_prpsinfo` layout.

- `CoreSnapshot` recognizes only the demonstrated `CORE/NT_PRPSINFO` surface and requires the exact native x86-64 descriptor size.
- The immutable `CoreProcessInfo` record preserves the kernel-recorded PID, parent PID, process-group ID, session ID, bounded process filename, and bounded process command text.
- Process text is bounded by the fixed `pr_fname` / `pr_psargs` fields. NUL termination is honored when present and kernel truncation remains observable rather than being reconstructed from host state.
- The recorded process PID must be positive and must exist in the already-authoritative `NT_PRSTATUS` thread catalogue. Contradictory PID evidence fails closed; parent/group/session relationships are not guessed because no independent core authority currently exists for those fields.
- Malformed descriptor size and duplicate `NT_PRPSINFO` notes fail closed. A core with no `NT_PRPSINFO` remains valid, exposes no fabricated process metadata, and preserves independent crash/thread evidence.
- `CoreInspectionSession` exposes the same immutable record without changing thread, frame, module, source, or memory ownership.
- `mdbg-core process` renders only the kernel-recorded bounded record. It does not scrape host `/proc`, infer identity from `NT_FILE`, or reconstruct truncated command text.
- Genuine-core integration cross-checks the emitted PID against the child that crashed, the parent PID against the test parent, preserves the kernel process relationships/text exactly, proves malformed/PID-contradictory/absent-note behavior, and exercises the read-only CLI.

This seals the bounded process-identity milestone. Additional `elf_prpsinfo` fields or note variants are not a checklist; they require a new debugging/forensics workflow with independent evidence.

## Phase 15 promotion — evidence-gated process startup metadata

The next architectural hypothesis is immutable process-startup metadata recorded in a genuine Linux core, with `CORE/NT_AUXV` as the first candidate evidence source.

Phase 15 must not assume the note or any entry is present. A first slice must begin by proving that the controlled kernel core actually contains a bounded native auxiliary-vector record, then select only entries that materially improve post-mortem ownership and can be validated against existing authorities where possible. Candidate evidence includes executable/interpreter startup addresses and page-size metadata, but no key is supported merely because Linux defines it.

Any accepted auxiliary-vector slice must keep core bytes authoritative, preserve unknown entries without inventing meaning or simply reject them outside the bounded consumer, cross-check applicable values against existing `NT_FILE` / ELF image ownership where a deterministic relationship exists, and remain inert when the optional note is absent.

Do not build a generic ELF-note or auxv browser, consult the crashed process's former `/proc` state, dereference pointer-valued entries without separately proven snapshot ownership, or enumerate `AT_*` keys without an executable post-mortem use case.

## Selection rule

Choose the highest-value core-forensics capability only after a genuine artifact demonstrates the missing evidence and the current implementation cannot expose or consume it safely. Once one coherent note family is covered for its demonstrated workflow, seal it rather than farming note or field variants.
