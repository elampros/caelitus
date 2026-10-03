# API documentation of the C++ code (Doxygen, with the doxygen-awesome theme):
#   cmake --build <build-dir> --target docs     ->  <build-dir>/docs/html/index.html
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if (NOT DOXYGEN_FOUND)
    message(STATUS "Doxygen not found: the 'docs' target is unavailable (sudo apt install doxygen graphviz)")
    return()
endif ()

FetchContent_Declare(doxygen_awesome
        URL https://github.com/jothepro/doxygen-awesome-css/archive/refs/tags/v2.3.4.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        SOURCE_SUBDIR no-cmake)  # stylesheet only
FetchContent_MakeAvailable(doxygen_awesome)

set(DOCS_THEME_DIR ${doxygen_awesome_SOURCE_DIR})
set(DOCS_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR})
if (TARGET Doxygen::dot)
    set(DOCS_HAVE_DOT YES)
else ()
    set(DOCS_HAVE_DOT NO)
endif ()
configure_file(Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)

add_custom_target(docs
        COMMAND Doxygen::doxygen ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Generating C++ API documentation in ${CMAKE_CURRENT_BINARY_DIR}/html"
        VERBATIM)
