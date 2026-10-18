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
set(KI_FUNCTIONS "8802d560;8802d774" CACHE STRING "semicolon-separated guest functions to recompile")
set(KI_PYTHON "${CMAKE_SOURCE_DIR}/.venv/bin/python" CACHE FILEPATH "Python with rabbitizer installed")

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-label)
endif()

add_library(ki_runtime STATIC runtime/src/runtime.cpp)
target_include_directories(ki_runtime PUBLIC runtime/include)

set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
set(GEN_SRC "${GEN_DIR}/recompiled.cpp")
set(func_args "")
foreach(f IN LISTS KI_FUNCTIONS)
  list(APPEND func_args --func ${f})
endforeach()
add_custom_command(
  OUTPUT "${GEN_SRC}" "${GEN_DIR}/recompiled.lst"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
  COMMAND "${KI_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py"
          --image "${KI_CODE_IMAGE}" --vram 88000000 --expect-sha1 ${KI_CODE_IMAGE_SHA1}
          ${func_args} --out "${GEN_SRC}" --listing "${GEN_DIR}/recompiled.lst"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/kirecomp/kirecomp.py" "${KI_CODE_IMAGE}"
  COMMENT "Recompiling guest functions: ${KI_FUNCTIONS}"
  VERBATIM)

add_library(ki_recompiled STATIC "${GEN_SRC}")
target_link_libraries(ki_recompiled PUBLIC ki_runtime)

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
