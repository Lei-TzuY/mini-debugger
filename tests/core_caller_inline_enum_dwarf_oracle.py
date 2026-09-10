#!/usr/bin/env python3
import re
import sys

from core_caller_inline_dwarf_oracle import (
    numeric_attr,
    origin_name,
    parse_dies,
    ref_offset,
    referenced_type,
    resolved_name,
    run,
    unwrap_type,
)


def signed_numeric_attr(value, context):
    matches = re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", value)
    if not matches:
        raise RuntimeError(f"{context}: signed numeric attribute is unavailable: {value}")
    token = matches[-1]
    if token.startswith("-0x"):
        return -int(token[1:], 16)
    return int(token, 0)


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
            if not child["attrs"].get("location"):
                continue
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


def enum_storage(enum_type, by_offset):
    storage = enum_type
    type_text = enum_type["attrs"].get("type")
    if type_text:
        storage = by_offset.get(ref_offset(type_text, "caller_mode enum base type"))
        if storage is None:
            raise RuntimeError("caller_mode enum references an unavailable base type")
        storage = unwrap_type(storage, by_offset, "caller_mode enum base type")
        if storage["tag"] != "DW_TAG_base_type":
            raise RuntimeError("caller_mode enum compatible type is not DW_TAG_base_type")

    size_text = enum_type["attrs"].get("byte_size") or storage["attrs"].get("byte_size")
    if not size_text:
        raise RuntimeError("caller_mode enum has no compiler-described byte size")
    byte_size = numeric_attr(size_text, "caller_mode enum byte size")
    if byte_size == 0 or byte_size > 8:
        raise RuntimeError(f"caller_mode enum byte size is unsupported: {byte_size}")

    encoding_text = enum_type["attrs"].get("encoding") or storage["attrs"].get("encoding")
    if not encoding_text:
        raise RuntimeError("caller_mode enum has no compiler-described signedness")
    encoding = numeric_attr(encoding_text, "caller_mode enum encoding")
    if encoding in (5, 6):
        is_signed = True
    elif encoding in (7, 8):
        is_signed = False
    else:
        raise RuntimeError(f"caller_mode enum has unsupported scalar encoding: {encoding_text}")
    return byte_size, is_signed, encoding


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

    first = enum_types[0]
    if any(candidate["offset"] != first["offset"] for candidate in enum_types):
        raise RuntimeError("caller_mode bindings disagree on compiler enum type ownership")

    byte_size, is_signed, encoding = enum_storage(first, by_offset)
    enumerators = direct_enumerators(records, first)
    if len(enumerators) != 3:
        raise RuntimeError(
            f"caller_mode requires exactly three direct enumerators, found {len(enumerators)}"
        )

    expected = [
        ("CALLER_INLINE_COLD", -3),
        ("CALLER_INLINE_READY", 7),
        ("CALLER_INLINE_HOT", 42),
    ]
    actual = []
    for enumerator, (expected_name, expected_value) in zip(enumerators, expected):
        name = resolved_name(enumerator, by_offset)
        if name != expected_name:
            raise RuntimeError(
                f"caller_mode enumerator order/name mismatch: expected {expected_name}, got {name}"
            )
        value_text = enumerator["attrs"].get("const_value")
        if not value_text:
            raise RuntimeError(f"caller_mode enumerator {name} has no DW_AT_const_value")
        value = signed_numeric_attr(value_text, f"caller_mode enumerator {name}")
        if value != expected_value:
            raise RuntimeError(
                f"caller_mode enumerator {name} value mismatch: expected {expected_value}, got {value}"
            )
        actual.append(f"{name}={value}")

    print(
        "caller_mode compiler evidence: "
        f"binding-count={len(bindings)} type=DW_TAG_enumeration_type "
        f"byte-size={byte_size} signed={str(is_signed).lower()} encoding={encoding} "
        + " ".join(actual)
    )


if __name__ == "__main__":
    main()
