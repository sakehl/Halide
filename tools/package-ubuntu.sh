#!/bin/bash
set -e

halide_source="$1"
halide_build_root="$2"

[ -z "$LLVM_DIR" ] && echo "Must set specific LLVM_DIR for packaging" && exit
[ -z "$Clang_DIR" ] && echo "Must set specific Clang_DIR for packaging" && exit
[ -z "$halide_source" ] && echo "Usage: $0 <source-dir> <build-dir>" && exit
[ -z "$halide_build_root" ] && echo "Usage: $0 <source-dir> <build-dir>" && exit

cmake --preset=package-unix-shared -S "$halide_source" -B "$halide_build_root/shared-Release"
cmake --preset=package-unix-static -S "$halide_source" -B "$halide_build_root/static-Release"

cmake --build "$halide_build_root/shared-Release"
cmake --build "$halide_build_root/static-Release"

cd "$halide_build_root"
cat <<EOM >debian.cmake
include("shared-Release/CPackConfig.cmake")

set(CPACK_COMPONENTS_HALIDE_RUNTIME Halide_Runtime)
set(CPACK_COMPONENTS_HALIDE_DEVELOPMENT Halide_Development)

set(CPACK_INSTALL_CMAKE_PROJECTS
    # We don't package debug binaries on Unix systems. Our developers
    # don't use them and LLVM in debug mode is next to unusable, too.
    # static-Debug Halide ALL /
    # shared-Debug Halide ALL /
    static-Release Halide ALL /
    shared-Release Halide ALL /
)
EOM

cpack -G DEB --config debian.cmake
