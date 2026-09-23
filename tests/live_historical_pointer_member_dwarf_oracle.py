#!/usr/bin/env python3
import re
import subprocess
import sys
from core_physical_aggregate_dwarf_oracle import (
    clean_name, direct_children, parse_records, run,
    scalar_number, type_reference, unwrap_type,
)

def find_parameter(records):
    funcs=[(i,r) for i,r in enumerate(records)
           if r["tag"]=="DW_TAG_subprogram"
           and clean_name(r["attrs"].get("name",""))=="historical_linked_caller"]
    if len(funcs)!=1:
        raise RuntimeError(f"expected one historical_linked_caller DIE, found {len(funcs)}")
    index,function=funcs[0]
    for record in records[index+1:]:
        if record["depth"]<=function["depth"]:
            break
        if record["tag"]=="DW_TAG_formal_parameter" and clean_name(
                record["attrs"].get("name",""))=="historical_member_pointer":
            return record
    raise RuntimeError("historical_member_pointer formal parameter not found")

def call_return_pc(path):
    text=run("objdump","-d","--disassemble=historical_linked_caller",path)
    lines=text.splitlines()
    for index,line in enumerate(lines):
        if "call" not in line or "<historical_linked_callee>" not in line:
            continue
        for following in lines[index+1:]:
            match=re.match(r"^\s*([0-9a-fA-F]+):",following)
            if match:
                return int(match.group(1),16)
    raise RuntimeError("historical_linked_caller has no decodable callee call")

def active_location(path,location,pc):
    if "(DW_OP_" in location and "location list" not in location:
        return location.split("(",1)[1].rsplit(")",1)[0],None
    match=re.search(r"0x([0-9a-fA-F]+)\s+\(location list\)",location)
    if not match:
        raise RuntimeError("unsupported historical linked pointer location: "+location)
    wanted=int(match.group(1),16)
    text=run("readelf","--debug-dump=loc",path)
    active=False
    for line in text.splitlines():
        offset=re.match(r"^\s*([0-9a-fA-F]{8,16})\b",line)
        if not active and offset and int(offset.group(1),16)==wanted:
            active=True
        if not active:
            continue
        if "<End of list>" in line:
            break
        if "(DW_OP_" not in line:
            continue
        prefix,expression=line.split("(",1)
        values=re.findall(r"\b[0-9a-fA-F]{8,16}\b",prefix)
        if len(values)<2:
            continue
        begin,end=int(values[-2],16),int(values[-1],16)
        expression=expression.rsplit(")",1)[0]
        if begin<=pc<end:
            return expression,(begin,end)
    raise RuntimeError(f"historical linked pointer location list misses return PC 0x{pc:x}")

def require_base(records,member,name,offset,size,encoding):
    if clean_name(member["attrs"].get("name",""))!=name:
        raise RuntimeError(f"expected member {name}")
    loc=member["attrs"].get("data_member_location")
    if loc is None or scalar_number(loc,f"{name} offset")!=offset:
        raise RuntimeError(f"{name}: compiler offset changed")
    t=member["attrs"].get("type")
    if not t:
        raise RuntimeError(f"{name}: missing type")
    base=unwrap_type(records,type_reference(t,name),name)
    if base["tag"]!="DW_TAG_base_type":
        raise RuntimeError(f"{name}: not a base type")
    if scalar_number(base["attrs"].get("byte_size",""),f"{name} size")!=size:
        raise RuntimeError(f"{name}: wrong size")
    if scalar_number(base["attrs"].get("encoding",""),f"{name} encoding")!=encoding:
        raise RuntimeError(f"{name}: wrong encoding")

def verify(path):
    records=parse_records(run("readelf","--debug-dump=info",path))
    parameter=find_parameter(records)
    type_attr=parameter["attrs"].get("type")
    location=parameter["attrs"].get("location")
    if not type_attr or not location:
        raise RuntimeError("historical member pointer lacks type/location evidence")
    root=unwrap_type(records,type_reference(type_attr,"root pointer"),"root pointer")
    if root["tag"]!="DW_TAG_pointer_type":
        raise RuntimeError("historical member root is not a pointer")
    if root["attrs"].get("byte_size") and scalar_number(
            root["attrs"]["byte_size"],"root pointer size")!=8:
        raise RuntimeError("root pointer is not eight bytes")
    pointee_attr=root["attrs"].get("type")
    if not pointee_attr:
        raise RuntimeError("root pointer has no pointee")
    structure=unwrap_type(records,type_reference(pointee_attr,"linked structure"),"linked structure")
    if structure["tag"]!="DW_TAG_structure_type":
        raise RuntimeError("root pointee is not a structure")
    if scalar_number(structure["attrs"].get("byte_size",""),"structure size")!=16:
        raise RuntimeError("HistoricalLiveLinked is not sixteen bytes")
    members=[m for m in direct_children(records,structure) if m["tag"]=="DW_TAG_member"]
    if len(members)!=2:
        raise RuntimeError(f"HistoricalLiveLinked requires two members, found {len(members)}")
    require_base(records,members[0],"direct",0,4,7)
    linked=members[1]
    if clean_name(linked["attrs"].get("name",""))!="linked":
        raise RuntimeError("second member is not linked")
    if scalar_number(linked["attrs"].get("data_member_location",""),"linked offset")!=8:
        raise RuntimeError("linked offset is not eight")
    linked_type=linked["attrs"].get("type")
    if not linked_type:
        raise RuntimeError("linked has no type")
    linked_pointer=unwrap_type(records,type_reference(linked_type,"linked"),"linked")
    if linked_pointer["tag"]!="DW_TAG_pointer_type":
        raise RuntimeError("linked is not a pointer")
    if linked_pointer["attrs"].get("byte_size") and scalar_number(
            linked_pointer["attrs"]["byte_size"],"linked pointer size")!=8:
        raise RuntimeError("linked pointer is not eight bytes")
    terminal_attr=linked_pointer["attrs"].get("type")
    if not terminal_attr:
        raise RuntimeError("linked pointer has no pointee")
    terminal=unwrap_type(records,type_reference(terminal_attr,"linked pointee"),"linked pointee")
    if terminal["tag"]!="DW_TAG_base_type" or scalar_number(
            terminal["attrs"].get("byte_size",""),"linked pointee size")!=4 or scalar_number(
            terminal["attrs"].get("encoding",""),"linked pointee encoding")!=5:
        raise RuntimeError("linked pointee is not signed int32")
    pc=call_return_pc(path)
    expression,owned=active_location(path,location,pc)
    match=re.fullmatch(r"DW_OP_reg(\d+) \(([^)]+)\)",expression)
    if not match:
        raise RuntimeError("root historical member pointer is not one register op: "+expression)
    range_text="direct" if owned is None else f"[0x{owned[0]:x},0x{owned[1]:x})"
    print("historical live pointer-member DWARF oracle passed: "
          f"return-pc=0x{pc:x} location={expression} range={range_text} "
          "pointee=HistoricalLiveLinked{direct@0:u32,linked@8:int32*}")

def main():
    if len(sys.argv)!=2:
        raise SystemExit("usage: live_historical_pointer_member_dwarf_oracle.py <fixture>")
    verify(sys.argv[1])

if __name__=="__main__":
    try:
        main()
    except (RuntimeError,subprocess.CalledProcessError) as error:
        print(f"historical live pointer-member DWARF oracle failure: {error}",file=sys.stderr)
        raise SystemExit(1)
