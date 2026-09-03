#pragma once

#include "debugger/debugger.hpp"

#include <cstdint>

namespace mdbg {

enum class FinishStopReason { Returned, Interrupted };

struct FinishResult {
  FinishStopReason reason;
  StopInfo stop;
  std::uintptr_t return_address;
};

FinishResult finish_frame(Debugger& debugger);

}  // namespace mdbg
