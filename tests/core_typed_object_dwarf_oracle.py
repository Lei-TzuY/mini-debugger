#!/usr/bin/env python3

import re
import subprocess
import sys
from dataclasses import dataclass
from typing import Optional


@dataclass
class Die:
    depth: int
    offset: int
    tag: str
    name: str = ""
    type_ref: Optional[int] = None
    byte_size: Optional[int] = None
    member_offset: Optional[int] = None


def fail(message: str) -> None:
    raise RuntimeError(message)


def parse_number(text: str) -> Optional[int]:
    for token in reversed(text.strip().split()):
        cleaned = token.strip(",;()[]")
        if re.fullmatch(r"(?:0x[0-9a-fA-F]+|[0-9]+)", cleaned):
            return int(cleaned, 0)
    return None


def parse_dies(text: str) -> list[Die]:
    header = re.compile(
        r"^\s*<(\d+)><(?:0x)?([0-9a-fA-F]+)>:\s+Abbrev Number:\s+\d+\s+\((DW_TAG_[^)]+)\)"
    )
    reference = re.compile(r"<0x([0-9a-fA-F]+)>")
    dies: list[Die] = []
    current: Optional[Die] = None
    for line in text.splitlines():
        match = header.match(line)
        if match:
            current = Die(int(match.group(1)), int(match.group(2), 16), match.group(3))
            dies.append(current)
            continue
        if current is None:
            continue
        if "DW_AT_name" in line:
            current.name = line.rsplit(":", 1)[-1].strip()
        elif "DW_AT_type" in line:
            match = reference.search(line)
            if match:
                current.type_ref = int(match.group(1), 16)
        elif "DW_AT_byte_size" in line:
            current.byte_size = parse_number(line.rsplit(":", 1)[-1])
        elif "DW_AT_data_member_location" in line:
            current.member_offset = parse_number(line.rsplit(":", 1)[-1])
    return dies


def die_by_offset(dies: list[Die], offset: int, context: str) -> Die:
    for die in dies:
        if die.offset == offset:
            return die
    fail(f"{context} references missing DIE 0x{offset:x}")


def unwrap_integer(dies: list[Die], die: Die) -> Die:
    current = die
    for _ in range(8):
        if current.tag == "DW_TAG_base_type":
            if current.byte_size != 8:
                fail("typed payload pointee is not an 8-byte base type")
            return current
        if current.tag not in ("DW_TAG_typedef", "DW_TAG_const_type"):
            fail(f"typed payload pointee reached unsupported {current.tag}")
        if current.type_ref is None:
            fail("typed payload wrapper lost DW_AT_type")
        current = die_by_offset(dies, current.type_ref, "typed payload wrapper")
    fail("typed payload integer wrapper chain is too deep")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: core_typed_object_dwarf_oracle.py <fixture>", file=sys.stderr)
        return 2
    fixture = sys.argv[1]
    output = subprocess.check_output(
        ["readelf", "--wide", "--debug-dump=info", fixture],
        text=True,
        stderr=subprocess.DEVNULL,
    )
    dies = parse_dies(output)
    if not dies:
        fail("compiler oracle found no DWARF DIEs")
    if not any(die.name == "typed_pointer" for die in dies):
        fail("compiler DWARF did not retain typed_pointer")

    structures = [
        (index, die)
        for index, die in enumerate(dies)
        if die.tag == "DW_TAG_structure_type" and die.name == "TypedObjectPointee"
    ]
    if len(structures) != 1:
        fail("compiler DWARF did not emit exactly one TypedObjectPointee structure")
    struct_index, structure = structures[0]
    if structure.byte_size != 16:
        fail(f"TypedObjectPointee size is {structure.byte_size}, expected 16")

    payload: Optional[Die] = None
    marker: Optional[Die] = None
    for die in dies[struct_index + 1 :]:
        if die.depth <= structure.depth:
            break
        if die.depth != structure.depth + 1 or die.tag != "DW_TAG_member":
            continue
        if die.name == "payload":
            payload = die
        elif die.name == "marker":
            marker = die
    if payload is None or marker is None:
        fail("TypedObjectPointee direct members are incomplete")
    if payload.member_offset != 0 or marker.member_offset != 8:
        fail(
            f"TypedObjectPointee offsets are payload={payload.member_offset} marker={marker.member_offset}"
        )
    if payload.type_ref is None:
        fail("payload member lost DW_AT_type")
    pointer = die_by_offset(dies, payload.type_ref, "payload member")
    if pointer.tag != "DW_TAG_pointer_type":
        fail(f"payload member resolved to {pointer.tag}, expected DW_TAG_pointer_type")
    if pointer.type_ref is None:
        fail("payload pointer lost pointee DW_AT_type")
    pointee = die_by_offset(dies, pointer.type_ref, "payload pointer")
    unwrap_integer(dies, pointee)

    print(
        "typed-object DWARF oracle passed: "
        f"struct=0x{structure.offset:x} payload-type=0x{pointer.offset:x}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"typed-object DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
