@echo off
setlocal

set BUILD_DIR=build
set BUILD_TYPE=Release

if "%1"=="" goto build
if "%1"=="clean" goto clean
if "%1"=="debug" set BUILD_TYPE=Debug
if "%1"=="release" set BUILD_TYPE=Release
if "%1"=="test" goto test
goto build

:clean
echo Cleaning build directory...
if exist %BUILD_DIR% rmdir /s /q %BUILD_DIR%
echo Done.
goto end

:build
echo Building %BUILD_TYPE%...
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %BUILD_DIR%
cmake .. -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
cmake --build . --config %BUILD_TYPE%
cd ..
echo Build complete.
goto end

:test
echo Running tests...
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
cd %BUILD_DIR%
cmake .. -DCMAKE_BUILD_TYPE=Debug -DCTEE_BUILD_TESTS=ON
cmake --build . --config Debug
ctest --output-on-failure
cd ..
goto end

:end
endlocal
