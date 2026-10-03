# Third-party dependencies. System packages are preferred; spdlog,
# nlohmann/json and Asio fall back to a pinned download when missing.

include(FetchContent)
find_package(Threads REQUIRED)

# spdlog (libspdlog-dev)
find_package(spdlog 1.12 QUIET)
if (NOT spdlog_FOUND)
    FetchContent_Declare(spdlog
            URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(spdlog)
endif ()

# nlohmann/json (nlohmann-json3-dev)
find_package(nlohmann_json 3.11 QUIET)
if (NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
    FetchContent_MakeAvailable(nlohmann_json)
endif ()

# Asio, standalone and header-only (libasio-dev)
find_path(ASIO_INCLUDE_DIR asio.hpp)
if (NOT ASIO_INCLUDE_DIR)
    FetchContent_Declare(asio
            URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-28-1.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP ON
            SOURCE_SUBDIR no-cmake)  # header-only: download, don't configure
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif ()
add_library(caelitus_asio INTERFACE)
target_include_directories(caelitus_asio SYSTEM INTERFACE ${ASIO_INCLUDE_DIR})
target_compile_definitions(caelitus_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(caelitus_asio INTERFACE Threads::Threads)

# MariaDB Connector/C++ (the mariadb-connector-cpp package from MariaDB, or
# built from source, which installs into <prefix>/lib/mariadb/)
find_path(MARIADBCPP_INCLUDE_DIR mariadb/conncpp.hpp REQUIRED)
find_library(MARIADBCPP_LIBRARY mariadbcpp PATH_SUFFIXES mariadb REQUIRED)

# libmosquitto (libmosquitto-dev)
find_path(MOSQUITTO_INCLUDE_DIR mosquitto.h REQUIRED)
find_library(MOSQUITTO_LIBRARY mosquitto REQUIRED)
