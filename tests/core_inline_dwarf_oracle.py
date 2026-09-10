#!/usr/bin/env python3
import re
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def clean_name(value):
    marker = "): "
    if marker in value:
        return value.rsplit(marker, 1)[1].strip()
    return value.strip()


def parse_dies(text):
    records = []
    current = None
    header_re = re.compile(
        r"^\s*<(\d+)><([0-9a-fA-F]+)>:\s+Abbrev Number:\s+\d+\s+\((DW_TAG_[^)]+)\)"
    )
    attr_re = re.compile(r"^\s*<[0-9a-fA-F]+>\s+DW_AT_([^\s]+)\s*:\s*(.*)$")
    for line in text.splitlines():
        header = header_re.match(line)
        if header:
            if current is not None:
                records.append(current)
            current = {
                "depth": int(header.group(1)),
                "offset": int(header.group(2), 16),
                "tag": header.group(3),
                "attrs": {},
            }
            continue
        if current is None:
            continue
        attr = attr_re.match(line)
        if attr:
            current["attrs"][attr.group(1)] = attr.group(2).strip()
    if current is not None:
        records.append(current)
    return records


def numeric_attr(value, context):
    matches = re.findall(r"0x([0-9a-fA-F]+)", value)
    if not matches:
        matches = re.findall(r"\b(\d+)\b", value)
    if not matches:
        raise RuntimeError(f"{context}: numeric attribute is unavailable: {value}")
    token = matches[-1]
    return int(token, 16 if "0x" in value else 10)


def ref_offset(value, context):
    match = re.search(r"<0x([0-9a-fA-F]+)>", value)
    if not match:
        raise RuntimeError(f"{context}: DIE reference is unavailable: {value}")
    return int(match.group(1), 16)


def symbol_address(path, name):
    for line in run("nm", "-n", path).splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[2] == name:
            return int(fields[0], 16)
    raise RuntimeError(f"missing probe symbol: {name}")


def die_range(record, context):
    low_text = record["attrs"].get("low_pc")
    high_text = record["attrs"].get("high_pc")
    if not low_text or not high_text:
        raise RuntimeError(f"{context}: low_pc/high_pc is unavailable")
    low = numeric_attr(low_text, f"{context}: low_pc")
    raw_high = numeric_attr(high_text, f"{context}: high_pc")
    high = raw_high if raw_high > low else low + raw_high
    if high <= low:
        raise RuntimeError(f"{context}: invalid PC range")
    return low, high


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_inline_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    probe = symbol_address(path, "snapshot_inline_crash_probe")

    covered = []
    for record in records:
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        low, high = die_range(record, "inline DIE")
        if not (low <= probe < high):
            continue
        origin_text = record["attrs"].get("abstract_origin")
        call_file = record["attrs"].get("call_file")
        call_line = record["attrs"].get("call_line")
        if not origin_text or not call_file or not call_line:
            raise RuntimeError("covering inline DIE lacks abstract-origin/call-site metadata")
        origin_offset = ref_offset(origin_text, "inline abstract_origin")
        origin = by_offset.get(origin_offset)
        if origin is None or origin["tag"] != "DW_TAG_subprogram":
            raise RuntimeError("inline abstract origin does not reference a subprogram")
        name = clean_name(origin["attrs"].get("name", ""))
        if not name:
            raise RuntimeError("inline abstract-origin subprogram has no name")
        if numeric_attr(call_file, f"{name}: call_file") != 1:
            raise RuntimeError(f"{name}: bounded fixture expected call_file 1")
        line = numeric_attr(call_line, f"{name}: call_line")
        if line == 0:
            raise RuntimeError(f"{name}: zero call_line")
        covered.append((record["depth"], name, low, high, line))

    covered.sort()
    names = [entry[1] for entry in covered]
    if names != ["inline_outer", "inline_inner"]:
        raise RuntimeError(f"unexpected covering inline chain: {names}")
    if covered[1][0] != covered[0][0] + 1:
        raise RuntimeError("inline_inner is not nested directly inside inline_outer")
    if not (covered[0][2] <= covered[1][2] and covered[1][3] <= covered[0][3]):
        raise RuntimeError("inner inline range is not contained by outer inline range")

    for depth, name, low, high, line in covered:
        print(
            f"{name}: depth={depth} probe=0x{probe:x} "
            f"range=[0x{low:x},0x{high:x}) call_file=1 call_line={line}"
        )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"inline DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
