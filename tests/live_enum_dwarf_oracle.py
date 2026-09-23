#!/usr/bin/env python3
import re
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    clean_name,
    direct_children,
    find_variable,
    parse_records,
    run,
    scalar_number,
    symbol_addresses,
    type_reference,
    unwrap_type,
)
from core_physical_enum_dwarf_oracle import integral_representation


def active_location(path, location, probe):
    direct = re.search(r"DW_OP_reg0 \(rax\)", location)
    if direct:
        return "DW_OP_reg5 (rdi)", None

    match = re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)", location)
    if not match:
        raise RuntimeError(
            "live_mode has unsupported compiler location representation: " + location
        )
    wanted = int(match.group(1), 16)
    loc = run("readelf", "--debug-dump=loc", path)
    active = False
    for line in loc.splitlines():
        offset = re.match(r"^\s*([0-9a-fA-F]{8})\b", line)
        if not active and offset and int(offset.group(1), 16) == wanted:
            active = True
        if not active:
            continue
        if "<End of list>" in line:
            break
        if "(DW_OP_" not in line:
            continue
        prefix, expression = line.split("(", 1)
        values = re.findall(r"\b[0-9a-fA-F]{8,16}\b", prefix)
        if len(values) < 2:
            continue
        begin = int(values[-2], 16)
        end = int(values[-1], 16)
        expression = expression.rsplit(")", 1)[0]
        if begin <= probe < end:
            return expression, (begin, end)
    raise RuntimeError(
        f"live_mode location list does not own probe 0x{probe:x}"
    )


def verify(path):
    info = run("readelf", "--debug-dump=info", path)
    records = parse_records(info)
    symbols = symbol_addresses(path)
    if "live_enum_probe" not in symbols:
        raise RuntimeError("live_enum_probe symbol is missing")
    probe = symbols["live_enum_probe"]

    variable = find_variable(records, "inspect_live_enum", "live_mode")
    type_attr = variable["attrs"].get("type")
    location = variable["attrs"].get("location")
    if not type_attr or not location:
        raise RuntimeError("live_mode is missing compiler type/location evidence")

    enum_type = unwrap_type(
        records, type_reference(type_attr, "live_mode"), "live_mode"
    )
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError("live_mode is not a compiler enumeration type")
    if clean_name(enum_type["attrs"].get("name", "")) != "LiveMode":
        raise RuntimeError("live_mode compiler enum name changed")
    size = enum_type["attrs"].get("byte_size")
    if not size or scalar_number(size, "live_mode enum byte size") != 4:
        raise RuntimeError("live_mode enum is not exactly four bytes")
    representation, is_signed = integral_representation(records, enum_type)
    if is_signed:
        raise RuntimeError("live_mode compiler enum representation is unexpectedly signed")

    expected = {"LiveIdle": 3, "LiveReady": 7, "LiveBusy": 42}
    actual = {}
    for child in direct_children(records, enum_type):
        if child["tag"] != "DW_TAG_enumerator":
            continue
        name = clean_name(child["attrs"].get("name", ""))
        value = child["attrs"].get("const_value")
        if not name or value is None:
            raise RuntimeError("live_mode enumerator lost compiler name/value")
        actual[name] = scalar_number(value, f"{name} const_value")
    if actual != expected:
        raise RuntimeError(f"live_mode enumerator table changed: {actual}")

    expression, owned_range = active_location(path, location, probe)
    if expression != "DW_OP_reg5 (rdi)":
        raise RuntimeError(
            "live_mode active compiler location is not the compiler-proven "
            "DW_OP_reg5 (rdi) form: " + expression
        )

    range_text = (
        "direct"
        if owned_range is None
        else f"[0x{owned_range[0]:x},0x{owned_range[1]:x})"
    )
    print(
        "live enum DWARF oracle passed: "
        f"probe=0x{probe:x} location={expression} range={range_text} "
        f"type=LiveMode byte-size=4 unsigned representation={representation} "
        "LiveIdle=3 LiveReady=7 LiveBusy=42"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: live_enum_dwarf_oracle.py <fixture>")
    verify(sys.argv[1])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"live enum DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
