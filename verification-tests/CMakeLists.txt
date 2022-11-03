cmake_minimum_required(VERSION 3.16)
project(HalideDiffusion)

set(CMAKE_CXX_STANDARD 11)  # or newer
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)
set(CMAKE_PREFIX_PATH ../Halide-install)


find_package(Halide REQUIRED)
find_package(Threads REQUIRED)

#add_executable(diffusion diffusion.cpp)
#target_link_libraries(diffusion PRIVATE Halide::Halide)

add_executable(even even.cpp)
target_link_libraries(even PRIVATE Halide::Halide)

add_custom_command(
  OUTPUT opencl_runtime.o even_c.h
  COMMAND even --runtime
  DEPENDS even
)

add_custom_command(
  OUTPUT even_static.a even_static.h
  COMMAND even --aot
  DEPENDS even
)

add_custom_target(rebuilding_even ALL)

set(OUTFILE halide.out)

add_custom_command(
  OUTPUT even_c.cpp ${OUTFILE}
  COMMAND HL_DEBUG_CODEGEN=3 ./even > ${OUTFILE} 2>&1
  COMMENT "See ${OUTFILE}!"
  DEPENDS even rebuilding_even
  VERBATIM
)


add_custom_command(
  OUTPUT even_c_run_static
  COMMAND g++ ../even_c_run_static.cpp even_static.a -I . -I ../../Halide-install/include/ -lpthread -ldl -o even_c_run_static
  DEPENDS even_static.a even_static.h
)

add_custom_target(
    even_c_run_static_t ALL
    DEPENDS even_c_run_static
) 

add_executable(even_c_run 
  even_c_run.cpp
  opencl_runtime.o
  even_c.cpp)

target_link_libraries(even_c_run
  PRIVATE Halide::Halide
  pthread
  dl)

target_include_directories(even_c_run 
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR})


# add_executable(even_c_run_static
#   even_c_run_static.cpp
#   even_static.a)

# target_link_libraries(even_c_run_static
#   PUBLIC Halide::Halide
#   pthread
#   dl)

# target_include_directories(even_c_run_static 
#   PRIVATE ${CMAKE_CURRENT_BINARY_DIR})