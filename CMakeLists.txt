cmake_minimum_required(VERSION 3.20)
project(sdr_acquisition VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# ── SdrTaskApi (shared task types + codec — sibling repo) ────────────────────
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../SdrTaskApi/CMakeLists.txt")
    add_subdirectory(../SdrTaskApi ${CMAKE_BINARY_DIR}/SdrTaskApi)
else()
    find_package(sdr_task_api REQUIRED)
endif()

# ── Required for all targets (production binary + tests) ──────────────────────
pkg_check_modules(TINYXML2 REQUIRED IMPORTED_TARGET tinyxml2)
pkg_check_modules(FFTW3F   REQUIRED IMPORTED_TARGET fftw3f)
pkg_check_modules(FMT      REQUIRED IMPORTED_TARGET fmt)

# spdlog: prefer cmake package (built from source), fall back to pkg-config
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    pkg_check_modules(SPDLOG REQUIRED IMPORTED_TARGET spdlog)
    add_library(spdlog::spdlog INTERFACE IMPORTED)
    target_include_directories(spdlog::spdlog INTERFACE ${SPDLOG_INCLUDE_DIRS})
    target_link_libraries(spdlog::spdlog INTERFACE ${SPDLOG_LIBRARIES})
    target_compile_definitions(spdlog::spdlog INTERFACE SPDLOG_FMT_EXTERNAL)
endif()

# ── Required only for the production binary ───────────────────────────────────
# SoapySDR is intentionally absent here — it belongs in the test binary only.
pkg_check_modules(PROTON QUIET IMPORTED_TARGET qpid-proton-cpp)
pkg_check_modules(PQXX   QUIET IMPORTED_TARGET libpqxx)

# ── Production binary (requires PROTON + PQXX) ───────────────────────────────
if(PROTON_FOUND AND PQXX_FOUND)
    add_executable(sdr_acquisition
        src/main.cpp
        src/SweepConfig.cpp
        src/FftProcessor.cpp
        src/SpectrumScanner.cpp
        src/TaskManagerIqSource.cpp
        src/DetectionDb.cpp
        src/AmqpPublisher.cpp
    )
    target_include_directories(sdr_acquisition PRIVATE include)
    target_link_libraries(sdr_acquisition PRIVATE
        sdr_task_api
        PkgConfig::PROTON
        PkgConfig::TINYXML2
        PkgConfig::FFTW3F
        PkgConfig::PQXX
        spdlog::spdlog
        PkgConfig::FMT
        Threads::Threads
    )
    target_compile_definitions(sdr_acquisition PRIVATE
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG
        SPDLOG_FMT_EXTERNAL
    )
    install(TARGETS sdr_acquisition DESTINATION bin)
else()
    message(STATUS "sdr_acquisition binary skipped "
        "(PROTON_FOUND=${PROTON_FOUND}, PQXX_FOUND=${PQXX_FOUND})")
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()
add_subdirectory(tests)

# ── Install ───────────────────────────────────────────────────────────────────
install(FILES config/scanner.xml
    DESTINATION etc/sdr-acquisition
    RENAME scanner.xml.example)
install(FILES schema/init.sql DESTINATION share/sdr-acquisition)
