cmake_minimum_required(VERSION 3.20)
project(sdr_acquisition VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── PkgConfig ─────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

# ── SdrTaskApi ────────────────────────────────────────────────────────────────
# Expected as a sibling directory: ../SdrTaskApi  (same parent as this repo).
# Falls back to a system-installed cmake package.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../SdrTaskApi/CMakeLists.txt")
    message(STATUS "SdrTaskApi: using sibling source at ../SdrTaskApi")
    add_subdirectory(../SdrTaskApi ${CMAKE_BINARY_DIR}/SdrTaskApi)
else()
    find_package(sdr_task_api QUIET)
    if(NOT sdr_task_api_FOUND)
        message(FATAL_ERROR
            "\n[AcquisitionApp] SdrTaskApi not found.\n"
            "  Option 1 — clone as a sibling directory (recommended):\n"
            "    git clone https://github.com/BMichaud7/SdrTaskApi.git "
            "$(dirname $(pwd))/SdrTaskApi\n"
            "  Option 2 — install system-wide and set sdr_task_api_DIR.\n"
            "  Both repos must share the same parent directory for "
            "the sibling path to resolve automatically.\n")
    endif()
    message(STATUS "SdrTaskApi: using installed package ${sdr_task_api_VERSION}")
endif()

# ── TinyXML2 ─────────────────────────────────────────────────────────────────
pkg_check_modules(TINYXML2 QUIET IMPORTED_TARGET tinyxml2)
if(NOT TINYXML2_FOUND)
    message(FATAL_ERROR
        "\n[AcquisitionApp] tinyxml2 not found.\n"
        "  Ubuntu/Debian : sudo apt-get install libtinyxml2-dev\n"
        "  CentOS/RHEL   : sudo dnf install tinyxml2-devel\n")
endif()

# ── FFTW3 single-precision ────────────────────────────────────────────────────
pkg_check_modules(FFTW3F QUIET IMPORTED_TARGET fftw3f)
if(NOT FFTW3F_FOUND)
    message(FATAL_ERROR
        "\n[AcquisitionApp] FFTW3 (single-precision) not found.\n"
        "  Ubuntu/Debian : sudo apt-get install libfftw3-dev\n"
        "  CentOS/RHEL   : sudo dnf install fftw-devel\n")
endif()

# ── fmt ───────────────────────────────────────────────────────────────────────
pkg_check_modules(FMT QUIET IMPORTED_TARGET fmt)
if(NOT FMT_FOUND)
    message(FATAL_ERROR
        "\n[AcquisitionApp] {fmt} not found.\n"
        "  Ubuntu/Debian : sudo apt-get install libfmt-dev\n"
        "  CentOS/RHEL   : sudo dnf install fmt-devel\n")
endif()

# ── spdlog ────────────────────────────────────────────────────────────────────
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    pkg_check_modules(SPDLOG QUIET IMPORTED_TARGET spdlog)
    if(NOT SPDLOG_FOUND)
        message(FATAL_ERROR
            "\n[AcquisitionApp] spdlog not found.\n"
            "  Ubuntu/Debian : sudo apt-get install libspdlog-dev\n"
            "  CentOS/RHEL   : build from source — see Containerfile.test\n")
    endif()
    add_library(spdlog::spdlog INTERFACE IMPORTED)
    target_include_directories(spdlog::spdlog INTERFACE ${SPDLOG_INCLUDE_DIRS})
    target_link_libraries(spdlog::spdlog INTERFACE ${SPDLOG_LIBRARIES})
    target_compile_definitions(spdlog::spdlog INTERFACE SPDLOG_FMT_EXTERNAL)
endif()

# ── Optional — production binary only ─────────────────────────────────────────
# qpid-proton and libpqxx are only needed for the production binary
# (sdr_acquisition). The unit tests compile and run without them.
# pkg-config names vary: Ubuntu="libqpid-proton-cpp", CentOS="qpid-proton-cpp"
# Ubuntu 24.04's libqpid-proton-cpp.pc has broken transitive deps; fall back
# to find_library/find_path if pkg-config fails.
pkg_check_modules(PROTON QUIET IMPORTED_TARGET qpid-proton-cpp)
if(NOT PROTON_FOUND)
    pkg_check_modules(PROTON QUIET IMPORTED_TARGET libqpid-proton-cpp)
endif()
if(NOT PROTON_FOUND)
    find_library(PROTON_CPP_LIB NAMES qpid-proton-cpp)
    find_path(PROTON_INCLUDE_DIR NAMES proton/connection.hpp)
    if(PROTON_CPP_LIB AND PROTON_INCLUDE_DIR)
        add_library(PkgConfig::PROTON INTERFACE IMPORTED)
        set_target_properties(PkgConfig::PROTON PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${PROTON_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES      "${PROTON_CPP_LIB}")
        set(PROTON_FOUND 1)
        message(STATUS "[AcquisitionApp] qpid-proton-cpp found via find_library: ${PROTON_CPP_LIB}")
    else()
        message(STATUS
            "[AcquisitionApp] qpid-proton-cpp not found — "
            "sdr_acquisition binary will be skipped.\n"
            "  Ubuntu/Debian: sudo apt-get install libqpid-proton-cpp12-dev\n"
            "  CentOS/RHEL  : sudo dnf install qpid-proton-cpp-devel")
    endif()
endif()

pkg_check_modules(PQXX QUIET IMPORTED_TARGET libpqxx)
if(NOT PQXX_FOUND)
    message(STATUS
        "[AcquisitionApp] libpqxx not found — "
        "sdr_acquisition binary will be skipped.\n"
        "  Ubuntu/Debian: sudo apt-get install libpqxx-dev\n"
        "  CentOS/RHEL  : sudo dnf install libpqxx-devel")
endif()

# ── Production binary (requires PROTON + PQXX) ────────────────────────────────
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
    message(STATUS
        "[AcquisitionApp] sdr_acquisition binary skipped "
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

# ── Configure summary ─────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "┌─ AcquisitionApp ${PROJECT_VERSION} ─────────────────────────────────┐")
message(STATUS "│  C++ standard : C++20                                     │")
message(STATUS "│  Build type   : ${CMAKE_BUILD_TYPE}                              ")
message(STATUS "│  Dependencies (required for all targets):                 │")
message(STATUS "│    SdrTaskApi    : FOUND (sibling or installed)           │")
message(STATUS "│    tinyxml2      : FOUND ${TINYXML2_VERSION}                       ")
message(STATUS "│    fftw3f        : FOUND ${FFTW3F_VERSION}                         ")
message(STATUS "│    fmt           : FOUND ${FMT_VERSION}                            ")
message(STATUS "│    spdlog        : FOUND                                  │")
message(STATUS "│    SoapySDR      : test-only (required by acq_tests)      │")
message(STATUS "│    googletest    : test-only (system or FetchContent)     │")
message(STATUS "│  Dependencies (required for sdr_acquisition binary only): │")
if(PROTON_FOUND)
    message(STATUS "│    qpid-proton   : FOUND ${PROTON_VERSION}                      ")
else()
    message(STATUS "│    qpid-proton   : NOT FOUND — binary skipped            │")
endif()
if(PQXX_FOUND)
    message(STATUS "│    libpqxx       : FOUND ${PQXX_VERSION}                        ")
else()
    message(STATUS "│    libpqxx       : NOT FOUND — binary skipped            │")
endif()
message(STATUS "└───────────────────────────────────────────────────────────┘")
message(STATUS "")
