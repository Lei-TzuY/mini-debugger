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
  require(cfi.available(),"historical pointer-member fixture missing .eh_frame");
  const auto callee=elf.find_symbol("historical_linked_callee_probe");
  const auto after=elf.find_symbol("historical_linked_after_probe");
  const auto object=elf.find_symbol("historical_linked_object");
  const auto linked_target=elf.find_symbol("historical_linked_target");
  const auto caller=elf.find_symbol("historical_linked_caller");
  require(callee&&after&&object&&linked_target&&caller&&caller->size!=0,
          "historical pointer-member symbols missing");
  const auto callee_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*callee));
  const auto after_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*after));
  const auto object_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*object));
  const auto linked_address=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*linked_target));
  const auto caller_begin=static_cast<std::uintptr_t>(
      elf.runtime_address(debugger.pid(),*caller));
  require(caller->size<=std::numeric_limits<std::uintptr_t>::max()-caller_begin,
          "historical pointer-member caller range overflows");
  const auto caller_end=caller_begin+caller->size;
  debugger.add_breakpoint(callee_address);
  debugger.add_breakpoint(after_address);
  const auto stop=debugger.continue_execution();
  require(stop.reason==mdbg::StopReason::Breakpoint&&stop.breakpoint_address==callee_address,
          "historical pointer-member fixture did not stop in callee");
  const auto frames=mdbg::build_inspection_frames(debugger,elf,cfi,3);
  require(frames.size()>=2&&frames[1].runtime_pc>=caller_begin&&frames[1].runtime_pc<caller_end,
          "CFI did not recover pointer-member caller frame");
  const auto pointer=mdbg::inspect_local_value(
      debugger,elf,frames[1],"historical_member_pointer");
  require(pointer.kind==mdbg::LocalValueKind::Pointer&&pointer.raw_value==object_address&&
              pointer.pointee_type,
          "historical member root lost pointer identity");
  const auto& type=*pointer.pointee_type;
  require(type.kind==mdbg::LocalValueKind::Structure&&type.byte_size==16&&
              type.members.size()==2,
          "historical member root lost structure metadata");
  require(type.members[0].name=="direct"&&type.members[0].kind==mdbg::LocalValueKind::Integer&&
              type.members[0].offset==0&&type.members[0].byte_size==4&&
              type.members[1].name=="linked"&&type.members[1].kind==mdbg::LocalValueKind::Pointer&&
              type.members[1].offset==8&&type.members[1].byte_size==8&&
              type.members[1].pointee_type&&type.members[1].pointee_type->byte_size==4&&
              type.members[1].pointee_type->is_signed,
          "historical pointer-valued member metadata changed");
  const auto materialized=mdbg::dereference_local_pointer(
      debugger,elf,frames[1],"historical_member_pointer");
  require(materialized.kind==mdbg::LocalValueKind::Structure&&
              materialized.members.size()==2&&
              materialized.members[0].raw_value==UINT64_C(0x55667788)&&
              materialized.members[1].kind==mdbg::LocalValueKind::Pointer&&
              materialized.members[1].raw_value==linked_address,
          "historical linked object was not materialized exactly");
  require(materialized.members[1].pointee_type &&
              materialized.members[1].pointee_type->byte_size==4 &&
              materialized.members[1].pointee_type->is_signed,
          "historical linked object lost pointer-member pointee metadata");

  const auto selected=mdbg::inspect_local_aggregate_member(materialized,"linked");
  require(selected.kind==mdbg::LocalValueKind::Pointer&&
              selected.raw_value==linked_address&&selected.pointee_type&&
              selected.pointee_type->kind==mdbg::LocalValueKind::Integer&&
              selected.pointee_type->byte_size==4&&selected.pointee_type->is_signed,
          "historical live pointer-member selection lost bounded metadata");
  const auto terminal=mdbg::dereference_local_pointer(
      debugger,frames[1],selected);
  require(terminal.kind==mdbg::LocalValueKind::Integer&&
              terminal.byte_size==4&&terminal.is_signed&&
              terminal.raw_value==UINT64_C(0x02468ace),
          "historical live pointer-member dereference lost terminal int32 value");

  auto null_member=selected;
  null_member.raw_value=0;
  bool null_rejected=false;
  try{
    (void)mdbg::dereference_local_pointer(debugger,frames[1],null_member);
  }catch(const std::runtime_error&){
    null_rejected=true;
  }
  require(null_rejected,"historical live null pointer member was dereferenced");

  auto malformed_member=selected;
  malformed_member.pointee_type->byte_size=0;
  bool malformed_rejected=false;
  try{
    (void)mdbg::dereference_local_pointer(debugger,frames[1],malformed_member);
  }catch(const std::runtime_error&){
    malformed_rejected=true;
  }
  require(malformed_rejected,
          "historical live malformed pointer-member metadata was accepted");
  const auto next=debugger.continue_execution();
  require(next.reason==mdbg::StopReason::Breakpoint&&next.breakpoint_address==after_address,
          "historical pointer-member fixture did not reach next stop");
  bool stale_rejected=false;
  try{
    (void)mdbg::dereference_local_pointer(debugger,frames[1],selected);
  }catch(const std::logic_error&){
    stale_rejected=true;
  }
  require(stale_rejected,
          "historical live pointer member accepted a stale inspection frame");
  const auto exit=debugger.continue_execution();
  require(exit.reason==mdbg::StopReason::Exited&&exit.value==0,
          "historical pointer-member fixture did not exit cleanly");
}
}
int main(int argc,char** argv){
  if(argc!=2) return 2;
  try{ verify(argv[1]); return 0; }
  catch(const std::exception& error){
    std::fprintf(stderr,"historical live pointer-member integration failure: %s\n",error.what());
    return 1;
  }
}
