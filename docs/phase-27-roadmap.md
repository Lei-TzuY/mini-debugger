# Phase 27 Roadmap — signal-restored historical pointer provenance

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 27 composes the previously proven signal-frame register ownership, bounded DWARF pointer typing, immutable snapshot memory, and one-step typed dereference into one historical object-reachability path. The slice is compiler/kernel/core driven: no synthetic DWARF, hand-authored register database, or signal-specific dereference engine is introduced.

## Completed executable slice

- a dedicated synchronous-SIGILL fixture keeps a real `uint64_t *interrupted_pointer` formal live in RDI across the exported `signal_pointer_interrupted_probe`; permanent GCC and Clang-large lanes compile PIE and non-PIE artifacts with `-O1 -g -gdwarf-4`;
- `readelf` is an independent compiler oracle: the fixture must retain the formal and a compiler-produced `DW_OP_reg5 (rdi)` location that covers the exact interrupted probe PC before source-value evaluation is attempted;
- the SIGILL `SA_SIGINFO` handler publishes the kernel-provided `ucontext_t`, then deliberately overwrites handler/crash-time RDI with `0x0ddc0ffeebadf00d` before crashing, so NT_PRSTATUS cannot accidentally masquerade as the interrupted application's pointer ownership;
- the integration independently reads Linux x86-64 `ucontext_t` general-register slots from genuine core bytes and requires saved RDI to equal the runtime address of `signal_pointer_target`, while saved RIP must equal the exact exported synchronous-fault probe;
- `signal_pointer_target` is zero in the ELF/runtime artifact and is changed only at runtime to `0x7a6b5c4d3e2f1908`; reading that sentinel from the core at the saved RDI address therefore proves genuine immutable core-memory reachability rather than file-image fallback;
- signal-frame recovery attaches restored RDI ownership only to the exact interrupted frame; later ordinary CFI frames are explicitly required not to inherit that caller-saved register;
- snapshot source-value evaluation now accepts exactly one scalar compiler-proven `DW_OP_reg5` operation when the selected inspection frame owns RDI, preserves bounded integer/pointer width rules, marks the pointer itself as `SnapshotCoreRegister`, and reuses the existing pointer metadata resolver;
- the existing `dereference_local_pointer()` path performs the object read: the dereferenced `uint64_t` must equal the runtime sentinel and carry `SnapshotCoreMemory` provenance, proving the register-origin pointer and immutable core-memory pointee remain distinct provenance stages;
- the genuine-core integration exercises both the direct `CoreInspectionSession` API and a real sibling `mdbg-core` session using `frame`, `print interrupted_pointer`, and `deref interrupted_pointer`;
- the same compiler/kernel/core/evaluator/CLI path is required under GCC and Clang-large, PIE and non-PIE.

Test-first evidence intentionally reached the genuine signal cores and passed the compiler-location, `ucontext_t`, exact-PC, runtime-mutated-core-byte, and restored-frame ownership gates before failing only at the previous snapshot evaluator error that rejected scalar `DW_OP_reg5`. The production change is therefore bounded to that proven gap.

## Explicit boundary

Phase 27 does **not** generalize snapshot source evaluation to arbitrary `DW_OP_regN`, infer caller-saved registers through ordinary CFI, accept missing frame ownership, copy crash-time PRSTATUS values into historical frames, add pointer-to-pointer traversal, or broaden pointee types beyond the already established bounded integer/flat-structure model.

A restored pointer may be inspected only when compiler DWARF and the selected historical frame agree on its concrete register ownership. Null, invalid, unmapped, ambiguous, stale, missing-type, or unsupported-location cases remain fail-closed through the existing snapshot/type/memory layers.

## Phase 28 promotion — bounded typed object traversal

The next architectural frontier is not another restored-register variant. The current post-mortem type graph deliberately stops after one pointer dereference: `LocalStructMemberType` retains only direct scalar member name/offset/width/signedness, and the Phase 19 boundary explicitly excluded pointer-valued structure members, `ptr->field`, pointer-to-pointer expansion, recursive traversal, and a second dereference.

Phase 28 should begin only from a genuine GCC/Clang core artifact whose compiler-produced DWARF describes one bounded structure containing a pointer-valued direct member and whose immutable memory contains deterministic reachable bytes. The first coherent slice must prove all of the following before broadening the expression surface:

1. independently establish the compiler-produced structure/member/type graph under both permanent compiler lanes; no synthetic DIEs or hand-authored type metadata;
2. extend the bounded member-type model only enough to retain one direct pointer member while preserving the existing aggregate-size/member-count/offset bounds;
3. expose one explicit typed member hop from an already-proven structure object and one further bounded dereference; do not build a general C/C++ expression parser;
4. preserve provenance at every hop: register/core/artifact ownership for the root pointer or object, core/artifact ownership for the containing structure, and core/artifact ownership for the second pointee must remain independently visible;
5. reject null, unmapped, unsupported pointee kind, out-of-range member layout, ambiguous module/debug ownership, and depth/cycle violations deterministically before uncontrolled traversal;
6. use existing snapshot memory and type decoders rather than introducing a parallel object-memory reader;
7. prove the path through both the core API and a real `mdbg-core` command workflow under GCC and Clang-large, with PIE/non-PIE coverage where the compiler emits the shape deterministically.

One successful direct pointer-member traversal is enough to establish the architecture. Arrays, inheritance, bit-fields, arbitrary nesting, unbounded recursive object graphs, pointer arithmetic, casts, and a full expression language remain outside the first Phase 28 slice.

## Selection rule

Proceed only when a genuine compiler/core object graph demonstrates a missing typed member traversal that the existing one-step dereference path cannot represent. If both permanent compiler lanes do not produce a stable bounded shape, stop at the evidence gate and audit a different architectural frontier instead of manufacturing pointer/member variants.
