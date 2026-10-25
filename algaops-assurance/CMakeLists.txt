cmake_minimum_required(VERSION 3.16)

project(AlgaOpsAssurance
    VERSION 1.0.0
    DESCRIPTION "Edge Trust-Gated Data Integrity and Equipment Diagnostics for Microalgae PBR"
    LANGUAGES C
)

# ============================================================================
# Compiler configuration
# ============================================================================

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Enable strict warnings (treat warnings as errors in CI; optional in dev).
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wformat=2 -Wshadow -Wconversion
        -Wno-unused-parameter
    )
endif()

# ============================================================================
# Library targets (reusable core — no main() here)
# ============================================================================

# --- CSV Reader (utility) ---
add_library(algaops_csv STATIC
    src/csv_reader.c
)
target_include_directories(algaops_csv PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src   # csv_reader.h lives in src/
)
target_link_libraries(algaops_csv PRIVATE m)

# --- TrustGuard ---
add_library(algaops_trustguard STATIC
    src/trustguard.c
)
target_include_directories(algaops_trustguard PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(algaops_trustguard PRIVATE m)

# --- Equipment Health ---
add_library(algaops_equip STATIC
    src/equipment_health.c
)
target_include_directories(algaops_equip PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src   # needs csv_reader.h
)
target_link_libraries(algaops_equip PRIVATE
    algaops_csv
    algaops_trustguard
    m
)

# ============================================================================
# Main executable
# ============================================================================

add_executable(algaops src/main.c)
target_include_directories(algaops PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(algaops PRIVATE
    algaops_equip
    algaops_trustguard
    algaops_csv
    m
)

# Copy data/ directory to build directory for convenient demo use.
file(COPY ${CMAKE_SOURCE_DIR}/data DESTINATION ${CMAKE_BINARY_DIR})

# ============================================================================
# Tests
# ============================================================================

enable_testing()

# --- TrustGuard unit tests ---
add_executable(test_trustguard tests/test_trustguard.c)
target_include_directories(test_trustguard PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(test_trustguard PRIVATE
    algaops_trustguard
    m
)
add_test(NAME test_trustguard COMMAND test_trustguard)

# --- Equipment Health unit tests ---
add_executable(test_equipment_health tests/test_equipment_health.c)
target_include_directories(test_equipment_health PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_equipment_health PRIVATE
    algaops_equip
    algaops_trustguard
    algaops_csv
    m
)
add_test(NAME test_equipment_health COMMAND test_equipment_health)

# --- Integration tests ---
add_executable(test_integration tests/test_integration.c)
target_include_directories(test_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(test_integration PRIVATE
    algaops_equip
    algaops_trustguard
    algaops_csv
    m
)
add_test(NAME test_integration COMMAND test_integration)

# ============================================================================
# Install rules (optional — for packaging)
# ============================================================================

install(TARGETS algaops DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/algaops)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "AlgaOps Assurance ${PROJECT_VERSION}")
message(STATUS "  C standard : C${CMAKE_C_STANDARD}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler   : ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
