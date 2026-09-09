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
        r"^\s*<(\d+)><[0-9a-fA-F]+>:\s+Abbrev Number:\s+\d+\s+\((DW_TAG_[^)]+)\)"
    )
    attr_re = re.compile(r"^\s*<[0-9a-fA-F]+>\s+DW_AT_([^\s]+)\s*:\s*(.*)$")
    for line in text.splitlines():
        header = header_re.match(line)
        if header:
            if current is not None:
                records.append(current)
            current = {"depth": int(header.group(1)), "tag": header.group(2), "attrs": {}}
            continue
        if current is None:
            continue
        attr = attr_re.match(line)
        if attr:
            current["attrs"][attr.group(1)] = attr.group(2).strip()
    if current is not None:
        records.append(current)
    return records


def find_subprogram(records, function_name):
    for index, record in enumerate(records):
        if record["tag"] != "DW_TAG_subprogram":
            continue
        if clean_name(record["attrs"].get("name", "")) == function_name:
            return index, record
    raise RuntimeError(f"{function_name}: subprogram DIE not found")


def numeric_attr(value, context):
    matches = re.findall(r"0x([0-9a-fA-F]+)", value)
    if not matches:
        raise RuntimeError(f"{context}: numeric DWARF attribute is unavailable: {value}")
    return int(matches[-1], 16)


def subprogram_range(records, function_name):
    _, record = find_subprogram(records, function_name)
    low_text = record["attrs"].get("low_pc")
    high_text = record["attrs"].get("high_pc")
    if not low_text or not high_text:
        raise RuntimeError(f"{function_name}: low_pc/high_pc is unavailable")
    low = numeric_attr(low_text, f"{function_name}: low_pc")
    high_raw = numeric_attr(high_text, f"{function_name}: high_pc")
    high = low + high_raw if high_raw <= low else high_raw
    if high <= low:
        raise RuntimeError(f"{function_name}: invalid subprogram range")
    return low, high


def verify_historical_return_boundary(records, symbols):
    probe_name = "snapshot_caller_resume_probe"
    if probe_name not in symbols:
        raise RuntimeError(f"missing probe symbol: {probe_name}")
    resume_pc = symbols[probe_name]
    if resume_pc == 0:
        raise RuntimeError("historical resume probe unexpectedly resolved to zero")
    low, high = subprogram_range(records, "caller_with_stack_local")
    lookup_pc = resume_pc - 1
    if resume_pc != high:
        raise RuntimeError(
            "caller return-PC evidence is not on the subprogram boundary: "
            f"resume=0x{resume_pc:x} range=[0x{low:x},0x{high:x})"
        )
    if not (low <= lookup_pc < high):
        raise RuntimeError(
            "historical lookup candidate is not owned by the caller subprogram: "
            f"lookup=0x{lookup_pc:x} range=[0x{low:x},0x{high:x})"
        )
    print(
        "caller_with_stack_local: genuine return boundary "
        f"resume=0x{resume_pc:x} lookup=0x{lookup_pc:x} "
        f"range=[0x{low:x},0x{high:x})"
    )


def find_variable_location(records, function_name, variable_name):
    index, record = find_subprogram(records, function_name)
    depth = record["depth"]
    for child in records[index + 1 :]:
        if child["depth"] <= depth:
            break
        if child["tag"] != "DW_TAG_variable":
            continue
        if clean_name(child["attrs"].get("name", "")) != variable_name:
            continue
        location = child["attrs"].get("location")
        if not location:
            raise RuntimeError(
                f"{function_name}: {variable_name} has no DW_AT_location"
            )
        return location
    raise RuntimeError(f"{function_name}: {variable_name} DIE not found")


def symbol_addresses(path):
    result = {}
    for line in run("nm", "-n", path).splitlines():
        fields = line.split()
        if len(fields) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", fields[0]):
            result[fields[2]] = int(fields[0], 16)
    return result


def validate_register(reg, xmm, context):
    if reg != 17 + xmm or not (0 <= xmm < 16):
        raise RuntimeError(f"{context}: inconsistent x86-64 XMM DWARF register")


def direct_xmm(location):
    match = re.search(r"DW_OP_reg(\d+) \(xmm(\d+)\)", location)
    if not match:
        return None
    reg = int(match.group(1))
    xmm = int(match.group(2))
    validate_register(reg, xmm, location)
    return reg, xmm


