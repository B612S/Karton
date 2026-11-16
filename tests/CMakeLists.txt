# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Karton contributors

# Autotests are opt-in (KARTON_BUILD_TESTS=ON). They are separate executables,
# link against a test-only static library and are never installed with Karton.
find_package(Qt6 REQUIRED COMPONENTS Test Xml Qml)

add_library(karton_testcore STATIC
    ${CMAKE_BINARY_DIR}/core/karton_debug.cpp
    ${CMAKE_SOURCE_DIR}/core/domain/domainconfig.cpp
    ${CMAKE_SOURCE_DIR}/core/domain/domainxmlbuilder.cpp
    ${CMAKE_SOURCE_DIR}/core/domain/domainxmlreader.cpp
    ${CMAKE_SOURCE_DIR}/core/os/osiconmapper.cpp
    ${CMAKE_SOURCE_DIR}/core/os/osinfoconfig.cpp
    ${CMAKE_SOURCE_DIR}/core/util/commandrunner.cpp
)
set_target_properties(karton_testcore PROPERTIES AUTOMOC ON)

target_include_directories(karton_testcore PUBLIC
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_BINARY_DIR}/core
)

target_link_libraries(karton_testcore PUBLIC
    Qt6::Core
    Qt6::Xml
    Qt6::Qml
    Libvirt::Libvirt
    Libosinfo::Libosinfo
    Glib::Glib
)

# A private tree for QStandardPaths; tests must not read or write the user's
# real Karton/libvirt configuration.
file(MAKE_DIRECTORY
    ${CMAKE_CURRENT_BINARY_DIR}/env/config
    ${CMAKE_CURRENT_BINARY_DIR}/env/cache
    ${CMAKE_CURRENT_BINARY_DIR}/env/state
)

set(KARTON_TEST_ENV
    "QT_QPA_PLATFORM=offscreen"
    "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/env/config"
    "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/env/cache"
    "XDG_STATE_HOME=${CMAKE_CURRENT_BINARY_DIR}/env/state"
)

set(KARTON_TESTS
    tst_domainxml
    tst_domainconfig
    tst_osiconmapper
    tst_commandrunner
)

foreach(test_name IN LISTS KARTON_TESTS)
    add_executable(${test_name} ${test_name}.cpp)
    target_link_libraries(${test_name} PRIVATE karton_testcore Qt6::Test)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "${KARTON_TEST_ENV}"
        TIMEOUT 60
        LABELS "unit"
    )
endforeach()
