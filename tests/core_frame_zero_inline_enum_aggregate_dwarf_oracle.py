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
    direct = re.search(
        r"\((DW_OP_[^)]+(?:\([^)]*\))?(?:;\s*DW_OP_piece:\s*\d+)?)\)",
        location,
    )
    if direct and "location list" not in location.lower():
        return re.sub(r"\s+", " ", direct.group(1)).strip(), "direct-exprloc"
    expression, begin, end, _ = active_debug_loc_expression(
        path, location, probe, context
    )
    return (
        re.sub(r"\s+", " ", expression).strip(),
        f"loclist=[0x{begin:x},0x{end:x})",
    )


def member_offset(member, context):
    location = member["attrs"].get("data_member_location")
    if location is None:
        raise RuntimeError(f"{context}: missing DW_AT_data_member_location")
    return numeric_attr(location, f"{context} offset")


def enum_representation(enum_type, by_offset):
    size = enum_type["attrs"].get("byte_size")
    if not size or numeric_attr(size, "enum byte size") != 4:
        raise RuntimeError("frame-zero enum type is not exactly four bytes")

    encoding = enum_type["attrs"].get("encoding")
    if encoding is not None:
        code = numeric_attr(encoding, "enum direct encoding")
        if code not in {5, 7}:
            raise RuntimeError("frame-zero enum direct encoding is unsupported")
        return code == 5

    underlying_text = enum_type["attrs"].get("type")
    if not underlying_text:
        raise RuntimeError("frame-zero enum has no compiler integer representation")
    underlying = by_offset.get(ref_offset(underlying_text, "enum underlying type"))
    if underlying is None:
        raise RuntimeError("frame-zero enum underlying type is unknown")
    underlying = unwrap_type(underlying, by_offset, "enum underlying type")
    if underlying["tag"] != "DW_TAG_base_type":
        raise RuntimeError("frame-zero enum underlying type is not a base type")
    base_size = underlying["attrs"].get("byte_size")
    base_encoding = underlying["attrs"].get("encoding")
    if not base_size or numeric_attr(base_size, "enum underlying size") != 4:
        raise RuntimeError("frame-zero enum underlying type is not four bytes")
    if not base_encoding:
        raise RuntimeError("frame-zero enum underlying type has no encoding")
    code = numeric_attr(base_encoding, "enum underlying encoding")
    if code not in {5, 7}:
        raise RuntimeError("frame-zero enum underlying encoding is unsupported")
    return code == 5


