# Phase 10 roadmap

Phase 10 begins after the bounded Phase 9 relocated-artifact milestone. Phase 9 established explicit ownership of the host runtime ELF and optional GNU separate debug companion while preserving the exact `NT_FILE` pathnames recorded in the immutable core. The next demonstrated failure is different: Linux core-dump policy may omit clean file-backed pages even though the core still records enough `NT_FILE` ownership to identify the corresponding bytes in an explicitly owned runtime artifact.

The Phase 10 architectural hypothesis is **omitted file-backed core-memory reconstruction with explicit provenance**. Captured core bytes remain authoritative evidence. Host-artifact bytes may only fill a range that the core did not capture at all and that one recorded mapping owns exactly; they must never silently replace anonymous, dirty, captured, mixed-provenance, or otherwise ambiguous snapshot memory.

## P10-A: bounded omitted file-backed memory inspection — complete

Completed executable capability:

- `read_snapshot_memory()` is a bounded read-only snapshot memory provider. Requests must be non-empty, at most 4096 bytes, and free of address-range overflow;
- every requested byte is checked against `CoreSnapshot` first. If the complete range is captured, the result is returned with `SnapshotMemoryProvenance::Core` and no host artifact is consulted;
- a range containing both captured and omitted bytes is rejected rather than stitched across trust domains. Phase 10 does not guess which source should win at an internal boundary;
- artifact fallback is allowed only when the entire requested range is absent from captured `PT_LOAD` bytes and is wholly owned by one `NT_FILE` mapping with an absolute recorded pathname;
- runtime file offset is derived exactly as `mapping.file_offset + (address - mapping.start)` with overflow and end-of-file checks before bytes are read;
- the existing `SnapshotModulePathResolver` supplies the host runtime artifact. Recorded module identity remains separate from the local file path, so Phase 9 relocation policy is reused instead of adding basename search or filesystem scanning;
- `SnapshotMemoryRead` reports whether bytes came from the core or the runtime artifact and, for fallback reads, carries the recorded module path, resolved host file path, and exact artifact file offset;
- `CoreInspectionSession::read_memory()` and the read-only `mdbg-core x <address> <length>` command expose the same provider. CLI output labels `core` versus `artifact:<recorded-module> file+<offset>` provenance rather than presenting reconstructed bytes as physically captured core evidence;
- integration generates genuine Linux kernel cores with `/proc/self/coredump_filter` restricted so clean file-backed mappings are omitted. The test reads the real crash-frame code address, requires artifact provenance and exact recorded module ownership, and runs for PIE and non-PIE under both permanent GCC and Clang-large lanes;
- unavailable runtime artifacts, ranges outside one recorded mapping, mixed captured/omitted ranges, invalid lengths, arithmetic overflow, and artifact EOF violations remain deterministic failures.

This milestone does not rewrite `CoreSnapshot::read_memory()`, fabricate missing anonymous memory, merge captured and artifact bytes, infer dirty-page equivalence, or route every snapshot CFI/DWARF consumer through host files. `CoreSnapshot` remains immutable captured evidence; the new provider is an explicit inspection layer above it.

## Phase 10 bounded milestone — complete

P10-A closes the concrete failure selected at the end of Phase 9: a genuine kernel core omits a clean file-backed instruction byte that an explicitly owned runtime artifact can reconstruct exactly. Extending fallback to more mapping classes, larger arbitrary reads, mixed ranges, or packaging variants without an independently failing workflow would be memory-provider farming, so the omitted-memory milestone is sealed here.

## Phase 11 promotion: provenance-aware snapshot memory consumers — next architectural hypothesis

Higher-level immutable inspection currently has distinct memory ownership needs. Stack/anonymous bytes used by CFI are expected to be captured core evidence, while a future compiler-produced DWARF expression or another inspection workflow could legitimately require a file-backed byte omitted by the kernel. Phase 11 must not globally replace `CoreSnapshot::read_memory()` with artifact fallback merely because P10-A exists.

The first Phase 11 slice must begin with a real reproducible higher-level failure—such as snapshot CFI, source-value evaluation, or another immutable inspection operation that demonstrably needs an omitted file-backed byte. Only then may that consumer receive a provenance-aware provider, and its acceptance must prove that captured bytes still dominate, anonymous/dirty evidence never falls back to a host artifact, recorded module identity remains immutable, and missing or mismatched ownership fails closed.

## Selection rule

Choose the smallest real post-mortem workflow that requires memory evidence not already expressible by the immutable core session. Do not enumerate coredump-filter combinations, mapping types, page-boundary variants, or DWARF consumers speculatively. Host-file bytes are reconstructed evidence with explicit provenance, never a silent replacement for bytes the core actually captured.
