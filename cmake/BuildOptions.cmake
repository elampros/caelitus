# Compiler settings shared by every caelitus target (warnings, sanitizers),
# and a helper that declares one module library.

add_library(caelitus_build_options INTERFACE)
target_compile_options(caelitus_build_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
        -Wcast-align -Wimplicit-fallthrough -Wextra-semi -Wsign-conversion)
if (CAELITUS_WARNINGS_AS_ERRORS)
    target_compile_options(caelitus_build_options INTERFACE -Werror)
endif ()

if (CAELITUS_SANITIZER STREQUAL "address")
    set(_sanitize -fsanitize=address,undefined -fno-omit-frame-pointer)
elseif (CAELITUS_SANITIZER STREQUAL "thread")
    set(_sanitize -fsanitize=thread)
    # GCC warns that Asio's atomic_thread_fence is invisible to TSan; harmless here.
    target_compile_options(caelitus_build_options INTERFACE $<$<CXX_COMPILER_ID:GNU>:-Wno-tsan>)
elseif (NOT CAELITUS_SANITIZER STREQUAL "")
    message(FATAL_ERROR "CAELITUS_SANITIZER must be '', 'address' or 'thread', not '${CAELITUS_SANITIZER}'")
endif ()
if (_sanitize)
    target_compile_options(caelitus_build_options INTERFACE ${_sanitize})
    target_link_options(caelitus_build_options INTERFACE ${_sanitize})
endif ()

# caelitus_module(<name> SOURCES ... [PUBLIC <deps>...] [PRIVATE <deps>...])
#
# A static library `caelitus_<name>`. Public headers live in include/caelitus/<name>/
# (consumers write #include "caelitus/<name>/X.hpp"); the module's own .cpp files and
# private headers live in src/<name>/.
function(caelitus_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC;PRIVATE" ${ARGN})
    set(target caelitus_${name})
    if (ARG_SOURCES)
        add_library(${target} STATIC ${ARG_SOURCES})
        set(scope PUBLIC)
        target_include_directories(${target}
                PUBLIC ${PROJECT_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated
                PRIVATE ${PROJECT_SOURCE_DIR}/src)
        target_link_libraries(${target} PRIVATE caelitus_build_options)
    else ()
        add_library(${target} INTERFACE)  # header-only
        set(scope INTERFACE)
        target_include_directories(${target} INTERFACE ${PROJECT_SOURCE_DIR}/include ${CMAKE_BINARY_DIR}/generated)
    endif ()
    if (ARG_PUBLIC)
        target_link_libraries(${target} ${scope} ${ARG_PUBLIC})
    endif ()
    if (ARG_PRIVATE)
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE})
    endif ()
    add_library(caelitus::${name} ALIAS ${target})
endfunction()
