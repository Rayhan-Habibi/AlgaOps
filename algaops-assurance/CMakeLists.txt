cmake_minimum_required(VERSION 3.16)

project(AlgaOpsAssurance
    VERSION 2.0.0
    DESCRIPTION "Edge Trust-Gated Data Integrity and Equipment Diagnostics for Microalgae PBR"
    LANGUAGES C
)

# ============================================================================
# Compiler configuration
# ============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(ALGAOPS_WERROR "Treat compiler warnings as errors (CI)" OFF)
option(ALGAOPS_BUILD_TESTS "Build the CTest suites" ON)
option(ALGAOPS_BUILD_SHARED "Build the shared library used by the gateway (ctypes)" ON)

# Enable strict warnings (treat warnings as errors in CI; optional in dev).
if(MSVC)
    add_compile_options(/W4)
    if(ALGAOPS_WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wshadow -Wconversion
        -Wno-unused-parameter
    )
    if(ALGAOPS_WERROR)
        add_compile_options(-Werror)
    endif()
endif()

set(ALGAOPS_MATH_LIB "")
if(NOT MSVC)
    set(ALGAOPS_MATH_LIB m)
endif()

# ============================================================================
# Core sources (portable C11, no external dependencies)
# ============================================================================

set(ALGAOPS_CORE_SOURCES
    src/csv_reader.c
    src/trustguard.c
    src/equipment_health.c
    src/features.c
    src/window_policy.c
    src/cross_sensor.c
    src/operating_mode.c
    src/baseline.c
    src/health_methods.c
    src/health_fixed_threshold.c
    src/health_mahalanobis.c
    src/linalg_small.c
    src/algaops_engine.c
    src/ascon_hash256.c
    src/audit_chain.c
)

# --- Static core library (CLI, tests, other C consumers) ---
add_library(algaops_core STATIC ${ALGAOPS_CORE_SOURCES})
target_include_directories(algaops_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src   # csv_reader.h / linalg_small.h live in src/
)
target_link_libraries(algaops_core PUBLIC ${ALGAOPS_MATH_LIB})

# --- Shared library for the gateway (Python ctypes) ---
if(ALGAOPS_BUILD_SHARED)
    add_library(algaops_shared SHARED ${ALGAOPS_CORE_SOURCES})
    target_include_directories(algaops_shared PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(algaops_shared PRIVATE ALGAOPS_BUILD_SHARED)
    target_link_libraries(algaops_shared PRIVATE ${ALGAOPS_MATH_LIB})
    set_target_properties(algaops_shared PROPERTIES
        OUTPUT_NAME algaops
        PREFIX ""                      # algaops.dll / algaops.so on every platform
        C_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
    )
    if(NOT WIN32)
        set_target_properties(algaops_shared PROPERTIES PREFIX "lib")   # libalgaops.so
    endif()
endif()

# ============================================================================
# Main executable
# ============================================================================

add_executable(algaops src/main.c)
target_link_libraries(algaops PRIVATE algaops_core)

# Copy data/ directory to build directory for convenient demo use.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

# ============================================================================
# Tests
# ============================================================================

if(ALGAOPS_BUILD_TESTS)
    enable_testing()

    set(ALGAOPS_TESTS
        test_trustguard          # original v1 suite (10 tests)
        test_equipment_health    # original v1 suite (8 tests)
        test_integration         # original v1 suite (6 tests)
        test_csv_reader
        test_window_policy
        test_operating_mode
        test_cross_sensor
        test_features
        test_baseline
        test_health_methods
        test_engine
        test_ascon_hash256
        test_audit_chain
    )
    foreach(t ${ALGAOPS_TESTS})
        add_executable(${t} tests/${t}.c)
        target_link_libraries(${t} PRIVATE algaops_core)
        target_compile_definitions(${t} PRIVATE
            ALGAOPS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endforeach()

    # CLI smoke tests: legacy invocation, policy run, evidence round trip.
    add_test(NAME cli_legacy_equipment_anomaly
        COMMAND algaops --baseline data/baseline_normal.csv
                        --input data/scenario_equipment_anomaly.csv --quiet)
    add_test(NAME cli_evidence_export
        COMMAND algaops --baseline data/baseline_normal.csv
                        --input data/scenario_stuck_sensor.csv --quiet
                        --evidence evidence_cli_test --data-source synthetic)
    add_test(NAME cli_evidence_verify
        COMMAND algaops --verify-evidence evidence_cli_test)
    set_tests_properties(cli_evidence_verify PROPERTIES DEPENDS cli_evidence_export)
    set_tests_properties(cli_legacy_equipment_anomaly cli_evidence_export cli_evidence_verify
        PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()

# ============================================================================
# Install rules (optional — for packaging)
# ============================================================================

install(TARGETS algaops DESTINATION bin)
if(ALGAOPS_BUILD_SHARED)
    install(TARGETS algaops_shared LIBRARY DESTINATION lib RUNTIME DESTINATION lib)
endif()
install(DIRECTORY include/ DESTINATION include/algaops)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "AlgaOps Assurance ${PROJECT_VERSION}")
message(STATUS "  C standard : C${CMAKE_C_STANDARD}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
