# caelitus_test(<name> [INTEGRATION] LIBS <libs>...)
#
# One test executable from tests/<name>.cpp, registered with CTest and
# labelled "unit" or "integration":
#   ctest --test-dir <build> -L unit
# Integration tests skip themselves (exit 0) unless their environment
# variables point them at a database / broker; see README.md.
function(caelitus_test name)
    cmake_parse_arguments(ARG "INTEGRATION" "" "LIBS" ${ARGN})
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} caelitus_build_options)
    target_compile_definitions(${name} PRIVATE CAELITUS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
    if (ARG_INTEGRATION)
        set_tests_properties(${name} PROPERTIES LABELS integration)
    else ()
        set_tests_properties(${name} PROPERTIES LABELS unit)
    endif ()
endfunction()

# ---- unit: no external services ----
caelitus_test(datetime_tests LIBS caelitus::db caelitus::json)
caelitus_test(db_tests LIBS caelitus::db_mariadb)
caelitus_test(config_tests LIBS caelitus::config caelitus::json)
caelitus_test(mqtt_tests LIBS caelitus::mqtt)
caelitus_test(tcp_tests LIBS caelitus::net caelitus_asio)
caelitus_test(cache_tests LIBS caelitus::catalog)
caelitus_test(scheduler_tests LIBS caelitus::scheduler)
caelitus_test(catalog_service_tests LIBS caelitus::catalog)
caelitus_test(api_tests LIBS caelitus::api)
caelitus_test(app_tests LIBS caelitus::catalog_mariadb caelitus::json)

# ---- integration: MariaDB and/or an MQTT broker ----
caelitus_test(db_integration_tests INTEGRATION LIBS caelitus::db_mariadb)
caelitus_test(catalog_integration_tests INTEGRATION LIBS caelitus::catalog_mariadb caelitus::db_mariadb)
caelitus_test(mqtt_integration_tests INTEGRATION LIBS caelitus::mqtt_mosquitto)
caelitus_test(app_integration_tests INTEGRATION LIBS caelitus::app)
