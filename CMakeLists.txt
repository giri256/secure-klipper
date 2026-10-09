# Builds Secure Klipper: libklipper, the org.kde.plasma.private.clipboard QML plugin and the
# autotests, without configuring the rest of plasma-workspace. Mirrors the parts of
# plasma-workspace/CMakeLists.txt that klipper needs. scripts/build.sh builds the tray edition.
cmake_minimum_required(VERSION 3.22)
project(secure-klipper)
set(PROJECT_VERSION "6.7.5")
set(PROJECT_VERSION_MAJOR 6)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQtDeclareLoggingCategory)
include(ECMGenerateExportHeader)
include(GenerateExportHeader)
include(ECMQmlModule)
include(ECMAddTests)
include(FeatureSummary)

find_package(Qt6 6.8 CONFIG REQUIRED COMPONENTS Concurrent DBus Quick Sql Widgets Test)
find_package(Qt6GuiPrivate REQUIRED NO_MODULE)
find_package(Qt6WaylandClientPrivate REQUIRED NO_MODULE)
find_package(KF6 6.0 REQUIRED COMPONENTS
    Config CoreAddons GuiAddons GlobalAccel I18n IconThemes KIO Notifications Prison
    Service WidgetsAddons WindowSystem XmlGui)
find_package(Plasma REQUIRED)
find_package(PlasmaQuick REQUIRED)
find_package(KWayland REQUIRED)
find_package(LayerShellQt CONFIG REQUIRED)
find_package(Wayland REQUIRED COMPONENTS Client)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)

# X11 support, same as the distro build
find_package(XCB MODULE REQUIRED COMPONENTS AUX XCB)
set(HAVE_X11 1)
set(HAVE_XCURSOR 1)
set(HAVE_XFIXES 1)
configure_file(config-X11.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config-X11.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_definitions(-DQT_NO_KEYWORDS)

# BUILD_TESTING comes from KDECMakeSettings (default ON)

add_subdirectory(klipper)
