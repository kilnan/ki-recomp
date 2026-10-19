cmake_minimum_required(VERSION 3.20)
project(ki_recomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Original game data never lives in the repository. The extracted code image
# (see tools/extract_game_code.sh) is an input of the build.
set(KI_CODE_IMAGE "${CMAKE_SOURCE_DIR}/data/kipack-out/rom-0.bin" CACHE FILEPATH
    "KI 1 L1.5D game code image (rom-0, loaded at 0x88000000)")
set(KI_CODE_IMAGE_SHA1 "ed875b9f2a0d1bccf79ff63189c37488ee718659" CACHE STRING
    "expected SHA1 of KI_CODE_IMAGE")
set(KI_DATA_IMAGES "${CMAKE_SOURCE_DIR}/data/kipack-out/rom-1.bin@88033900;${CMAKE_SOURCE_DIR}/data/kipack-out/rom-2.bin@88033880"
    CACHE STRING "read-only data images (FILE@VRAM) the translator reads jump tables from")
set(KI_FUNCTIONS "8802d560;8802d774;880176f0;8801a1e8;8802d694;8802dc94;880274c8;8801905c;880107a8;880123d4;88011dac;88010d8c;88010d78;88010da8" CACHE STRING "semicolon-separated guest functions to recompile")
set(KI_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with rabbitizer installed")

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-label)
endif()

# ADSP-2105 core for the DCS sound board, derived from MAME (BSD-3-Clause, see third_party/adsp2100).
add_library(adsp2105 STATIC third_party/adsp2100/adsp2105.cpp)
target_include_directories(adsp2105 PUBLIC third_party/adsp2100)
if(NOT MSVC)
  target_compile_options(adsp2105 PRIVATE -Wno-sign-compare -O2)
endif()

add_library(ki_runtime STATIC runtime/src/runtime.cpp runtime/src/kinst_board.cpp runtime/src/dcs.cpp)
target_include_directories(ki_runtime PUBLIC runtime/include)
target_link_libraries(ki_runtime PUBLIC adsp2105)

# A recompiled-code library defines the function table (ki::kFunctionTable) that
# the runtime looks entries up in, so the two static libraries depend on each
# other. Single-pass linkers (GNU ld) need the code library again after the
# runtime; Apple's linker does not mind.
function(ki_code_library target)
  target_link_libraries(${target} PUBLIC ki_runtime)
  target_link_libraries(${target} INTERFACE "$<TARGET_FILE:${target}>")
endfunction()

set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_SRC "${GEN_DIR}/recompiled.cpp")
set(func_args "")
foreach(f IN LISTS KI_FUNCTIONS)
  list(APPEND func_args --func ${f})
endforeach()
set(data_deps "")
foreach(d IN LISTS KI_DATA_IMAGES)
  list(APPEND func_args --data ${d})
  string(REGEX REPLACE "@[0-9a-fA-F]+$" "" d_file "${d}")
  list(APPEND data_deps "${d_file}")
endforeach()
add_custom_command(
  OUTPUT "${GEN_SRC}" "${GEN_DIR}/recompiled.lst"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
  COMMAND "${KI_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py"
          --image "${KI_CODE_IMAGE}" --vram 88000000 --expect-sha1 ${KI_CODE_IMAGE_SHA1}
          ${func_args} --no-timing --out "${GEN_SRC}" --listing "${GEN_DIR}/recompiled.lst"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py" "${KI_CODE_IMAGE}" ${data_deps}
  COMMENT "Recompiling guest functions: ${KI_FUNCTIONS}"
  VERBATIM)

add_library(ki_recompiled STATIC "${GEN_SRC}")
ki_code_library(ki_recompiled)

add_executable(compare_case tests/compare_case.cpp)
target_link_libraries(compare_case PRIVATE ki_recompiled)

# Runtime self-test with synthetic values only (no game data needed).
add_executable(runtime_selftest tests/runtime_selftest.cpp tests/empty_function_table.cpp)
target_link_libraries(runtime_selftest PRIVATE ki_runtime)

