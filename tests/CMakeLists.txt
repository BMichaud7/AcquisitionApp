cmake_minimum_required(VERSION 3.20)

# ── Google Test ───────────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found via find_package; fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# SoapySDR — used only by the test binary (SoapyIqSource + FakeAcqSoapyDevice)
find_package(SoapySDR REQUIRED)

# spdlog: handle cmake package vs pkg-config
if(TARGET spdlog::spdlog)
    set(_SPDLOG_LIBS spdlog::spdlog)
    set(_SPDLOG_DEFS SPDLOG_FMT_EXTERNAL)
    set(_SPDLOG_INCS "")
else()
    set(_SPDLOG_LIBS ${SPDLOG_LIBRARIES})
    set(_SPDLOG_DEFS "")
    set(_SPDLOG_INCS ${SPDLOG_INCLUDE_DIRS})
endif()

# ── Test binary ───────────────────────────────────────────────────────────────
# Sources: FftProcessor + SweepConfig + SpectrumScanner (no TaskManagerIqSource,
# no DetectionDb, no AmqpPublisher — none of those are under test here).
add_executable(acq_tests
    fake_soapy/FakeAcqSoapyDevice.cpp
    unit/test_fft_processor.cpp
    unit/test_sweep_config.cpp
    unit/test_spectrum_scanner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FftProcessor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/SweepConfig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/SpectrumScanner.cpp
)

target_include_directories(acq_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/fake_soapy
    ${CMAKE_CURRENT_SOURCE_DIR}/soapy_source
    ${SoapySDR_INCLUDE_DIRS}
    ${_SPDLOG_INCS}
)

target_link_libraries(acq_tests PRIVATE
    GTest::gtest_main
    SoapySDR
    PkgConfig::FFTW3F
    PkgConfig::TINYXML2
    ${_SPDLOG_LIBS}
    PkgConfig::FMT
    Threads::Threads
)

target_compile_definitions(acq_tests PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN
    ${_SPDLOG_DEFS}
)

include(CTest)
include(GoogleTest)
gtest_discover_tests(acq_tests)

# ── Benchmark binary ──────────────────────────────────────────────────────────
# Not registered with CTest; run manually: build/tests/acq_bench
add_executable(acq_bench
    bench/bench_fft_processor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FftProcessor.cpp
)
target_include_directories(acq_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${_SPDLOG_INCS}
)
target_link_libraries(acq_bench PRIVATE
    PkgConfig::FFTW3F
    ${_SPDLOG_LIBS}
    Threads::Threads
)
target_compile_definitions(acq_bench PRIVATE
    SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF
    ${_SPDLOG_DEFS}
)
