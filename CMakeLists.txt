cmake_minimum_required(VERSION 3.16)
project(FileCompressionTool VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ZLIB REQUIRED)

set(COMPRESSION_CORE_SOURCES
    src/zip_archive.cpp
    src/huffman.cpp
)

add_library(compression_core ${COMPRESSION_CORE_SOURCES}
    src/zip_archive.hpp
    src/huffman.hpp
)
target_include_directories(compression_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(compression_core PUBLIC ZLIB::ZLIB)

if (MSVC)
    target_compile_options(compression_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(compression_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(file-compress src/main.cpp)
target_link_libraries(file-compress PRIVATE compression_core)

include(CTest)
if (BUILD_TESTING)
    add_executable(smoke_test tests/smoke_test.cpp)
    target_link_libraries(smoke_test PRIVATE compression_core)

    add_library(compression_core_security_test STATIC ${COMPRESSION_CORE_SOURCES})
    target_include_directories(compression_core_security_test
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(compression_core_security_test PUBLIC ZLIB::ZLIB)
    target_compile_definitions(compression_core_security_test
        PRIVATE ZIPTOOL_ENABLE_TEST_HOOKS)
    if (MSVC)
        target_compile_options(compression_core_security_test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(compression_core_security_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(security_test tests/security_test.cpp)
    target_link_libraries(security_test PRIVATE compression_core_security_test)
    target_compile_definitions(security_test PRIVATE ZIPTOOL_ENABLE_TEST_HOOKS)

    add_executable(performance_test tests/performance_test.cpp)
    target_link_libraries(performance_test PRIVATE compression_core)

    add_test(NAME smoke_test COMMAND smoke_test)
    add_test(NAME SEC-COMP-SYMLINK-FILE COMMAND security_test file-symlink)
    add_test(NAME SEC-COMP-SYMLINK-DIR COMMAND security_test directory-symlink)
    add_test(NAME SEC-EXTRACT-RACE COMMAND security_test extract-race)
    set_tests_properties(
        SEC-COMP-SYMLINK-FILE SEC-COMP-SYMLINK-DIR SEC-EXTRACT-RACE
        PROPERTIES SKIP_RETURN_CODE 77)
endif()
