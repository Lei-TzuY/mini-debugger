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
    matches = re.findall(r"(?:-?0x[0-9a-fA-F]+|-?\d+)", value)
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
            if resolved_name(child, by_offset) != "caller_state":
                continue
            if child["attrs"].get("location"):
                result.append(child)
    return result


def direct_enumerators(records, enumeration):
    try:
        position = next(
            index
            for index, record in enumerate(records)
            if record["offset"] == enumeration["offset"]
        )
    except StopIteration as error:
        raise RuntimeError("CallerInlineState type DIE disappeared from parsed DWARF") from error

    enumerators = []
    for child in records[position + 1 :]:
        if child["depth"] <= enumeration["depth"]:
            break
        if (
            child["depth"] == enumeration["depth"] + 1
            and child["tag"] == "DW_TAG_enumerator"
        ):
            enumerators.append(child)
    return enumerators


def enum_representation(enumeration, by_offset):
    size_text = enumeration["attrs"].get("byte_size")
    if not size_text:
        raise RuntimeError("CallerInlineState has no compiler-described DW_AT_byte_size")
    byte_size = numeric_attr(size_text, "CallerInlineState byte size")

    encoding_text = enumeration["attrs"].get("encoding")
    if encoding_text:
        return byte_size, numeric_attr(encoding_text, "CallerInlineState encoding"), "direct"

    type_text = enumeration["attrs"].get("type")
    if not type_text:
        raise RuntimeError(
            "CallerInlineState has neither DW_AT_encoding nor compatible base DW_AT_type"
        )
    base = referenced_type(enumeration, by_offset, "CallerInlineState compatible base")
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError("CallerInlineState compatible representation is not a base type")
    base_size = base["attrs"].get("byte_size")
    base_encoding = base["attrs"].get("encoding")
    if not base_size or numeric_attr(base_size, "CallerInlineState base byte size") != byte_size:
        raise RuntimeError("CallerInlineState compatible base width disagrees with enum width")
    if not base_encoding:
        raise RuntimeError("CallerInlineState compatible base has no DW_AT_encoding")
    return byte_size, numeric_attr(base_encoding, "CallerInlineState base encoding"), "base-type"


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_enum_dwarf_oracle.py <fixture>")

    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    bindings = selected_enum_bindings(records, by_offset)
    if not bindings:
        raise RuntimeError(
            "caller_state has no compiler-produced concrete selected-inline DW_AT_location"
        )

    enum_types = []
    for binding in bindings:
        value_type = referenced_type(binding, by_offset, "caller_state type")
        if value_type["tag"] != "DW_TAG_enumeration_type":
            raise RuntimeError(
                "caller_state concrete binding does not resolve to DW_TAG_enumeration_type"
            )
        enum_types.append(value_type)

    enumeration = enum_types[0]
    if any(candidate["offset"] != enumeration["offset"] for candidate in enum_types):
        raise RuntimeError("caller_state bindings disagree on compiler enum type ownership")

    type_name = resolved_name(enumeration, by_offset)
    if type_name != "CallerInlineState":
        raise RuntimeError(f"unexpected enum type name: {type_name}")

    byte_size, encoding, representation = enum_representation(enumeration, by_offset)
    if byte_size != 4:
        raise RuntimeError(f"CallerInlineState compiler width is {byte_size}, expected 4")
    if encoding != 5:
        raise RuntimeError(
            f"CallerInlineState compiler representation is not signed integer encoding: {encoding}"
        )

    enumerators = direct_enumerators(records, enumeration)
    expected = [
        ("CALLER_INLINE_COLD", -3),
        ("CALLER_INLINE_READY", 7),
        ("CALLER_INLINE_DONE", 19),
    ]
    if len(enumerators) != len(expected):
        raise RuntimeError(
            f"CallerInlineState requires exactly {len(expected)} direct enumerators, found {len(enumerators)}"
        )
    actual = []
    for enumerator in enumerators:
        name = resolved_name(enumerator, by_offset)
        value_text = enumerator["attrs"].get("const_value")
        if name is None or value_text is None:
            raise RuntimeError("CallerInlineState enumerator lacks name or DW_AT_const_value")
        actual.append((name, signed_numeric_attr(value_text, f"{name} const value")))
    if actual != expected:
        raise RuntimeError(f"CallerInlineState enumerator table mismatch: {actual}")

    print(
        "caller_state compiler evidence: "
        f"binding-count={len(bindings)} type=DW_TAG_enumeration_type "
        f"name={type_name} byte-size={byte_size} encoding={encoding} via={representation} "
        + " enumerators="
        + ",".join(f"{name}:{value}" for name, value in actual)
    )


if __name__ == "__main__":
    main()
