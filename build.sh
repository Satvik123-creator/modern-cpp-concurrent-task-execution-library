#!/bin/bash
set -e

BUILD_DIR="build"
BUILD_TYPE="Release"

case "${1:-build}" in
    clean)
        echo "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        echo "Done."
        ;;
    debug)
        BUILD_TYPE="Debug"
        ;& 
    release|"")
        echo "Building $BUILD_TYPE..."
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
        cmake --build . --config "$BUILD_TYPE"
        cd ..
        echo "Build complete."
        ;;
    test)
        echo "Running tests..."
        mkdir -p "$BUILD_DIR"
        cd "$BUILD_DIR"
        cmake .. -DCMAKE_BUILD_TYPE=Debug -DCTEE_BUILD_TESTS=ON
        cmake --build . --config Debug
        ctest --output-on-failure
        cd ..
        ;;
    *)
        echo "Usage: $0 {clean|debug|release|test}"
        exit 1
        ;;
esac
