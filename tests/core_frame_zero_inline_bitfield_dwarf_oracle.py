#!/usr/bin/env python3
import re
import subprocess
import sys

from core_inline_dwarf_oracle import (
    active_debug_loc_expression,
    addr2line_contexts,
    clean_name,
    numeric_attr,
    origin_name,
    parse_dies,
    ref_offset,
    resolved_attr,
    resolved_name,
    run,
    symbol_address,
    unwrap_type,
)


def direct_children(records, parent):
    result = []
    found = False
    for record in records:
        if record["offset"] == parent["offset"]:
            found = True
            continue
        if not found:
            continue
        if record["depth"] <= parent["depth"]:
            break
        if record["depth"] == parent["depth"] + 1:
            result.append(record)
    return result


def require_integer_member(member, by_offset, name, encoding, bit_size):
    if clean_name(member["attrs"].get("name", "")) != name:
        raise RuntimeError(f"inline_bit_fields member mismatch: expected {name}")
    type_text = resolved_attr(member, by_offset, "type")
    if not type_text:
        raise RuntimeError(f"{name}: missing resolved DW_AT_type")
    value_type = by_offset.get(ref_offset(type_text, f"{name} type"))
    if value_type is None:
        raise RuntimeError(f"{name}: unknown type DIE")
    value_type = unwrap_type(value_type, by_offset, f"{name} type")
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{name}: not a compiler base type")
    size = value_type["attrs"].get("byte_size")
    actual_encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(size, f"{name} byte size") != 4:
        raise RuntimeError(f"{name}: base type is not four bytes")
    if not actual_encoding or numeric_attr(
        actual_encoding, f"{name} encoding"
    ) != encoding:
        raise RuntimeError(f"{name}: integer encoding changed")

    width = member["attrs"].get("bit_size")
    if not width or numeric_attr(width, f"{name} bit size") != bit_size:
        raise RuntimeError(f"{name}: compiler bit width changed")

    data_bit = member["attrs"].get("data_bit_offset")
    legacy_bit = member["attrs"].get("bit_offset")
    if (data_bit is None) == (legacy_bit is None):
        raise RuntimeError(
            f"{name}: requires exactly one compiler bit-location spelling"
        )

    if data_bit is not None:
        member_location = member["attrs"].get("data_member_location")
        if member_location is not None:
            raise RuntimeError(
                f"{name}: DW_AT_data_bit_offset unexpectedly also has byte offset"
            )
        absolute = numeric_attr(data_bit, f"{name} data bit offset")
        spelling = f"data_bit_offset={absolute}"
    else:
        member_location = member["attrs"].get("data_member_location")
        if member_location is None:
            raise RuntimeError(
                f"{name}: legacy DW_AT_bit_offset requires byte storage offset"
            )
        byte_offset = numeric_attr(member_location, f"{name} byte offset")
        bit_offset = numeric_attr(legacy_bit, f"{name} bit offset")
        if bit_offset > 32 or bit_size > 32 - bit_offset:
            raise RuntimeError(f"{name}: legacy bit slice exceeds 32-bit storage")
        absolute = byte_offset * 8 + (32 - bit_offset - bit_size)
        spelling = (
            f"bit_offset={bit_offset} data_member_location={byte_offset}"
        )
    return absolute, spelling


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_bitfield_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_bit_fields":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_bit_fields requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError("inline_bit_fields has no compiler-produced DW_AT_location")
    expression, begin, end, _ = active_debug_loc_expression(
        path, location, probe, "inline_bit_fields"
    )
    if not re.fullmatch(r"DW_OP_fbreg: -?\d+", expression):
        raise RuntimeError(
            "inline_bit_fields active compiler location is not one exact "
            "DW_OP_fbreg operation: " + expression
        )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_bit_fields has no resolved DW_AT_type")
    structure = by_offset.get(ref_offset(type_text, "inline_bit_fields type"))
    if structure is None:
        raise RuntimeError("inline_bit_fields type references an unknown DIE")
    structure = unwrap_type(structure, by_offset, "inline_bit_fields type")
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("inline_bit_fields does not resolve to a structure")
    size = structure["attrs"].get("byte_size")
    if not size or numeric_attr(size, "inline_bit_fields byte size") != 4:
        raise RuntimeError("inline_bit_fields compiler structure is not four bytes")

    members = [
        child
        for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"inline_bit_fields requires exactly two direct members, found {len(members)}"
        )
    signed_offset, signed_spelling = require_integer_member(
        members[0], by_offset, "signed_bits", 5, 5
    )
    unsigned_offset, unsigned_spelling = require_integer_member(
        members[1], by_offset, "unsigned_bits", 7, 6
    )
    if signed_offset != 0 or unsigned_offset != 5:
        raise RuntimeError(
            "inline_bit_fields compiler bit positions changed: "
            f"signed={signed_offset} unsigned={unsigned_offset}"
        )

    print(
        "frame-zero inline bit-field DWARF oracle passed: "
        f"die=0x{variable['offset']:x} range=[0x{begin:x},0x{end:x}) "
        f"location={expression} size=4 "
        f"signed_bits:int32/5 {signed_spelling}; "
        f"unsigned_bits:uint32/6 {unsigned_spelling}"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_bitfield_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_frame_zero_inline_bitfield_probe")
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_bitfield_inner",
        "frame_zero_inline_bitfield_outer",
        "frame_zero_inline_bitfield_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected frame-zero inline bit-field chain: {chain}")
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline bit-field DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
