#!/bin/bash
set -e

halide_source=$(readlink -f "$1")
halide_build_root=$(readlink -f "$2")

[ -z "$halide_source" ] && echo "Usage: $0 <source-dir> <build-dir>" && exit
[ -z "$halide_build_root" ] && echo "Usage: $0 <source-dir> <build-dir>" && exit

cmake --preset=package-ubuntu-shared -S "$halide_source" -B "$halide_build_root/shared-Release"
cmake --preset=package-ubuntu-static -S "$halide_source" -B "$halide_build_root/static-Release"

cmake --build "$halide_build_root/shared-Release"
cmake --build "$halide_build_root/static-Release"

cd "$halide_build_root"

rm -rf ./_CPack_Packages ./*.deb

# ensure correct umask is set for creating packages
umask 0022

cpack -G DEB -C Release --config "$halide_source/packaging/ubuntu/config.cmake"

echo "Running STRICT lintian checks..."
lintian -F ./*.deb

echo "Running ALL lintian checks..."
lintian --no-tag-display-limit ./*.deb
