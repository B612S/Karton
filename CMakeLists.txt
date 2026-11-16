# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
# SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

cmake_minimum_required(VERSION 3.20)
project(karton VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(KARTON_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(KARTON_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(ECM 6.0.0 REQUIRED NO_MODULE)

set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMFindQmlModule)
include(ECMAddAppIcon)
include(ECMQmlModule)
include(ECMQtDeclareLoggingCategory)
include(KDEClangFormat)
include(KDEGitCommitHooks)
include(ECMSetupVersion)

find_package(Qt6 REQUIRED COMPONENTS
    Core
    Quick
    Gui
    GuiPrivate
    QuickControls2
    Widgets
    Xml
    Multimedia
)

find_package(Libvirt REQUIRED)
find_package(Libosinfo REQUIRED)
find_package(Glib REQUIRED)
find_package(Spiceglib REQUIRED)
find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
find_package(Vulkan REQUIRED)

find_package(KF6 REQUIRED COMPONENTS
    Kirigami
    I18n
    CoreAddons
    QQC2DesktopStyle
    IconThemes
)

find_package(KF6KirigamiAddons 1.11.0 REQUIRED)

install(FILES
    img/karton_logo.svg
    DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps
    RENAME org.kde.karton.svg
)
install(FILES
    org.kde.karton.desktop
    DESTINATION ${KDE_INSTALL_APPDIR}
)

ecm_find_qmlmodule(org.kde.kirigami REQUIRED)

add_definitions(-DTRANSLATION_DOMAIN="karton")

ecm_setup_version(${CMAKE_PROJECT_VERSION}
    VARIABLE_PREFIX KARTON
    VERSION_HEADER core/karton_version.h)

add_executable(karton)

ecm_add_qml_module(karton
    GENERATE_PLUGIN_SOURCE
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/core/org/kde/karton
    URI org.kde.karton
)

target_include_directories(karton PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/app
    ${CMAKE_CURRENT_SOURCE_DIR}/core/domain
    ${CMAKE_CURRENT_SOURCE_DIR}/core/libvirt
    ${CMAKE_CURRENT_SOURCE_DIR}/core/viewer
    ${CMAKE_CURRENT_SOURCE_DIR}/core/os
    ${CMAKE_CURRENT_SOURCE_DIR}/core/util
    ${CMAKE_CURRENT_BINARY_DIR}/core
)

target_sources(karton
    PRIVATE
    core/app/main.cpp
    core/app/karton.cpp
    core/app/vmlistmodel.cpp
    core/domain/domain.cpp
    core/domain/domainconfig.cpp
    core/libvirt/libvirtmonitor.cpp
    core/libvirt/libvirteventloop.cpp
    core/viewer/domainviewer.cpp
    core/viewer/spicedisplayrenderer.h
    core/viewer/glscanoutrenderer.cpp
    core/viewer/primarysurfacerenderer.cpp
    core/viewer/vulkanscanoutrenderer.cpp
    core/viewer/vulkanscanoutrenderer.h
    core/domain/domainxmlbuilder.cpp
    core/os/osinfoconfig.cpp
    core/domain/domainxmlreader.cpp
    core/util/commandrunner.cpp
    core/util/evdev_to_xtkbd_map.c
    core/util/evdev_to_xtkbd_map.h
    core/util/devicepixelratiohelper.cpp
)

ecm_target_qml_sources(karton
    SOURCES
    qml/Main.qml
    qml/VMListPage.qml
    qml/VMViewerWindow.qml
    qml/InstallationDialog.qml
    qml/VMPage.qml
    qml/ConnectionSettingsDialog.qml
    qml/UsbDevicesDialog.qml
    qml/SettingsDialog.qml
    qml/SnapshotsDialog.qml
    qml/CloneDialog.qml
)

ecm_qt_declare_logging_category(
    karton
    HEADER core/karton_debug.h
    IDENTIFIER KARTON_DEBUG
    CATEGORY_NAME org.kde.karton
    DESCRIPTION "Karton Virtual Machine Manager"
    EXPORT Karton
)

ecm_qt_install_logging_categories(
    EXPORT Karton
    FILE karton.categories
    DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR}
)

target_link_libraries(karton
    PRIVATE
    Qt6::Quick
    Qt6::Qml
    Qt6::Gui
    Qt6::QuickControls2
    Qt6::Widgets
    Qt6::Xml
    Qt6::Multimedia
    Qt6::GuiPrivate
    KF6::I18n
    KF6::I18nQml
    KF6::CoreAddons
    KF6::IconThemes
    KirigamiApp
    Libvirt::Libvirt
    Spiceglib::Spiceglib
    Libosinfo::Libosinfo
    Glib::Glib
    OpenGL::EGL
    OpenGL::GL
    Vulkan::Vulkan
)

install(TARGETS karton ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})


# Tests are opt-in and are never part of the application target or package.
option(KARTON_BUILD_TESTS "Build Karton autotests (Qt Test)" OFF)
if (KARTON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.cpp *.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
