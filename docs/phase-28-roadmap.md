# Phase 28 Roadmap — bounded typed object traversal

Status: complete for the current Linux x86-64 genuine-core milestone.

Phase 28 closes one deliberately bounded post-mortem object-navigation gap without turning `mdbg-core` into a C/C++ expression evaluator. The compiler-proven root is a pointer to a small structure whose direct `payload` member is itself a pointer to one bounded integer pointee.

## Completed acceptance

- A genuine GCC/Clang core fixture owns `typed_pointer -> TypedObjectPointee { uint64_t *payload; uint64_t marker; }`; `readelf` is used as an independent oracle for the compiler-produced structure, member offsets, member types, pointer type, and integer pointee metadata. No synthetic DIE graph is used.
- Snapshot DWARF type resolution retains only the metadata needed for one direct pointer-valued member: member name, constant offset, x86-64 pointer width, pointer kind, and bounded integer pointee width/signedness. Existing structure/member count and storage bounds remain authoritative.
- `CoreInspectionSession::inspect_pointer_member()` performs exactly one explicit root-pointer-to-direct-member hop. `dereference_pointer_member()` performs exactly one further dereference into the member's bounded integer pointee. There is no recursive graph walk, pointer arithmetic, array indexing, inheritance, bitfield handling, or general expression parser.
- Provenance remains explicit across every stage: the root pointer keeps register/core/artifact ownership; the containing structure is materialized only through existing snapshot-memory resolution; the selected member retains the containing object's core/artifact storage provenance; the second pointee is read again through the same snapshot-memory resolver and reports its own core/artifact provenance.
- Fail-closed behavior is preserved. Unknown direct members and non-pointer direct members are rejected. Null and host-width-invalid member pointers are rejected before the second read. Unmapped second-pointee addresses continue through bounded `read_snapshot_memory()` failure. Because the public operation has fixed depth rather than recursion, cycles cannot trigger uncontrolled traversal.
- The real `mdbg-core` product surface exposes the same bounded operations as `member <name> <member>` and `deref-member <name> <member>`. The CLI delegates to `CoreInspectionSession`; it does not duplicate DWARF or memory ownership logic.
- The genuine-core integration proves the API and CLI on both permanent compiler lanes, including PIE and non-PIE artifacts, while retaining the historical-frame, thread-selection, XMM, stack-local, scalar-pointer, aggregate-pointer, and source/provenance regression coverage.

Phase 28 is intentionally sealed here. Extending this into pointer-to-pointer recursion, arbitrary nested member paths, arrays, or a C/C++ expression language would be a new architecture decision and requires independent compiler-produced evidence rather than variant farming.

## Phase 29 promotion — bounded scoped local discovery

The next higher-value gap is discoverability rather than deeper traversal. Snapshot value inspection currently requires the user to know a local or formal-parameter name in advance. The debugger already parses lexical scopes to resolve one requested name, but it has no first-class way to enumerate the inspectable values that are active in the selected immutable thread/frame.

The first coherent Phase 29 slice must:

1. Use genuine GCC and Clang-large DWARF/core artifacts. Do not invent variable names, synthetic DIEs, or a parallel symbol database.
2. Add a bounded API that enumerates named `DW_TAG_variable` / `DW_TAG_formal_parameter` entries whose lexical scope is active at the selected frame lookup PC, using the existing scope/abstract-origin machinery.
3. Preserve shadowing semantics: if the same source name exists at multiple lexical depths, expose only the uniquely active deepest binding; reject an equal-depth ambiguity rather than silently choosing one.
4. Separate discovery from materialization. Listing a name must not pretend its value is recoverable; when useful, report bounded type/kind or availability metadata only if that evidence can be derived through existing decoders without evaluating unsupported locations.
5. Keep selected-thread and selected-frame ownership authoritative. Switching thread/frame must rebuild the discovery result from that immutable context; stale names from a previous selection must not leak forward.
6. Expose one real `mdbg-core locals` workflow over the same API and prove that at least one parameter/local set changes across genuine frame or thread selection.
7. Keep output and work bounded. Do not recursively expand objects, implicitly dereference pointers, scan unrelated compilation units after the owning subprogram is resolved, or materialize every value merely to list names.
8. Require both permanent compiler lanes and PIE/non-PIE evidence when the compiler-produced scope shape is stable. If the two compilers do not provide a stable scoped-discovery scenario, do not hand-author DWARF to force it; audit the next architectural frontier instead.

This promotes post-mortem inspection from "query a name you already know" to "discover what this immutable frame can actually offer", while keeping the typed traversal and value-recovery contracts bounded and evidence-driven.
