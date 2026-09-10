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
            if child["attrs"].get("location"):
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
    encoding = enum_type["attrs"].get("encoding")
    if not encoding or numeric_attr(encoding, "caller_mode encoding") != 5:
        raise RuntimeError("caller_mode compiler enum type is not signed integer encoded")

    enumerators = direct_enumerators(records, enum_type)
    expected = {
        "CallerInlineIdle": -3,
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
        f"binding-count={len(bindings)} type=DW_TAG_enumeration_type byte-size=4 signed "
        "CallerInlineIdle=-3 CallerInlineReady=7 CallerInlineBusy=42"
    )


if __name__ == "__main__":
    main()
