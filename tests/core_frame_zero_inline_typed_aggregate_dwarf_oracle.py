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


def exact_active_location(path, location, probe, context):
    direct = re.search(r"\((DW_OP_[^)]+(?:\([^)]*\))?(?:;\s*DW_OP_piece:\s*\d+)?)\)", location)
    if direct and "location list" not in location.lower():
        return re.sub(r"\s+", " ", direct.group(1)).strip(), "direct-exprloc"

    expression, begin, end, _ = active_debug_loc_expression(
        path, location, probe, context
    )
    return re.sub(r"\s+", " ", expression).strip(), f"loclist=[0x{begin:x},0x{end:x})"


def member_offset(member, context):
    location = member["attrs"].get("data_member_location")
    if location is None:
        raise RuntimeError(f"{context}: missing DW_AT_data_member_location")
    return numeric_attr(location, f"{context} offset")


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_typed_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_typed":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_typed requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError("inline_typed has no compiler-produced DW_AT_location")
    expression, ownership = exact_active_location(
        path, location, probe, "inline_typed"
    )

    allowed = (
        re.fullmatch(r"DW_OP_fbreg: -?\d+", expression)
        or re.fullmatch(r"DW_OP_reg[12] \((?:rdx|rcx)\)", expression)
        or re.fullmatch(
            r"DW_OP_reg[12] \((?:rdx|rcx)\); DW_OP_piece: 8",
            expression,
        )
    )
    if not allowed:
        raise RuntimeError(
            "inline_typed active compiler ownership is outside the bounded "
            f"frame-zero candidate set: {expression}"
        )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_typed has no resolved DW_AT_type")
    structure = by_offset.get(ref_offset(type_text, "inline_typed type"))
    if structure is None:
        raise RuntimeError("inline_typed type references an unknown DIE")
    structure = unwrap_type(structure, by_offset, "inline_typed type")
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("inline_typed does not resolve to a structure")
    if clean_name(structure["attrs"].get("name", "")) != "FrameZeroInlineTyped":
        raise RuntimeError("inline_typed structure name changed")
    size = structure["attrs"].get("byte_size")
    if not size or numeric_attr(size, "inline_typed byte size") != 8:
        raise RuntimeError("inline_typed structure is not exactly eight bytes")

    members = [
        child
        for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 1 or clean_name(
        members[0]["attrs"].get("name", "")
    ) != "linked":
        raise RuntimeError("inline_typed linked member changed")
    linked = members[0]
    if member_offset(linked, "inline_typed linked") != 0:
        raise RuntimeError("inline_typed linked is not at offset zero")

    member_type_text = linked["attrs"].get("type")
    if not member_type_text:
        raise RuntimeError("inline_typed linked has no DW_AT_type")
    pointer = by_offset.get(ref_offset(member_type_text, "inline_typed linked type"))
    if pointer is None:
        raise RuntimeError("inline_typed linked type references an unknown DIE")
    pointer = unwrap_type(pointer, by_offset, "inline_typed linked type")
    if pointer["tag"] != "DW_TAG_pointer_type":
        raise RuntimeError("inline_typed linked is not a compiler pointer type")
    pointer_size = pointer["attrs"].get("byte_size")
    if pointer_size and numeric_attr(pointer_size, "inline_typed pointer byte size") != 8:
        raise RuntimeError("inline_typed pointer is not x86-64 width")

    pointee_text = pointer["attrs"].get("type")
    if not pointee_text:
        raise RuntimeError("inline_typed pointer has no pointee type")
    pointee = by_offset.get(ref_offset(pointee_text, "inline_typed pointee"))
    if pointee is None:
        raise RuntimeError("inline_typed pointee references an unknown DIE")
    pointee = unwrap_type(pointee, by_offset, "inline_typed pointee")
    if pointee["tag"] != "DW_TAG_base_type":
        raise RuntimeError("inline_typed pointee is not a base type")
    pointee_size = pointee["attrs"].get("byte_size")
    pointee_encoding = pointee["attrs"].get("encoding")
    if not pointee_size or numeric_attr(
        pointee_size, "inline_typed pointee byte size"
    ) != 4:
        raise RuntimeError("inline_typed pointee is not exactly four bytes")
    if not pointee_encoding or numeric_attr(
        pointee_encoding, "inline_typed pointee encoding"
    ) != 5:
        raise RuntimeError("inline_typed pointee is not a signed integer")

    payload = symbol_address(path, "frame_zero_inline_typed_payload")
    print(
        "frame-zero inline typed aggregate DWARF oracle passed: "
        f"die=0x{variable['offset']:x} probe=0x{probe:x} "
        f"ownership={ownership} location={expression} "
        f"structure=FrameZeroInlineTyped:8 linked@0=pointer-to-signed-int32 "
        f"payload-symbol=0x{payload:x}"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_typed_aggregate_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_frame_zero_inline_typed_probe")
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_typed_inner",
        "frame_zero_inline_typed_outer",
        "frame_zero_inline_typed_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected frame-zero inline typed chain: {chain}")
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline typed aggregate DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
