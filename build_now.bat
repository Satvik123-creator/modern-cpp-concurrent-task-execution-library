@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
where cl.exe
set CC=cl.exe
set CXX=cl.exe
cd /d "C:\Users\Satvik\OneDrive\Desktop\Concurrent_Task_Execution_Engine"
if exist build rmdir /s /q build
mkdir build
cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCTEE_BUILD_TESTS=ON -DCMAKE_CXX_COMPILER=cl.exe -DCMAKE_C_COMPILER=cl.exe
if errorlevel 1 goto :fail
cmake --build .
if errorlevel 1 goto :fail
echo BUILD SUCCESSFUL
goto :end
:fail
echo BUILD FAILED
:end
