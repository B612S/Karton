# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 Karton contributors

find_package(Qt6 REQUIRED COMPONENTS Test Xml Qml)

add_executable(karton-xml-tests
    domainxml_test.cpp
    ${CMAKE_BINARY_DIR}/core/karton_debug.cpp
    ${CMAKE_SOURCE_DIR}/core/domainconfig.cpp
    ${CMAKE_SOURCE_DIR}/core/domainxmlbuilder.cpp
    ${CMAKE_SOURCE_DIR}/core/domainxmlreader.cpp
    ${CMAKE_SOURCE_DIR}/core/osinfoconfig.cpp
)

set_target_properties(karton-xml-tests PROPERTIES AUTOMOC ON)

target_include_directories(karton-xml-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_BINARY_DIR}/core
)

target_link_libraries(karton-xml-tests PRIVATE
    Qt6::Core
    Qt6::Xml
    Qt6::Test
    Qt6::Qml
    Libvirt::Libvirt
    Libosinfo::Libosinfo
    Glib::Glib
)

add_test(NAME karton-xml-tests COMMAND karton-xml-tests)

add_executable(karton-commandrunner-tests
    commandrunner_test.cpp
    ${CMAKE_BINARY_DIR}/core/karton_debug.cpp
    ${CMAKE_SOURCE_DIR}/core/commandrunner.cpp
)

set_target_properties(karton-commandrunner-tests PROPERTIES AUTOMOC ON)

target_include_directories(karton-commandrunner-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_BINARY_DIR}/core
)

target_link_libraries(karton-commandrunner-tests PRIVATE
    Qt6::Core
    Qt6::Test
)

add_test(NAME karton-commandrunner-tests COMMAND karton-commandrunner-tests)
