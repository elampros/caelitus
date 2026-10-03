# caelitus: book catalog server (JSON-RPC over TCP, MariaDB, MQTT) and its
# command-line client. Build with the presets in CMakePresets.json:
#
#   cmake --preset asan && cmake --build --preset asan     # server, libraries, tests
#   ctest --test-dir build/asan                            # tests
#   cmake --build --preset asan --target docs              # Doxygen API docs
#
# Dependencies: cmake/Dependencies.cmake. Compiler options and the module helper:
# cmake/BuildOptions.cmake. The libraries: src/CMakeLists.txt. See README.md.

cmake_minimum_required(VERSION 3.20)
project(caelitus
        VERSION 1.0.0
        DESCRIPTION "Book catalog server: JSON-RPC over TCP, MariaDB, MQTT"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---- options -------------------------------------------------------------------
set(CAELITUS_SANITIZER "" CACHE STRING "Build with a sanitizer: '', 'address' (ASan+UBSan) or 'thread' (TSan)")
set_property(CACHE CAELITUS_SANITIZER PROPERTY STRINGS "" address thread)
option(CAELITUS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(BUILD_TESTING "Build the tests" ON)

include(cmake/Dependencies.cmake)
include(cmake/BuildOptions.cmake)

# Version header generated from the project() version.
configure_file(cmake/Version.hpp.in ${CMAKE_BINARY_DIR}/generated/caelitus/Version.hpp @ONLY)

add_subdirectory(src)

if (BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif ()

add_subdirectory(docs)
