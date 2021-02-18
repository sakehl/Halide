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

# This is a horrible, ugly, hack to patch around CPack breaking on PIE executables.
real_file=$(which file)
mkdir -p "$halide_build_root/_shims"
cat <<EOM >"$halide_build_root/_shims/file"
#!/bin/bash
$real_file "\$*" | awk '/ELF.*interpreter/ { sub("shared object","",\$0); print } { print }'
EOM
chmod +x "$halide_build_root/_shims/file"

# Write Ubuntu-specific config and merge the shared/static build directories.
cat <<'EOM' >ubuntu.cmake
include("shared-Release/CPackConfig.cmake")

## General setup

set(CPACK_PACKAGE_CONTACT "Alex Reinking <alex_reinking@berkeley.edu>")
set(CPACK_STRIP_FILES TRUE)

## Components configuration

# This is a mapping from CPack component names to CMake install() components.
# We use the identity mapping here for simplicity; some advanced configurations
# with GUI installers require these to diverge.
set(CPACK_COMPONENTS_HALIDE_RUNTIME Halide_Runtime)
set(CPACK_COMPONENTS_HALIDE_DEVELOPMENT Halide_Development)
set(CPACK_COMPONENTS_HALIDE_DOCUMENTATION Halide_Documentation)

set(CPACK_COMPONENTS_ALL Halide_Runtime Halide_Development Halide_Documentation)

set(CPACK_INSTALL_CMAKE_PROJECTS
    static-Release Halide ALL /
    shared-Release Halide ALL /
)

## Ubuntu-specific configuration
# We set every variable documented here: https://cmake.org/cmake/help/latest/cpack_gen/deb.html
# even if it's just to the default. That way there are no surprises.

set(CPACK_DEB_COMPONENT_INSTALL YES)

set(CPACK_DEBIAN_HALIDE_RUNTIME_PACKAGE_NAME libHalide${CPACK_PACKAGE_VERSION_MAJOR})
set(CPACK_DEBIAN_HALIDE_DEVELOPMENT_PACKAGE_NAME libHalide${CPACK_PACKAGE_VERSION_MAJOR}-dev)
set(CPACK_DEBIAN_HALIDE_DOCUMENTATION_PACKAGE_NAME libHalide${CPACK_PACKAGE_VERSION_MAJOR}-doc)

set(CPACK_DEBIAN_HALIDE_RUNTIME_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_HALIDE_DEVELOPMENT_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_HALIDE_DOCUMENTATION_FILE_NAME DEB-DEFAULT)

unset(CPACK_DEBIAN_PACKAGE_EPOCH)

set(CPACK_DEBIAN_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION}")

unset(CPACK_DEBIAN_PACKAGE_RELEASE)

unset(CPACK_DEBIAN_PACKAGE_ARCHITECTURE)  # TODO: support packaging 32-bit builds
set(CPACK_DEBIAN_HALIDE_DOCUMENTATION_PACKAGE_ARCHITECTURE all)

set(CPACK_DEBIAN_HALIDE_RUNTIME_PACKAGE_DEPENDS "llvm-11")
set(CPACK_DEBIAN_HALIDE_DEVELOPMENT_PACKAGE_DEPENDS "llvm-11-dev, liblld-11-dev")
set(CPACK_DEBIAN_HALIDE_DOCUMENTATION_PACKAGE_DEPENDS "")

set(CPACK_DEBIAN_ENABLE_COMPONENT_DEPENDS ON)

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")

# These get their values from cpack cpack_add_component
unset(CPACK_DEBIAN_HALIDE_RUNTIME_DESCRIPTION)
unset(CPACK_DEBIAN_HALIDE_DEVELOPMENT_DESCRIPTION)
unset(CPACK_DEBIAN_HALIDE_DOCUMENTATION_DESCRIPTION)

set(CPACK_DEBIAN_HALIDE_RUNTIME_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_HALIDE_DEVELOPMENT_PACKAGE_SECTION devel)
set(CPACK_DEBIAN_HALIDE_DOCUMENTATION_PACKAGE_SECTION doc)

unset(CPACK_DEBIAN_ARCHIVE_TYPE)  # Deprecated: do not use

set(CPACK_DEBIAN_COMPRESSION_TYPE "gzip")

set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")

set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CMAKE_PROJECT_HOMEPAGE_URL}")

set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

unset(CPACK_DEBIAN_PACKAGE_PREDEPENDS)
unset(CPACK_DEBIAN_PACKAGE_ENHANCES)
unset(CPACK_DEBIAN_PACKAGE_BREAKS)
unset(CPACK_DEBIAN_PACKAGE_CONFLICTS)
unset(CPACK_DEBIAN_PACKAGE_PROVIDES)
unset(CPACK_DEBIAN_PACKAGE_REPLACES)
unset(CPACK_DEBIAN_PACKAGE_RECOMMENDS)
unset(CPACK_DEBIAN_PACKAGE_SUGGESTS)

set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS YES)
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS_POLICY "=")

unset(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA)
unset(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION)

unset(CPACK_DEBIAN_PACKAGE_SOURCE)

unset(CPACK_DEBIAN_DEBUGINFO_PACKAGE)

unset(CPACK_DEBIAN_PACKAGE_DEBUG)
EOM

rm -f ./*.deb
rm -rf ./_CPack_Packages

# ensure correct umask is set for creating packages
umask 0022

(
  export PATH="$halide_build_root/_shims:$PATH"
  cpack -G DEB --config ubuntu.cmake
)

echo "Running STRICT lintian checks..."
lintian -F ./*.deb

echo "Running ALL lintian checks..."
lintian ./*.deb
