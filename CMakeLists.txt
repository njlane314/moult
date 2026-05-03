cmake_minimum_required(VERSION 3.20)
project(moult-core VERSION 0.1.0 LANGUAGES CXX)

option(MOULT_CORE_BUILD_TESTS "Build moult-core tests" ON)
option(MOULT_CORE_BUILD_EXAMPLES "Build moult-core examples" ON)
option(MOULT_BUILD_CLI "Build moult command-line tool" ON)
option(MOULT_BUILD_CLANG_ADAPTER "Build libclang-based C++ adapter" ON)

if (MOULT_BUILD_CLANG_ADAPTER)
    find_library(MOULT_LIBCLANG_LIBRARY
        NAMES clang libclang
        PATHS
            /Library/Developer/CommandLineTools/usr/lib
            /Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib
            /opt/homebrew/opt/llvm/lib
            /usr/local/opt/llvm/lib
    )
    if (NOT MOULT_LIBCLANG_LIBRARY)
        message(WARNING "libclang was not found; disabling MOULT_BUILD_CLANG_ADAPTER")
        set(MOULT_BUILD_CLANG_ADAPTER OFF)
    endif()
endif()

add_library(moult_core STATIC
    src/capsule.cpp
    src/diagnostics.cpp
    src/edits.cpp
    src/engine.cpp
    src/evidence.cpp
    src/facts.cpp
    src/json.cpp
    src/plan.cpp
    src/serializers.cpp
    src/source.cpp
    src/types.cpp
)

add_library(moult::core ALIAS moult_core)
set_target_properties(moult_core PROPERTIES EXPORT_NAME core)

target_compile_features(moult_core PUBLIC cxx_std_20)
target_include_directories(moult_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if (MSVC)
    target_compile_options(moult_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(moult_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(moult_cpp_modernization STATIC
    src/cpp/modernization.cpp
)
add_library(moult::cpp_modernization ALIAS moult_cpp_modernization)
set_target_properties(moult_cpp_modernization PROPERTIES EXPORT_NAME cpp_modernization)
target_compile_features(moult_cpp_modernization PUBLIC cxx_std_20)
target_include_directories(moult_cpp_modernization
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(moult_cpp_modernization PUBLIC moult::core)
if (MSVC)
    target_compile_options(moult_cpp_modernization PRIVATE /W4 /permissive-)
else()
    target_compile_options(moult_cpp_modernization PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (MOULT_BUILD_CLANG_ADAPTER)
    add_library(moult_clang_adapter STATIC
        src/clang/adapter.cpp
    )
    add_library(moult::clang_adapter ALIAS moult_clang_adapter)
    set_target_properties(moult_clang_adapter PROPERTIES EXPORT_NAME clang_adapter)
    target_compile_features(moult_clang_adapter PUBLIC cxx_std_20)
    target_include_directories(moult_clang_adapter
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(moult_clang_adapter PUBLIC moult::core PRIVATE ${MOULT_LIBCLANG_LIBRARY})
    if (MSVC)
        target_compile_options(moult_clang_adapter PRIVATE /W4 /permissive-)
    else()
        target_compile_options(moult_clang_adapter PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if (MOULT_BUILD_CLI)
    add_executable(moult apps/moult.cpp)
    target_link_libraries(moult PRIVATE moult::cpp_modernization)
    if (MOULT_BUILD_CLANG_ADAPTER)
        target_compile_definitions(moult PRIVATE MOULT_HAVE_CLANG_ADAPTER=1)
        target_link_libraries(moult PRIVATE moult::clang_adapter)
    endif()
endif()

if (MOULT_CORE_BUILD_TESTS)
    enable_testing()
    add_executable(moult_core_tests tests/test_core.cpp)
    target_link_libraries(moult_core_tests PRIVATE moult::core)
    add_test(NAME moult_core_tests COMMAND moult_core_tests)

    add_executable(moult_cpp_modernization_tests tests/test_cpp_modernization.cpp)
    target_link_libraries(moult_cpp_modernization_tests PRIVATE moult::cpp_modernization)
    add_test(NAME moult_cpp_modernization_tests COMMAND moult_cpp_modernization_tests)

    if (MOULT_BUILD_CLANG_ADAPTER)
        add_executable(moult_clang_adapter_tests tests/test_clang_adapter.cpp)
        target_link_libraries(moult_clang_adapter_tests PRIVATE moult::clang_adapter moult::cpp_modernization)
        add_test(NAME moult_clang_adapter_tests COMMAND moult_clang_adapter_tests)
    endif()

    if (MOULT_BUILD_CLI)
        add_test(NAME moult_cli_cpp_modernization
            COMMAND moult plan --include-facts ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/legacy_modernization.cpp
        )
        if (MOULT_BUILD_CLANG_ADAPTER)
            add_test(NAME moult_cli_clang_cpp_modernization
                COMMAND moult plan --adapter clang --include-facts ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/legacy_modernization.cpp
            )
        endif()
    endif()
endif()

if (MOULT_CORE_BUILD_EXAMPLES)
    add_executable(moult_minimal_engine examples/minimal_engine.cpp)
    target_link_libraries(moult_minimal_engine PRIVATE moult::core)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/moult-core-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/moult-core-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moult-core
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/moult-core-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

set(MOULT_INSTALL_TARGETS moult_core moult_cpp_modernization)
if (MOULT_BUILD_CLANG_ADAPTER)
    list(APPEND MOULT_INSTALL_TARGETS moult_clang_adapter)
endif()

install(TARGETS ${MOULT_INSTALL_TARGETS} EXPORT moultCoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if (MOULT_BUILD_CLI)
    install(TARGETS moult RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT moultCoreTargets
    FILE moultCoreTargets.cmake
    NAMESPACE moult::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moult-core
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/moult-core-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/moult-core-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/moult-core
)