def list_xmm(location, loc_text, probe):
    offset_match = re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)", location)
    if not offset_match:
        raise RuntimeError(f"unsupported DW_AT_location representation: {location}")
    wanted = int(offset_match.group(1), 16)
    lines = loc_text.splitlines()
    start = None
    offset_re = re.compile(r"^\s*([0-9a-fA-F]{8})\b")
    for index, line in enumerate(lines):
        match = offset_re.match(line)
        if match and int(match.group(1), 16) == wanted:
            start = index
            break
    if start is None:
        raise RuntimeError(f"location-list offset 0x{wanted:x} was not emitted by readelf")

    range_re = re.compile(
        r"([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16})\s+"
        r"\(DW_OP_reg(\d+) \(xmm(\d+)\)\)"
    )
    for line in lines[start:]:
        if "<End of list>" in line:
            break
        match = range_re.search(line)
        if not match:
            continue
        begin = int(match.group(1), 16)
        end = int(match.group(2), 16)
        reg = int(match.group(3))
        xmm = int(match.group(4))
        if begin <= probe < end:
            validate_register(reg, xmm, f"probe 0x{probe:x}")
            return reg, xmm, begin, end
    raise RuntimeError(f"probe 0x{probe:x} is not covered by an XMM DWARF location")


def direct_fbreg(location):
    match = re.search(r"DW_OP_fbreg:\s*(-?\d+)", location)
    if not match:
        return None
    return int(match.group(1))


def list_fbreg(location, loc_text, probe):
    offset_match = re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)", location)
    if not offset_match:
        raise RuntimeError(f"unsupported stack-local DW_AT_location: {location}")
    wanted = int(offset_match.group(1), 16)
    lines = loc_text.splitlines()
    start = None
    offset_re = re.compile(r"^\s*([0-9a-fA-F]{8})\b")
    for index, line in enumerate(lines):
        match = offset_re.match(line)
        if match and int(match.group(1), 16) == wanted:
            start = index
            break
    if start is None:
        raise RuntimeError(f"stack-local location-list offset 0x{wanted:x} was not emitted")

    range_re = re.compile(
        r"([0-9a-fA-F]{8,16})\s+([0-9a-fA-F]{8,16}).*"
        r"\(DW_OP_fbreg:\s*(-?\d+)\)"
    )
    for line in lines[start:]:
        if "<End of list>" in line:
            break
        match = range_re.search(line)
        if not match:
            continue
        begin = int(match.group(1), 16)
        end = int(match.group(2), 16)
        offset = int(match.group(3))
        if begin <= probe < end:
            return offset, begin, end
    raise RuntimeError(f"probe 0x{probe:x} is not covered by a DW_OP_fbreg location")


def frame_base_kind(records, function_name):
    _, record = find_subprogram(records, function_name)
    frame_base = record["attrs"].get("frame_base")
    if not frame_base:
        raise RuntimeError(f"{function_name}: no DW_AT_frame_base")
    if "DW_OP_call_frame_cfa" in frame_base:
        return "call_frame_cfa"
    if re.search(r"DW_OP_reg7\s*\(rsp\)", frame_base):
        return "rsp"
    raise RuntimeError(f"{function_name}: unsupported frame base: {frame_base}")


def verify(function_name, probe_name, loc_text, records, symbols):
    if probe_name not in symbols:
        raise RuntimeError(f"missing probe symbol: {probe_name}")
    probe = symbols[probe_name]
    location = find_variable_location(records, function_name, "xmm_value")
    direct = direct_xmm(location)
    if direct is not None:
        reg, xmm = direct
        print(f"{function_name}: probe=0x{probe:x} direct DW_OP_reg{reg} (xmm{xmm})")
        return
    reg, xmm, begin, end = list_xmm(location, loc_text, probe)
    print(
        f"{function_name}: probe=0x{probe:x} range=[0x{begin:x},0x{end:x}) "
        f"DW_OP_reg{reg} (xmm{xmm})"
    )


def verify_stack_local(
    function_name, variable_name, probe_name, loc_text, records, symbols, probe_adjust=0
):
    if probe_name not in symbols:
        raise RuntimeError(f"missing probe symbol: {probe_name}")
    raw_probe = symbols[probe_name]
    probe = raw_probe + probe_adjust
    if probe < 0:
        raise RuntimeError(f"{probe_name}: adjusted probe underflow")
    location = find_variable_location(records, function_name, variable_name)
    base = frame_base_kind(records, function_name)
    direct = direct_fbreg(location)
    if direct is not None:
        print(
            f"{function_name}: {variable_name} probe=0x{probe:x} direct "
            f"DW_OP_fbreg {direct} frame_base={base}"
        )
        return
    offset, begin, end = list_fbreg(location, loc_text, probe)
    print(
        f"{function_name}: {variable_name} probe=0x{probe:x} "
        f"range=[0x{begin:x},0x{end:x}) DW_OP_fbreg {offset} frame_base={base}"
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_xmm_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_dies(info)
    symbols = symbol_addresses(path)
    verify("crash_with_xmm", "snapshot_xmm_crash_probe", loc, records, symbols)
    verify_stack_local(
        "crash_with_xmm", "stack_local", "snapshot_xmm_crash_probe", loc, records, symbols
    )
    verify_historical_return_boundary(records, symbols)
    verify_stack_local(
        "caller_with_stack_local",
        "caller_stack_local",
        "snapshot_caller_resume_probe",
        loc,
        records,
        symbols,
        probe_adjust=-1,
    )
    verify("sibling_hold_xmm", "snapshot_xmm_sibling_probe", loc, records, symbols)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"XMM DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