def require_binding(path, probe, records, by_offset):
    candidates = []
    for position, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "frame_zero_inline_enum_aggregate_inner":
            continue
        for child in records[position + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) == "inline_enum_aggregate":
                candidates.append(child)

    if len(candidates) != 1:
        raise RuntimeError(
            "inline_enum_aggregate requires exactly one concrete active binding, "
            f"found {len(candidates)}"
        )
    variable = candidates[0]
    location = variable["attrs"].get("location")
    if not location:
        raise RuntimeError("inline_enum_aggregate has no compiler-produced DW_AT_location")
    expression, ownership = exact_active_location(
        path, location, probe, "inline_enum_aggregate"
    )
    allowed = (
        re.fullmatch(r"DW_OP_fbreg: -?\d+", expression)
        or re.fullmatch(
            r"DW_OP_reg[12] \((?:rdx|rcx)\); DW_OP_piece: 4; "
            r"DW_OP_reg[12] \((?:rdx|rcx)\); DW_OP_piece: 4",
            expression,
        )
    )
    if not allowed:
        raise RuntimeError(
            "inline_enum_aggregate active compiler ownership is outside the bounded "
            f"frame-zero candidate set: {expression}"
        )

    type_text = resolved_attr(variable, by_offset, "type")
    if not type_text:
        raise RuntimeError("inline_enum_aggregate has no resolved DW_AT_type")
    structure = by_offset.get(ref_offset(type_text, "inline_enum_aggregate type"))
    if structure is None:
        raise RuntimeError("inline_enum_aggregate type references an unknown DIE")
    structure = unwrap_type(structure, by_offset, "inline_enum_aggregate type")
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("inline_enum_aggregate does not resolve to a structure")
    if clean_name(structure["attrs"].get("name", "")) != "FrameZeroInlineEnumAggregate":
        raise RuntimeError("inline_enum_aggregate structure name changed")
    size = structure["attrs"].get("byte_size")
    if not size or numeric_attr(size, "inline_enum_aggregate byte size") != 8:
        raise RuntimeError("inline_enum_aggregate structure is not exactly eight bytes")

    members = [
        child for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError("inline_enum_aggregate requires exactly two direct members")
    if clean_name(members[0]["attrs"].get("name", "")) != "direct":
        raise RuntimeError("inline_enum_aggregate direct member changed")
    if member_offset(members[0], "inline_enum_aggregate direct") != 0:
        raise RuntimeError("inline_enum_aggregate direct is not at offset zero")
    direct_type_text = members[0]["attrs"].get("type")
    if not direct_type_text:
        raise RuntimeError("inline_enum_aggregate direct has no type")
    direct_type = by_offset.get(ref_offset(direct_type_text, "direct member type"))
    if direct_type is None:
        raise RuntimeError("inline_enum_aggregate direct type is unknown")
    direct_type = unwrap_type(direct_type, by_offset, "direct member type")
    if direct_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError("inline_enum_aggregate direct is not a base type")
    if numeric_attr(direct_type["attrs"].get("byte_size", ""), "direct byte size") != 4:
        raise RuntimeError("inline_enum_aggregate direct is not four bytes")
    if numeric_attr(direct_type["attrs"].get("encoding", ""), "direct encoding") != 5:
        raise RuntimeError("inline_enum_aggregate direct is not signed int32")

    mode = members[1]
    if clean_name(mode["attrs"].get("name", "")) != "mode":
        raise RuntimeError("inline_enum_aggregate mode member changed")
    if member_offset(mode, "inline_enum_aggregate mode") != 4:
        raise RuntimeError("inline_enum_aggregate mode is not at offset four")
    mode_type_text = mode["attrs"].get("type")
    if not mode_type_text:
        raise RuntimeError("inline_enum_aggregate mode has no type")
    enum_type = by_offset.get(ref_offset(mode_type_text, "mode member type"))
    if enum_type is None:
        raise RuntimeError("inline_enum_aggregate mode type is unknown")
    enum_type = unwrap_type(enum_type, by_offset, "mode member type")
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError("inline_enum_aggregate mode is not a compiler enum")
    if clean_name(enum_type["attrs"].get("name", "")) != "FrameZeroInlineMode":
        raise RuntimeError("frame-zero enum type name changed")
    is_signed = enum_representation(enum_type, by_offset)
    if is_signed:
        raise RuntimeError("frame-zero enum representation is unexpectedly signed")

    enumerators = [
        child for child in direct_children(records, enum_type)
        if child["tag"] == "DW_TAG_enumerator"
    ]
    actual = {}
    for entry in enumerators:
        name = clean_name(entry["attrs"].get("name", ""))
        raw = entry["attrs"].get("const_value")
        if not name or raw is None:
            raise RuntimeError("frame-zero enum enumerator lost compiler name/value")
        actual[name] = numeric_attr(raw, f"{name} value")
    expected = {
        "FrameZeroInlineIdle": 3,
        "FrameZeroInlineReady": 7,
        "FrameZeroInlineBusy": 42,
    }
    if actual != expected:
        raise RuntimeError(f"frame-zero enum table changed: {actual}")

    print(
        "frame-zero inline enum aggregate DWARF oracle passed: "
        f"die=0x{variable['offset']:x} probe=0x{probe:x} "
        f"ownership={ownership} location={expression} "
        "structure=FrameZeroInlineEnumAggregate:8 "
        "direct@0=signed-int32 mode@4=FrameZeroInlineMode:uint32 "
        "FrameZeroInlineIdle=3 FrameZeroInlineReady=7 FrameZeroInlineBusy=42"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_frame_zero_inline_enum_aggregate_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_frame_zero_inline_enum_aggregate_probe")
    contexts = addr2line_contexts(path, probe)
    wanted = [
        "frame_zero_inline_enum_aggregate_inner",
        "frame_zero_inline_enum_aggregate_outer",
        "frame_zero_inline_enum_aggregate_physical",
    ]
    chain = [name for name, _ in contexts]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected frame-zero inline enum aggregate chain: {chain}")
    require_binding(path, probe, records, by_offset)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"frame-zero inline enum aggregate DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
