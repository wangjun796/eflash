@echo off
setlocal

echo Building Dhara project for Visual Studio 2022...

:: Check if cmake is installed
where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: CMake is not installed or not in PATH
    exit /b 1
)

:: Check if build directory exists, create if not
if not exist "build_vs2022" mkdir build_vs2022

:: Enter build directory
cd build_vs2022

:: Use CMake to generate Visual Studio 2022 solution
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_PLATFORM=x64

if %errorlevel% neq 0 (
    echo Error: CMake configuration failed
    exit /b 1
)

:: Build the project
cmake --build . --config Release

if %errorlevel% neq 0 (
    echo Error: Build failed
    exit /b 1
)

echo Build completed successfully!
echo To open the solution, navigate to the build_vs2022 folder and open dhara.sln
echo To run tests, execute: cmake --build . --config Release --target RUN_TESTS
cmake --build . --config Release --target RUN_TESTS