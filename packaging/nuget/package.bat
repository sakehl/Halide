@echo off

set halide_source=%~1
set halide_build_root=%~2
set halide_arch=%~3

if not exist "%VCPKG_ROOT%\.vcpkg-root" (
    echo Must define VCPKG_ROOT to be the root of the VCPKG install
    goto error
)

if not exist "%LLVM_DIR%\LLVMConfig.cmake" (
    echo Must set specific LLVM_DIR for packaging
    goto error
)

if not exist "%Clang_DIR%\ClangConfig.cmake" (
    echo Must set specific Clang_DIR for packaging
    goto error
)

if "%halide_source%" == "" (
    echo Usage: %~0 "<source-dir>" "<build-dir>" [Win32,x64,ARM,ARM64]
    goto error
)

if "%halide_build_root%" == "" (
    echo Usage: %~0 "<source-dir>" "<build-dir>" [Win32,x64,ARM,ARM64]
    goto error
)

if "%halide_arch%" == "" (
    echo Usage: %~0 "<source-dir>" "<build-dir>" [Win32,x64,ARM,ARM64]
    goto error
)

REM Ninja Multi-Config in 3.18 has some sort of bug. Very disappointing.
cmake -G "Visual Studio 16 2019" -Thost=x64 -A "%halide_arch%" ^
      "-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake" ^
      "-DLLVM_DIR=%LLVM_DIR%" ^
      "-DClang_DIR=%Clang_DIR%" ^
      -DBUILD_SHARED_LIBS=YES ^
      -DWITH_TESTS=NO ^
      -DWITH_TUTORIALS=NO ^
      -DWITH_DOCS=NO ^
      -DWITH_UTILS=NO ^
      -DWITH_PYTHON_BINDINGS=NO ^
      "-DCMAKE_INSTALL_BINDIR=native/bin" ^
      "-DCMAKE_INSTALL_LIBDIR=native/lib" ^
      "-DCMAKE_INSTALL_INCLUDEDIR=native/include" ^
      "-DCMAKE_INSTALL_DATADIR=share/Halide" ^
      "-DHALIDE_INSTALL_CMAKEDIR=share/Halide" ^
      "-DCPACK_NUGET_COMPONENT_INSTALL=OFF" ^
      "-DCPACK_NUGET_PACKAGE_AUTHORS=Andrew Adams, Jonathan Ragan-Kelley, Steven Johnson, Tzu-Mao Li, alexreinking, Volodymyr Kysenko, Benoit Steiner, Dillon Sharlet, Shoaib Kamil, Zalman Stern" ^
      "-DCPACK_NUGET_PACKAGE_TITLE=Halide Compiler and Libraries" ^
      "-DCPACK_NUGET_PACKAGE_OWNERS=alexreinking" ^
      "-DCPACK_NUGET_PACKAGE_LICENSEURL=https://github.com/halide/Halide/blob/master/LICENSE.txt" ^
      "-DCPACK_NUGET_PACKAGE_COPYRIGHT=Copyright (c) 2012-2020 MIT CSAIL, Google, Facebook, Adobe, NVIDIA CORPORATION, and other contributors." ^
      "-DCPACK_NUGET_PACKAGE_TAGS=Halide C++ CUDA OpenCL GPU Performance DSL native" ^
      "-DCPACK_NUGET_PACKAGE_DEPENDENCIES=" ^
      "-DCPACK_NUGET_PACKAGE_DEBUG=OFF" ^
      -S "%halide_source%" ^
      -B "%halide_build_root%"
if ERRORLEVEL 1 goto error

REM We don't distribute Debug binaries because they aren't useful
REM cmake --build %halide_build_root% --config Debug
cmake --build "%halide_build_root%" --config Release
if ERRORLEVEL 1 goto error

pushd "%halide_build_root%"
cpack -G NuGet -C "Release"
if ERRORLEVEL 1 (
    popd
    goto error
)
popd

exit /b

:error
exit /b 1
