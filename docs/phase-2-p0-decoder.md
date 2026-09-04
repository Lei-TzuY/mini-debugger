# Phase 2 P0: bounded x86 call decoder

This milestone extracts source-`next` near-call recognition from debugger control flow into a directly testable x86-64 decoding component.

Acceptance evidence:

- existing supported call classes remain unchanged: direct `E8 rel32`, `FF /2` register/memory forms across the existing ModRM/SIB displacement cases, and the currently supported `41`/`48`/`49` register-call prefixes;
- decoding is testable without launching or attaching to a tracee;
- truncated instructions, unsupported opcode groups, unsupported REX prefixes, and REX memory calls remain explicit fallbacks;
- source stepping now asks the decoder only for instruction length and retains breakpoint/lifecycle ownership itself;
- the byte-reader interface is lazy, so direct calls and obvious non-calls do not require speculative reads beyond the opcode.

Completion of this gate permits Phase 2 to promote to dynamic-loader events and deferred breakpoints. It does not imply general-purpose x86 instruction decoding.
