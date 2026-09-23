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
    type_reference,
    unwrap_type,
)
from core_physical_enum_dwarf_oracle import integral_representation


def call_return_pc(path):
    text = run("objdump", "-d", "--disassemble=historical_enum_caller", path)
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if "call" not in line or "<historical_enum_callee>" not in line:
            continue
        for following in lines[index + 1 :]:
            match = re.match(r"^\s*([0-9a-fA-F]+):", following)
            if match:
                return int(match.group(1), 16)
    raise RuntimeError(
        "historical_enum_caller has no decodable call to historical_enum_callee"
    )


def active_location(path, location, pc):
    if "(DW_OP_" in location and "location list" not in location:
        return location.split("(", 1)[1].rsplit(")", 1)[0], None

    match = re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)", location)
    if not match:
        raise RuntimeError(
            "historical_mode has unsupported compiler location representation: "
            + location
        )
    wanted = int(match.group(1), 16)
    loc = run("readelf", "--debug-dump=loc", path)
    active = False
    for line in loc.splitlines():
        offset = re.match(r"^\s*([0-9a-fA-F]{8,16})\b", line)
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
        if begin <= pc < end:
            return expression, (begin, end)
    raise RuntimeError(
        f"historical_mode location list does not own caller return PC 0x{pc:x}"
    )


def verify(path):
    info = run("readelf", "--debug-dump=info", path)
    records = parse_records(info)
    variable = find_variable(
        records, "historical_enum_caller", "historical_mode"
    )
    type_attr = variable["attrs"].get("type")
    location = variable["attrs"].get("location")
    if not type_attr or not location:
        raise RuntimeError(
            "historical_mode is missing compiler type/location evidence"
        )

    enum_type = unwrap_type(
        records, type_reference(type_attr, "historical_mode"), "historical_mode"
    )
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError("historical_mode is not a compiler enum")
    if clean_name(enum_type["attrs"].get("name", "")) != "HistoricalLiveMode":
        raise RuntimeError("historical enum compiler name changed")
    size = enum_type["attrs"].get("byte_size")
    if not size or scalar_number(size, "historical enum byte size") != 4:
        raise RuntimeError("historical enum is not exactly four bytes")
    representation, is_signed = integral_representation(records, enum_type)
    if is_signed:
        raise RuntimeError("historical enum representation is unexpectedly signed")

    expected = {
        "HistoricalIdle": 3,
        "HistoricalReady": 7,
        "HistoricalBusy": 42,
    }
    actual = {}
    for child in direct_children(records, enum_type):
        if child["tag"] != "DW_TAG_enumerator":
            continue
        name = clean_name(child["attrs"].get("name", ""))
        value = child["attrs"].get("const_value")
        if not name or value is None:
            raise RuntimeError("historical enum enumerator lost name/value")
        actual[name] = scalar_number(value, f"{name} const_value")
    if actual != expected:
        raise RuntimeError(f"historical enum table changed: {actual}")

    return_pc = call_return_pc(path)
    expression, owned_range = active_location(path, location, return_pc)
    supported = (
        expression.startswith("DW_OP_fbreg")
        or expression.startswith("DW_OP_breg3 (rbx)")
    )
    if not supported:
        raise RuntimeError(
            "historical enum requires a new historical machine-state form: "
            + expression
        )
    range_text = (
        "direct"
        if owned_range is None
        else f"[0x{owned_range[0]:x},0x{owned_range[1]:x})"
    )
    print(
        "historical live enum DWARF oracle passed: "
        f"return-pc=0x{return_pc:x} location={expression} range={range_text} "
        f"type=HistoricalLiveMode byte-size=4 unsigned "
        f"representation={representation}"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: live_historical_enum_dwarf_oracle.py <fixture>"
        )
    verify(sys.argv[1])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(
            f"historical live enum DWARF oracle failure: {error}",
            file=sys.stderr,
        )
        raise SystemExit(1)
