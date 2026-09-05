# Phase 4 roadmap

Phase 4 starts after the Phase 3 process-domain registry removes the remaining one-image/one-process-topology assumption. The next architectural objective is to make source-level debug data executable: the debugger should be able to identify a variable in the current source scope, evaluate the compiler-produced DWARF location that owns its value, read that value from the selected process/TID, and present it without bypassing the existing module, register, memory, CFI, or process-domain ownership models.

## Priority 0: bounded local integer value inspection — complete

The first source-value slice is executable under the permanent GCC and Clang-large gates:

- the existing DWARF4 C fixture now contains a noinline `inspect_local_value` function whose `uint64_t local_value` is derived from a volatile runtime seed and remains live at the exported `local_value_probe`;
- `inspect_local_integer` consumes the owning module's real `.debug_info`, `.debug_abbrev`, and `.debug_str`, with DWARF32/version-4/8-byte-address bounds and explicit rejection for forms outside the compiler-proven surface;
- the selected runtime PC is routed through the existing module resolver and ELF load bias before DIE lookup, so debug information from unrelated modules is never searched as if address spaces were interchangeable;
- the bounded DIE walk resolves the active subprogram, one direct-child local by name, its typedef chain, and a 1–8 byte signed/unsigned base integer type; missing, ambiguous, nested-scope, malformed, or unsupported structures fail explicitly;
- compiler-produced location evaluation is deliberately narrow: the local must use `DW_OP_fbreg`; GCC's `DW_AT_frame_base = DW_OP_call_frame_cfa` is evaluated through the existing `.eh_frame` CFI cursor, while Clang's `DW_OP_reg6 (rbp)` uses the selected thread's register state;
- the final value read goes through `Debugger::read_memory`, preserving selected process/TID ownership instead of introducing a second ptrace path;
- the CLI exposes `print <name>` and renders the resolved signed or unsigned integer according to the recovered base-type width;
- direct API and real `mdbg` subprocess integration stop at `local_value_probe`, observe `0x1020304050607080`, reject a missing local explicitly, then continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

The supported form/opcode set remains intentionally bounded to what those real compiler fixtures emitted; Priority 0 does not claim a general DIE, expression, location-list, aggregate, or optimized-variable engine.

## Priority 1: register-resident formal parameters — current frontier

A post-Priority-0 compiler probe provides the next concrete gap without speculative opcode enumeration. With a noinline optimized (`-Og`/`-O1`, DWARF4) function taking a `uint64_t parameter`, both GCC and Clang emit the parameter as a direct child `DW_TAG_formal_parameter` with `DW_AT_location = DW_OP_reg5 (rdi)`. The same probe already pushes its ordinary local into `.debug_loc`, so parameter-register support is the smallest independent step before location-list ownership.

Current Priority 1 acceptance:

- add one deterministic optimized compiler fixture whose formal parameter has a known non-constant runtime value and whose probe stops while that parameter is live;
- prove the existing Priority 0 evaluator rejects or cannot resolve that formal parameter before adding production support;
- extend name resolution only far enough to accept an unambiguous direct-child `DW_TAG_formal_parameter` alongside the existing direct-child local;
- evaluate only the compiler-proven direct-register operation required by the fixture (`DW_OP_reg5` initially) and read it through the selected thread's existing register snapshot; do not generalize to the complete DW_OP_regN family unless another executable lane requires it;
- preserve the same bounded integer type resolution, module ownership, explicit failure behavior, and `print <name>` presentation established by Priority 0;
- prove PIE/non-PIE and GCC/Clang-large end to end, including a clean exit and a negative lookup;
- do not add `.debug_loc` parsing in the same slice. The compiler probe deliberately establishes location lists as the next distinct ownership problem after direct-register parameters.

## Later priorities

After Priority 1, the next concrete compiler-produced gap is location-list ownership for optimized locals: the same probe emits the local through `.debug_loc` (GCC with location views, Clang with a conventional range list) and places its live value in a register over a bounded PC range. Lexical shadowing, pointer/aggregate type presentation, and a bounded expression surface remain later candidates and must still be ordered by executable evidence rather than breadth.

## Selection rule

Priority 1 is the current frontier. Select the smallest real compiler-produced formal-parameter workflow that fails because direct-register value locations are unsupported, prove that failure first, and implement only the DIE tag/location operation necessary to make that workflow correct under the existing GCC and Clang-large CI gates. Do not pull `.debug_loc` or broad DWARF expression support into the same PR merely because the compiler probe exposed them nearby.
