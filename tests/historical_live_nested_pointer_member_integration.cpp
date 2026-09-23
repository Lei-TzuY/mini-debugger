#include "debugger/debugger.hpp"
#include "dwarf/eh_frame.hpp"
#include "dwarf/inline_member.hpp"
#include "dwarf/local_value.hpp"
#include "elf/elf.hpp"
#include "unwind/cfi.hpp"
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition,const std::string& message){
  if(!condition) throw std::runtime_error(message);
}
void verify(const std::string& fixture){
  auto debugger=mdbg::Debugger::launch(fixture,{});
  const mdbg::ElfFile elf(fixture);
  const mdbg::EhFrame cfi(fixture);
  require(cfi.available(),"historical nested-pointer fixture missing .eh_frame");
  const auto callee=elf.find_symbol("historical_nested_callee_probe");
  const auto after=elf.find_symbol("historical_nested_after_probe");
  const auto object=elf.find_symbol("historical_nested_object");
  const auto leaf=elf.find_symbol("historical_nested_leaf");
  const auto caller=elf.find_symbol("historical_nested_caller");
  require(callee&&after&&object&&leaf&&caller&&caller->size!=0,
          "historical nested-pointer symbols missing");
  const auto callee_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*callee));
  const auto after_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*after));
  const auto object_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*object));
  const auto leaf_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*leaf));
  const auto caller_begin=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*caller));
  require(caller->size<=std::numeric_limits<std::uintptr_t>::max()-caller_begin,
          "historical nested-pointer caller range overflows");
  const auto caller_end=caller_begin+caller->size;
  debugger.add_breakpoint(callee_address);
  debugger.add_breakpoint(after_address);
  const auto stop=debugger.continue_execution();
  require(stop.reason==mdbg::StopReason::Breakpoint&&stop.breakpoint_address==callee_address,
          "historical nested-pointer fixture did not stop in callee");
  const auto frames=mdbg::build_inspection_frames(debugger,elf,cfi,3);
  require(frames.size()>=2&&frames[1].runtime_pc>=caller_begin&&frames[1].runtime_pc<caller_end,
          "CFI did not recover nested-pointer caller frame");

  const auto pointer=mdbg::inspect_local_value(
      debugger,elf,frames[1],"historical_nested_pointer");
  require(pointer.kind==mdbg::LocalValueKind::Pointer&&
              pointer.raw_value==object_address&&pointer.pointee_type&&
              pointer.pointee_type->kind==mdbg::LocalValueKind::Structure,
          "historical nested root lost pointer-to-structure identity");
  require(pointer.pointee_type->byte_size==16&&
              pointer.pointee_type->members.size()==2,
          "historical nested root lost outer structure metadata");

  const auto outer=mdbg::dereference_local_pointer(
      debugger,elf,frames[1],"historical_nested_pointer");
  require(outer.kind==mdbg::LocalValueKind::Structure&&outer.members.size()==2&&
              outer.members[0].name=="direct"&&
              outer.members[0].raw_value==UINT64_C(0x55667788)&&
              outer.members[1].name=="linked"&&
              outer.members[1].kind==mdbg::LocalValueKind::Pointer&&
              outer.members[1].raw_value==leaf_address,
          "historical nested outer object was not materialized exactly");

  const auto selected=mdbg::inspect_local_aggregate_member(outer,"linked");
  require(selected.kind==mdbg::LocalValueKind::Pointer&&
              selected.raw_value==leaf_address&&selected.pointee_type&&
              selected.pointee_type->kind==mdbg::LocalValueKind::Structure&&
              selected.pointee_type->byte_size==8&&
              selected.pointee_type->members.size()==2,
          "historical nested pointer member lost bounded structure-pointee metadata");
  require(selected.pointee_type->members[0].name=="terminal"&&
              selected.pointee_type->members[0].kind==mdbg::LocalValueKind::Integer&&
              selected.pointee_type->members[0].offset==0&&
              selected.pointee_type->members[0].byte_size==4&&
              selected.pointee_type->members[0].is_signed&&
              selected.pointee_type->members[1].name=="marker"&&
              selected.pointee_type->members[1].kind==mdbg::LocalValueKind::Integer&&
              selected.pointee_type->members[1].offset==4&&
              selected.pointee_type->members[1].byte_size==4&&
              !selected.pointee_type->members[1].is_signed,
          "historical nested leaf layout metadata changed");

  const auto leaf_value=mdbg::dereference_local_pointer(
      debugger,frames[1],selected);
  require(leaf_value.kind==mdbg::LocalValueKind::Structure&&
              leaf_value.byte_size==8&&leaf_value.members.size()==2&&
              leaf_value.members[0].name=="terminal"&&
              leaf_value.members[0].raw_value==UINT64_C(0x02468ace)&&
              leaf_value.members[0].is_signed&&
              leaf_value.members[1].name=="marker"&&
              leaf_value.members[1].raw_value==UINT64_C(0x89abcdef)&&
              !leaf_value.members[1].is_signed,
          "historical nested structure dereference did not recover the leaf exactly");

  auto null_member=selected;
  null_member.raw_value=0;
  bool null_rejected=false;
  try{
    (void)mdbg::dereference_local_pointer(debugger,frames[1],null_member);
  }catch(const std::runtime_error&){
    null_rejected=true;
  }
  require(null_rejected,"historical nested null pointer member was dereferenced");

  const auto next=debugger.continue_execution();
  require(next.reason==mdbg::StopReason::Breakpoint&&next.breakpoint_address==after_address,
          "historical nested-pointer fixture did not reach next stop");
  bool stale_rejected=false;
  try{
    (void)mdbg::dereference_local_pointer(debugger,frames[1],selected);
  }catch(const std::logic_error&){
    stale_rejected=true;
  }
  require(stale_rejected,
          "historical nested pointer member accepted a stale inspection frame");
  const auto exit=debugger.continue_execution();
  require(exit.reason==mdbg::StopReason::Exited&&exit.value==0,
          "historical nested-pointer fixture did not exit cleanly");
}
}
int main(int argc,char** argv){
  if(argc!=2) return 2;
  try{ verify(argv[1]); return 0; }
  catch(const std::exception& error){
    std::fprintf(stderr,"historical live nested pointer-member integration failure: %s\n",error.what());
    return 1;
  }
}
