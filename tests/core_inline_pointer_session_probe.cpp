#include "snapshot/session.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: core_inline_pointer_session_probe <core>\n";
    return 2;
  }

  try {
    mdbg::CoreInspectionSession session(argv[1]);
    require(session.trace().frames.size() > 1,
            "caller-inline core did not retain physical caller frame 1");
    session.select_frame(1);
    const auto contexts = session.inline_contexts();
    require(contexts.size() >= 2 && contexts[1].name == "caller_inline_inner",
            "caller-inline core did not expose inner selected-inline context");
    session.select_inline_context(1);

    const auto pointer = session.inspect_value("caller_pointer");
    require(pointer.kind == mdbg::LocalValueKind::Pointer,
            "selected-inline caller_pointer was not materialized as a pointer");
    require(pointer.byte_size == sizeof(std::uintptr_t),
            "selected-inline caller_pointer lost x86-64 pointer width");
    require(pointer.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "selected-inline caller_pointer did not come from immutable caller stack memory");
    require(pointer.pointee_type.has_value(),
            "selected-inline caller_pointer lost pointee metadata");
    require(pointer.pointee_type->kind == mdbg::LocalValueKind::Integer &&
                pointer.pointee_type->byte_size == sizeof(int) &&
                pointer.pointee_type->is_signed,
            "selected-inline caller_pointer has unexpected pointee metadata");

    const auto value = session.dereference_value("caller_pointer");
    require(value.name == "*caller_pointer" &&
                value.kind == mdbg::LocalValueKind::Integer &&
                value.raw_value == UINT64_C(0x02468ace) &&
                value.byte_size == sizeof(int) && value.is_signed,
            "selected-inline pointer did not dereference the compiler-owned integer pointee");
    require(value.storage == mdbg::LocalValueStorage::SnapshotCoreMemory,
            "selected-inline pointee bytes did not retain core-memory provenance");

    session.clear_inline_context();
    require(!session.selected_inline_context_index(),
            "returning to physical ownership did not invalidate inline selection");

    std::cout << "selected-inline pointer session probe passed\n";
  } catch (const std::exception& error) {
    std::cerr << "selected-inline pointer session probe failure: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
