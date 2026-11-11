# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.16)
project(qqmem VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "qqmem currently supports Linux only")
endif()

add_executable(qqmem src/qqmem.c)
set_target_properties(qqmem PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(qqmem PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS qqmem RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/qqmem)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
    add_test(NAME cli
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.py
                $<TARGET_FILE:qqmem>
    )
endif()
