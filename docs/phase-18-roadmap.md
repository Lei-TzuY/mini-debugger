# Phase 18 Roadmap — bounded typed pointer dereference in genuine cores

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 18 composes the existing typed DWARF local-value evaluator with immutable snapshot-memory recovery so an already-recovered source pointer can be followed exactly once without introducing a general expression engine.

## Genuine compiler/core evidence

The permanent GCC and Clang-large lanes build the existing optimized XMM/core fixture with an additional genuine `uint64_t *scalar_pointer` whose pointee is a compiler-owned `uint64_t` initialized to `0x8877665544332211`. The normal Ubuntu GitHub Actions workflow emits the same genuine Linux ET_CORE artifact used by the FPREGSET/XMM source-value tests.

The first test-first gate kept the normal CMake build and all 54 CTest cases green, then failed only when the genuine-core source-value integration requested pointee metadata and `CoreInspectionSession::dereference_value()` that did not yet exist. This separated the new source-navigation gap from the already-stable core parser, unwind, XMM, and snapshot-memory layers.

## Completed executable slice

- the existing DWARF pointer-type path now retains one bounded pointee descriptor instead of validating and discarding it;
- the retained descriptor is deliberately limited to an already-supported signed/unsigned integer pointee of 1–8 bytes;
- pointer recovery still goes through the same selected-frame local-value evaluator and the same already-parsed DIE set; no second public DIE walker or parallel expression evaluator was introduced;
- frame-zero source-value dispatch now sends only the compiler-proven `DW_OP_reg17 (xmm0)` floating case to the XMM materializer, so a frame-zero pointer represented by ordinary snapshot memory can continue through the existing memory-backed path;
- `CoreInspectionSession::dereference_value(name)` revalidates selected thread/frame ownership, recovers the named source pointer through `inspect_local_value()`, then reads exactly one pointee through `read_snapshot_memory()`;
- immutable core bytes remain authoritative and the existing explicit runtime-artifact fallback keeps its module/file/offset provenance and ambiguity rules;
- the returned dereferenced value is an ordinary bounded integer source value named `*<name>` and does not retain another pointee descriptor, so the API does not create an implicit pointer chain;
- `mdbg-core deref <name>` exposes exactly that one operation. It accepts one source name only; it does not parse `*expr`, pointer arithmetic, member syntax, array indexing, or arbitrary C/C++ expressions;
- the genuine-core integration proves `scalar_pointer` retains pointer identity and unsigned 8-byte pointee metadata and that dereference recovers `0x8877665544332211` with snapshot-memory provenance;
- the CLI gate proves the same core session can recover crash-thread XMM source state, dereference the scalar pointer, switch immutable threads, and recover the sibling XMM value without cross-thread/frame ownership leakage;
- permanent GCC and Clang-large lanes validate the complete API and CLI workflow for PIE and non-PIE fixture artifacts.

## Explicit fail-closed boundary

Phase 18 does not implement recursive pointer navigation. Null pointers are rejected before a memory read. Unmapped, unavailable, ambiguous, or mixed-provenance pointee bytes inherit the existing `read_snapshot_memory()` failure rules. Aggregate, array, pointer-to-pointer, unsupported-width, or otherwise unsupported pointee types fail during the existing bounded type-resolution path rather than being guessed from raw bytes.

The returned dereferenced scalar is a terminal value for this milestone. Adding a second dereference, member traversal, offsets, casts, or an expression grammar would require a new evidence-backed phase.

## Phase 19 promotion — bounded aggregate navigation through pointers

The next architectural hypothesis is composition of the repository's **already-supported bounded flat-structure decoder** with the now-proven one-step pointer navigation path.

The repository can already materialize a bounded structure value from snapshot memory, including named integer members, and `mdbg-core` can already render that structure. What remains intentionally unsupported is a pointer whose compiler-proven pointee is such a bounded structure: the Phase 18 pointer metadata accepts integer pointees only, so these two mature capabilities are not yet connected.

A first Phase 19 slice must begin from a genuine compiler-produced `struct *` local in a real core artifact and prove all of the following before production expands:

1. the existing local-value evaluator identifies the pointer and its pointee structure through the same DIE/type graph rather than a new parser;
2. the pointee structure satisfies the existing byte-size/member-count/member-layout bounds and contains only already-supported scalar members;
3. one `deref <name>` operation reads the aggregate through `read_snapshot_memory()` and preserves core/runtime-artifact provenance;
4. the result reuses the existing `LocalValueKind::Structure` / member renderer rather than creating pointer-specific aggregate output;
5. selected thread/frame ownership and all snapshot-memory ambiguity rules remain authoritative;
6. arrays, nested aggregates, bit-fields, inheritance, pointer members, recursive traversal, `ptr->field` grammar, and arbitrary expression syntax remain outside the first slice unless a concrete compiler/core artifact independently requires one exact extension.

If a stable GCC/Clang genuine-core `struct *` scenario cannot be demonstrated, do not hand-write DWARF or fabricate memory layouts. Audit the next architectural frontier instead.

## Selection rule

Phase 19 work begins only from a genuine compiler- and core-produced aggregate-pointer scenario that the current Phase 18 implementation rejects. Once one coherent aggregate navigation slice is proven, seal it instead of enumerating structure/member variants.
