# Dhara - Visual Studio 2022 Build Instructions

This document explains how to build the Dhara project using Visual Studio 2022 and CMake.

## Prerequisites

- Visual Studio 2022 (with C++ development tools)
- CMake 3.15 or higher
- Git Bash (optional, for running build script)

## Building with the Batch Script

We provide a batch script to automate the build process:

1. Run `build_vs2022.bat` from the project root directory
2. The script will:
   - Create a `build_vs2022` directory
   - Generate Visual Studio 2022 solution files
   - Build the project in Release mode

## Manual Build Process

Alternatively, you can build manually:

1. Create a build directory:
   ```cmd
   mkdir build_vs2022
   cd build_vs2022
   ```

2. Configure with CMake:
   ```cmd
   cmake .. -G "Visual Studio 17 2022" -A x64
   ```

3. Build the project:
   ```cmd
   cmake --build . --config Release
   ```

## Generated Files

The build process creates:
- A Visual Studio 2022 solution file (`dhara.sln`)
- Multiple test executables with `.test` extension
- Tool executables (`gftool.exe`, `gentab.exe`)
- Static libraries (`dhara_lib.lib`, `ecc_lib.lib`)

## Running Tests

To run all tests:
```cmd
cmake --build . --config Release --target RUN_TESTS
```

Or to run specific tests, you can build and run the individual test targets:
```cmd
ctest -C Release -V
```

## Project Structure

The CMake build system creates the following targets:
- `dhara_lib`: Core library with NAND management functions
- `ecc_lib`: Error correction code library
- Test executables: Individual test programs
- Tool executables: Utility programs for GF tables and other tasks

## Notes

- The build configuration replicates the original Makefile behavior
- Debug builds include debug symbols (-ggdb equivalent)
- Warning level is set to /W4 for MSVC (equivalent to -Wall for GCC)
- Static linking is used to avoid runtime dependencies