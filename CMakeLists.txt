cmake_minimum_required(VERSION 3.20)
project(mini_debugger LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "mini-debugger currently supports Linux only")
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
  message(FATAL_ERROR "mini-debugger currently supports x86-64 only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(mdbg_core
  src/ptrace/ptrace.cpp
  src/process/process.cpp
  src/registers/registers.cpp
  src/elf/elf.cpp
  src/debugger/debugger.cpp
  src/unwind/frame_pointer.cpp
  src/unwind/cfi.cpp
  src/dwarf/line_table.cpp
  src/dwarf/source_lookup.cpp
  src/dwarf/eh_frame.cpp
  src/source/source_step.cpp
  src/source/source_finish.cpp
)
target_include_directories(mdbg_core PUBLIC src)
target_compile_options(mdbg_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(mdbg src/cli/main.cpp)
target_link_libraries(mdbg PRIVATE mdbg_core)
target_compile_options(mdbg PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
  add_executable(debugger_fixture_pie tests/fixtures/debugger_fixture.c)
  target_compile_options(debugger_fixture_pie PRIVATE -O0 -g -fno-omit-frame-pointer -fPIE)
  target_link_options(debugger_fixture_pie PRIVATE -pie)

  add_executable(debugger_fixture_nopie tests/fixtures/debugger_fixture.c)
  target_compile_options(debugger_fixture_nopie PRIVATE -O0 -g -fno-omit-frame-pointer -fno-pie)
  target_link_options(debugger_fixture_nopie PRIVATE -no-pie)

  add_executable(debugger_fixture_stripped tests/fixtures/debugger_fixture.c)
  target_compile_options(debugger_fixture_stripped PRIVATE -O0 -fPIE)
  target_link_options(debugger_fixture_stripped PRIVATE -pie)
  add_custom_command(TARGET debugger_fixture_stripped POST_BUILD
    COMMAND ${CMAKE_STRIP} --strip-all $<TARGET_FILE:debugger_fixture_stripped>)

  add_executable(debug_line_fixture_pie tests/fixtures/debug_line_fixture.c)
  target_compile_options(debug_line_fixture_pie PRIVATE -O0 -g -gdwarf-4 -fPIE)
  target_link_options(debug_line_fixture_pie PRIVATE -pie)

  add_executable(debug_line_fixture_nopie tests/fixtures/debug_line_fixture.c)
  target_compile_options(debug_line_fixture_nopie PRIVATE -O0 -g -gdwarf-4 -fno-pie)
  target_link_options(debug_line_fixture_nopie PRIVATE -no-pie)

  add_executable(debug_line_fixture_v5 tests/fixtures/debug_line_fixture.c)
  target_compile_options(debug_line_fixture_v5 PRIVATE -O0 -g -gdwarf-5 -fno-pie)
  target_link_options(debug_line_fixture_v5 PRIVATE -no-pie)

  add_executable(source_next_fixture_pie tests/fixtures/source_next_fixture.c)
  target_compile_options(source_next_fixture_pie PRIVATE
    -O0 -g -gdwarf-4 -fno-omit-frame-pointer -fPIE)
  target_link_options(source_next_fixture_pie PRIVATE -pie)

  add_executable(source_next_fixture_nopie tests/fixtures/source_next_fixture.c)
  target_compile_options(source_next_fixture_nopie PRIVATE
    -O0 -g -gdwarf-4 -fno-omit-frame-pointer -fno-pie)
  target_link_options(source_next_fixture_nopie PRIVATE -no-pie)

  add_executable(source_finish_omit_fixture_pie tests/fixtures/source_finish_omit_fixture.c)
  target_compile_options(source_finish_omit_fixture_pie PRIVATE
    -O0 -g -gdwarf-4 -fomit-frame-pointer -fPIE)
  target_link_options(source_finish_omit_fixture_pie PRIVATE -pie)

  add_executable(source_finish_omit_fixture_nopie tests/fixtures/source_finish_omit_fixture.c)
  target_compile_options(source_finish_omit_fixture_nopie PRIVATE
    -O0 -g -gdwarf-4 -fomit-frame-pointer -fno-pie)
  target_link_options(source_finish_omit_fixture_nopie PRIVATE -no-pie)

  add_library(shared_cfi_fixture SHARED tests/fixtures/shared_cfi_library.c)
  target_compile_options(shared_cfi_fixture PRIVATE -O0 -g -gdwarf-4 -fomit-frame-pointer -fPIC)

  add_executable(shared_cfi_driver_pie tests/fixtures/shared_cfi_driver.c)
  target_compile_options(shared_cfi_driver_pie PRIVATE
    -O0 -g -gdwarf-4 -fomit-frame-pointer -fPIE)
  target_link_libraries(shared_cfi_driver_pie PRIVATE shared_cfi_fixture)
  target_link_options(shared_cfi_driver_pie PRIVATE -pie)
  set_target_properties(shared_cfi_driver_pie PROPERTIES BUILD_RPATH "$ORIGIN")

  add_executable(shared_cfi_driver_nopie tests/fixtures/shared_cfi_driver.c)
  target_compile_options(shared_cfi_driver_nopie PRIVATE
    -O0 -g -gdwarf-4 -fomit-frame-pointer -fno-pie)
  target_link_libraries(shared_cfi_driver_nopie PRIVATE shared_cfi_fixture)
  target_link_options(shared_cfi_driver_nopie PRIVATE -no-pie)
  set_target_properties(shared_cfi_driver_nopie PROPERTIES BUILD_RPATH "$ORIGIN")

  add_executable(debugger_integration tests/debugger_integration.cpp)
  target_link_libraries(debugger_integration PRIVATE mdbg_core)
  target_compile_options(debugger_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_test(NAME debugger_integration_pie
    COMMAND debugger_integration $<TARGET_FILE:debugger_fixture_pie>)
  add_test(NAME debugger_integration_nopie
    COMMAND debugger_integration $<TARGET_FILE:debugger_fixture_nopie>)

  add_executable(attach_integration tests/attach_integration.cpp)
  target_link_libraries(attach_integration PRIVATE mdbg_core)
  target_compile_options(attach_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME attach_integration_pie
    COMMAND attach_integration $<TARGET_FILE:debugger_fixture_pie>)
  add_test(NAME attach_integration_nopie
    COMMAND attach_integration $<TARGET_FILE:debugger_fixture_nopie>)

  add_executable(backtrace_integration tests/backtrace_integration.cpp)
  target_link_libraries(backtrace_integration PRIVATE mdbg_core)
  target_compile_options(backtrace_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME backtrace_integration_pie
    COMMAND backtrace_integration
      $<TARGET_FILE:debugger_fixture_pie>
      $<TARGET_FILE:source_finish_omit_fixture_pie>)
  add_test(NAME backtrace_integration_nopie
    COMMAND backtrace_integration
      $<TARGET_FILE:debugger_fixture_nopie>
      $<TARGET_FILE:source_finish_omit_fixture_nopie>)

  add_executable(shared_cfi_backtrace_integration tests/shared_cfi_backtrace_integration.cpp)
  target_link_libraries(shared_cfi_backtrace_integration PRIVATE mdbg_core)
  target_compile_options(shared_cfi_backtrace_integration PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME shared_cfi_backtrace_integration_pie
    COMMAND shared_cfi_backtrace_integration
      $<TARGET_FILE:shared_cfi_driver_pie>
      $<TARGET_FILE:shared_cfi_fixture>
      $<TARGET_FILE:debug_line_fixture_v5>)
  add_test(NAME shared_cfi_backtrace_integration_nopie
    COMMAND shared_cfi_backtrace_integration
      $<TARGET_FILE:shared_cfi_driver_nopie>
      $<TARGET_FILE:shared_cfi_fixture>
      $<TARGET_FILE:debug_line_fixture_v5>)

  add_executable(dwarf_line_integration tests/dwarf_line_integration.cpp)
  target_link_libraries(dwarf_line_integration PRIVATE mdbg_core)
  target_compile_options(dwarf_line_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME dwarf_line_integration_pie
    COMMAND dwarf_line_integration
      $<TARGET_FILE:debug_line_fixture_pie>
      $<TARGET_FILE:debugger_fixture_stripped>
      $<TARGET_FILE:debug_line_fixture_v5>)
  add_test(NAME dwarf_line_integration_nopie
    COMMAND dwarf_line_integration
      $<TARGET_FILE:debug_line_fixture_nopie>
      $<TARGET_FILE:debugger_fixture_stripped>
      $<TARGET_FILE:debug_line_fixture_v5>)

  add_executable(source_step_integration tests/source_step_integration.cpp)
  target_link_libraries(source_step_integration PRIVATE mdbg_core)
  target_compile_options(source_step_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME source_step_integration_pie
    COMMAND source_step_integration
      $<TARGET_FILE:debug_line_fixture_pie>
      $<TARGET_FILE:debugger_fixture_stripped>)
  add_test(NAME source_step_integration_nopie
    COMMAND source_step_integration
      $<TARGET_FILE:debug_line_fixture_nopie>
      $<TARGET_FILE:debugger_fixture_stripped>)

  add_executable(source_next_integration tests/source_next_integration.cpp)
  target_link_libraries(source_next_integration PRIVATE mdbg_core)
  target_compile_options(source_next_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME source_next_integration_pie
    COMMAND source_next_integration $<TARGET_FILE:source_next_fixture_pie>)
  add_test(NAME source_next_integration_nopie
    COMMAND source_next_integration $<TARGET_FILE:source_next_fixture_nopie>)

  add_executable(source_finish_integration tests/source_finish_integration.cpp)
  target_link_libraries(source_finish_integration PRIVATE mdbg_core)
  target_compile_options(source_finish_integration PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME source_finish_integration_pie
    COMMAND source_finish_integration
      $<TARGET_FILE:source_next_fixture_pie>
      $<TARGET_FILE:source_finish_omit_fixture_pie>)
  add_test(NAME source_finish_integration_nopie
    COMMAND source_finish_integration
      $<TARGET_FILE:source_next_fixture_nopie>
      $<TARGET_FILE:source_finish_omit_fixture_nopie>)

  add_executable(elf_tests tests/elf_tests.cpp)
  target_link_libraries(elf_tests PRIVATE mdbg_core)
  target_compile_options(elf_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME elf_tests COMMAND elf_tests
    $<TARGET_FILE:debugger_fixture_pie>
    $<TARGET_FILE:debugger_fixture_nopie>
    $<TARGET_FILE:debugger_fixture_stripped>)
endif()
