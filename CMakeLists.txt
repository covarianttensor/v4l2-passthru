cmake_minimum_required(VERSION 3.21)
project(v4l2-passthru VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick Qml Multimedia QuickControls2)
find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

qt_add_executable(v4l2-passthru
    src/main.cpp
    src/DeviceDiscovery.h
    src/DeviceDiscovery.cpp
    src/CaptureController.h
    src/CaptureController.cpp
)

target_include_directories(v4l2-passthru PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

qt_add_qml_module(v4l2-passthru
    URI V4L2Passthru
    VERSION 1.0
    QML_FILES
        qml/Main.qml
    RESOURCES
        assets/v4l2-passthru.svg
)

target_link_libraries(v4l2-passthru PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::Qml
    Qt6::Multimedia
    Qt6::QuickControls2
    PkgConfig::GSTREAMER
    PkgConfig::UDEV
    PkgConfig::ALSA
)

install(TARGETS v4l2-passthru
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

install(FILES assets/v4l2-passthru.svg
    DESTINATION share/icons/hicolor/scalable/apps
)

install(FILES assets/v4l2-passthru.desktop
    DESTINATION share/applications
)