enable_testing()
add_test(NAME runtime_selftest COMMAND runtime_selftest)
file(GLOB _cases LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/traces/poc/*")
foreach(case_dir IN LISTS _cases)
  if(IS_DIRECTORY "${case_dir}" AND EXISTS "${case_dir}/exit_regs.txt")
    get_filename_component(case_name "${case_dir}" NAME)
    add_test(NAME "poc_${case_name}" COMMAND compare_case "${case_dir}")
  endif()
endforeach()

# ---- whole program -------------------------------------------------------------
# Every entry point the translator can find (jal targets, code pointers in the
# data images) plus the entry and the exception handler, split into
# KI_SPLIT units so they compile in parallel. With cycle accounting and
# interrupt polling, for running on the board model.
option(KI_BUILD_FULL "build the whole-program executable ki_run" ON)
if(KI_BUILD_FULL)
  set(KI_SPLIT 16 CACHE STRING "number of generated translation units for the whole program")
  # Return sites that mid-game snapshots (tools/mame_snapshot.py) continue at
  # after their entry function returns: in the main loop after the per-frame
  # call at 0x880007e0, after the dispatcher call at 0x8800099c, and in the
  # main loop at 0x8802aa24 after its call to 0x88003e08.
  set(KI_EXTRA_ENTRIES "880007e8;880009a4;8802af78" CACHE STRING "additional entry points for the whole program")
  set(extra_args "")
  foreach(e IN LISTS KI_EXTRA_ENTRIES)
    list(APPEND extra_args --func ${e})
  endforeach()
  set(FULL_DIR "${CMAKE_BINARY_DIR}/gen_full")
  set(full_srcs "${FULL_DIR}/recompiled_table.cpp")
  math(EXPR last "${KI_SPLIT} - 1")
  foreach(k RANGE ${last})
    list(APPEND full_srcs "${FULL_DIR}/recompiled_${k}.cpp")
  endforeach()
  set(data_args "")
  foreach(d IN LISTS KI_DATA_IMAGES)
    list(APPEND data_args --data ${d})
  endforeach()
  add_custom_command(
    OUTPUT ${full_srcs} "${FULL_DIR}/recompiled.h"
    COMMAND "${KI_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py"
            --image "${KI_CODE_IMAGE}" --vram 88000000 --expect-sha1 ${KI_CODE_IMAGE_SHA1} ${data_args}
            --func 88000000 --func 880266c8 ${extra_args} --discover --discover-pointers
            --out-dir "${FULL_DIR}" --split ${KI_SPLIT}
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py" "${KI_CODE_IMAGE}" ${data_deps}
    COMMENT "Recompiling the whole program"
    VERBATIM)
  add_library(ki_full_code STATIC ${full_srcs})
  target_include_directories(ki_full_code PRIVATE "${FULL_DIR}")
  ki_code_library(ki_full_code)
  # The huge dispatch functions make -O2 very slow to compile; -O1 is enough for now.
  if(NOT MSVC)
    target_compile_options(ki_full_code PRIVATE -O1 -Wno-unused-parameter)
  endif()
  # compare_case against the whole program and the board model (for captures of
  # functions that call other code or touch I/O).
  add_executable(compare_full tests/compare_case.cpp)
  target_compile_definitions(compare_full PRIVATE KI_COMPARE_FULL)
  target_link_libraries(compare_full PRIVATE ki_full_code)

  # Same program generated with a per-instruction trace hook (lockstep debugging).
  option(KI_BUILD_TRACE "build ki_run_trace" OFF)
  if(KI_BUILD_TRACE)
    set(TRACE_DIR "${CMAKE_BINARY_DIR}/gen_trace")
    set(trace_srcs "${TRACE_DIR}/recompiled_table.cpp")
    foreach(k RANGE ${last})
      list(APPEND trace_srcs "${TRACE_DIR}/recompiled_${k}.cpp")
    endforeach()
    add_custom_command(
      OUTPUT ${trace_srcs} "${TRACE_DIR}/recompiled.h"
      COMMAND "${KI_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py"
              --image "${KI_CODE_IMAGE}" --vram 88000000 --expect-sha1 ${KI_CODE_IMAGE_SHA1} ${data_args}
              --func 88000000 --func 880266c8 ${extra_args} --discover --discover-pointers --trace
              --out-dir "${TRACE_DIR}" --split ${KI_SPLIT}
      DEPENDS "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py" "${KI_CODE_IMAGE}" ${data_deps}
      COMMENT "Recompiling the whole program with trace hooks"
      VERBATIM)
    add_library(ki_trace_code STATIC ${trace_srcs})
    target_include_directories(ki_trace_code PRIVATE "${TRACE_DIR}")
    ki_code_library(ki_trace_code)
    if(NOT MSVC)
      target_compile_options(ki_trace_code PRIVATE -O1 -Wno-unused-parameter)
    endif()
    add_executable(ki_run_trace src/ki_run.cpp src/ki_boot.cpp)
    target_link_libraries(ki_run_trace PRIVATE ki_trace_code)
  endif()

  add_library(ki_boot STATIC src/ki_boot.cpp)
  target_link_libraries(ki_boot PUBLIC ki_runtime)
  add_executable(ki_run src/ki_run.cpp)
  target_link_libraries(ki_run PRIVATE ki_boot ki_full_code)

  # DCS model against a MAME capture (tools/mame_dcs_capture.py); not a ctest
  # because the capture is derived from the game and lives in traces/.
  add_executable(dcs_replay tests/dcs_replay.cpp)
  target_link_libraries(dcs_replay PRIVATE ki_boot ki_full_code)
  target_include_directories(dcs_replay PRIVATE src)

  # Windowed player: needs SDL2 (on macOS: brew install sdl2-compat or sdl2).
  find_package(SDL2 CONFIG QUIET)
  if(SDL2_FOUND)
    add_executable(ki_play src/ki_play.cpp)
    target_link_libraries(ki_play PRIVATE ki_boot ki_full_code SDL2::SDL2)
  else()
    message(STATUS "SDL2 not found: ki_play is not built")
  endif()
endif()
