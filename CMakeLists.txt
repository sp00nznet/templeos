cmake_minimum_required(VERSION 3.16)
project(templeos C)

# The lifted kernel is generated from the user's ISO: tools/gen.py writes src/gen/.
if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/gen/kernel_files.cmake)
  message(FATAL_ERROR "src/gen/ is empty: run Setup (or python tools/gen.py <TempleOS.ISO>) first")
endif()
include(${CMAKE_SOURCE_DIR}/src/gen/kernel_files.cmake)

set(PCRECOMP_DIR ${CMAKE_SOURCE_DIR}/../pcrecomp-temple CACHE PATH "pcrecomp checkout (branch temple)")
find_package(SDL2 REQUIRED)

add_executable(templeos
  runtime/main.c runtime/machine.c runtime/tasks.c runtime/devices.c
  runtime/present.c runtime/dispatch.c src/gen/dispatch.c ${KERNEL_GEN})
target_include_directories(templeos PRIVATE runtime src/gen ${PCRECOMP_DIR}/runtime/recomp64_cpu)
target_compile_definitions(templeos PRIVATE RECOMP64_BASED RECOMP64_MMIO RECOMP64_SYSTEM)
if(TARGET SDL2::SDL2)
  target_link_libraries(templeos PRIVATE SDL2::SDL2)
else()
  target_include_directories(templeos PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(templeos PRIVATE ${SDL2_LIBRARIES})
endif()
if(MSVC OR CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_options(templeos PRIVATE /W3 /wd4146 /wd4244 /wd4018)
else()
  target_compile_options(templeos PRIVATE -O1 -fwrapv -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable)
  target_link_libraries(templeos PRIVATE m)
endif()
