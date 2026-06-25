cmake_minimum_required(VERSION 3.10)
project(DGLS VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Include paths ──
include_directories(
    geo/include
    geo/frustum
    diamond/include
    diamond/hamburger
    diamond/gpx
    diamond/hbv_bundle
    bond/include
)

# ── Source files ──
set(SOURCES
    geo/src/geo_jump.c
    bond/src/pogls_bond_export.c
)

# ── Optional: find zstd for GPX5 ──
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(ZSTD libzstd)
endif()

# ── Test executable ──
add_executable(test_pipeline test_pipeline.c ${SOURCES})
target_link_libraries(test_pipeline PRIVATE m)
if(ZSTD_FOUND)
    target_include_directories(test_pipeline PRIVATE ${ZSTD_INCLUDE_DIRS})
    target_link_libraries(test_pipeline PRIVATE ${ZSTD_LIBRARIES})
else()
    target_compile_definitions(test_pipeline PRIVATE NO_ZSTD=1)
endif()

# ── DRam benchmark (Windows only) ──
if(WIN32)
    add_executable(bench_dram_real bench_dram_real.c)
    target_link_libraries(bench_dram_real PRIVATE)
endif()

# ── DRam POC ──
add_executable(poc_dram_tile poc_dram_tile.c)

# ── Test target ──
enable_testing()
add_test(NAME pipeline COMMAND test_pipeline)
