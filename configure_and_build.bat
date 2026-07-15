@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo Failed to initialize MSVC environment
    exit /b 1
)

set CC=cl.exe
set CXX=cl.exe

if not exist build mkdir build
cd build

cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCTEE_BUILD_TESTS=ON -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_C_COMPILER=cl.exe
if errorlevel 1 (
    echo CMake configuration failed
    exit /b 1
)

cmake --build .
if errorlevel 1 (
    echo Build failed
    exit /b 1
)

echo Build successful
