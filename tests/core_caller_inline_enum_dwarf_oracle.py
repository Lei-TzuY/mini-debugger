#!/usr/bin/env python3
import re
import sys

from core_caller_inline_dwarf_oracle import (
    numeric_attr,
    origin_name,
    parse_dies,
    referenced_type,
    resolved_name,
    run,
)


def signed_numeric_attr(value, context):
    matches = re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", value)
    if not matches:
        raise RuntimeError(f"{context}: numeric attribute is unavailable: {value}")
    return int(matches[-1], 0)


def selected_enum_bindings(records, by_offset):
    result = []
    for pos, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "caller_inline_inner":
            continue
        for child in records[pos + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) != "caller_mode":
                continue
            location = child["attrs"].get("location")
            if location:
                result.append(child)
    return result


def direct_enumerators(records, enum_type):
    try:
        position = next(
            index
            for index, record in enumerate(records)
            if record["offset"] == enum_type["offset"]
        )
    except StopIteration as error:
        raise RuntimeError("caller_mode enum DIE disappeared from parsed DWARF") from error

    result = []
    for child in records[position + 1 :]:
        if child["depth"] <= enum_type["depth"]:
            break
        if child["depth"] == enum_type["depth"] + 1 and child["tag"] == "DW_TAG_enumerator":
            result.append(child)
    return result


def validate_integral_representation(enum_type, by_offset):
    encoding = enum_type["attrs"].get("encoding")
    if encoding:
        code = numeric_attr(encoding, "caller_mode encoding")
        if code not in {5, 7}:
            raise RuntimeError("caller_mode direct compiler encoding is not signed/unsigned integer")
        return ("direct-encoding", code == 5)

    underlying = referenced_type(enum_type, by_offset, "caller_mode underlying type")
    if underlying["tag"] != "DW_TAG_base_type":
        raise RuntimeError("caller_mode compiler representation is neither encoded nor base-typed")
    size = underlying["attrs"].get("byte_size")
    underlying_encoding = underlying["attrs"].get("encoding")
    if not size or numeric_attr(size, "caller_mode underlying byte size") != 4:
        raise RuntimeError("caller_mode underlying compiler type is not four bytes")
    if not underlying_encoding:
        raise RuntimeError("caller_mode underlying compiler type has no integer encoding")
    code = numeric_attr(underlying_encoding, "caller_mode underlying encoding")
    if code not in {5, 7}:
        raise RuntimeError("caller_mode underlying compiler type is not signed/unsigned integer")
    return ("underlying-type", code == 5)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_enum_dwarf_oracle.py <fixture>")

    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    bindings = selected_enum_bindings(records, by_offset)
    if not bindings:
        raise RuntimeError(
            "caller_mode has no compiler-produced concrete selected-inline DW_AT_location"
        )
    for binding in bindings:
        location = binding["attrs"].get("location", "")
        if "DW_OP_fbreg" not in location:
            raise RuntimeError(
                "caller_mode does not use the already-supported caller-frame DW_OP_fbreg location: "
                + location
            )

    enum_types = []
    for binding in bindings:
        value_type = referenced_type(binding, by_offset, "caller_mode type")
        if value_type["tag"] != "DW_TAG_enumeration_type":
            raise RuntimeError(
                "caller_mode concrete binding does not resolve to DW_TAG_enumeration_type"
            )
        enum_types.append(value_type)

    enum_type = enum_types[0]
    if any(candidate["offset"] != enum_type["offset"] for candidate in enum_types):
        raise RuntimeError("caller_mode bindings disagree on compiler enum type ownership")

    size = enum_type["attrs"].get("byte_size")
    if not size or numeric_attr(size, "caller_mode byte size") != 4:
        raise RuntimeError("caller_mode compiler enum type is not exactly four bytes")
    representation, is_signed = validate_integral_representation(enum_type, by_offset)

    enumerators = direct_enumerators(records, enum_type)
    expected = {
        "CallerInlineIdle": 3,
        "CallerInlineReady": 7,
        "CallerInlineBusy": 42,
    }
    actual = {}
    for enumerator in enumerators:
        name = resolved_name(enumerator, by_offset)
        value = enumerator["attrs"].get("const_value")
        if not name or value is None:
            raise RuntimeError("caller_mode enumerator lacks compiler name/const_value")
        actual[name] = signed_numeric_attr(value, f"{name} const_value")
    if actual != expected:
        raise RuntimeError(f"caller_mode compiler enumerator table mismatch: {actual}")

    print(
        "caller_mode compiler evidence: "
        f"binding-count={len(bindings)} location=DW_OP_fbreg "
        "type=DW_TAG_enumeration_type byte-size=4 "
        f"{'signed' if is_signed else 'unsigned'} representation={representation} "
        "CallerInlineIdle=3 CallerInlineReady=7 CallerInlineBusy=42"
    )


if __name__ == "__main__":
    main()
