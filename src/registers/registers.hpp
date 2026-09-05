#pragma once

#include <sys/user.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace mdbg {

std::optional<std::uint64_t> register_value(const user_regs_struct& regs,
                                            std::string_view name);
bool assign_general_purpose_register(user_regs_struct& regs, std::string_view name,
                                     std::uint64_t value);
std::vector<std::pair<std::string_view, std::uint64_t>>
general_purpose_registers(const user_regs_struct& regs);

}  // namespace mdbg
