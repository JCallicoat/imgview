cmake_minimum_required(VERSION 3.24)
project(imgview LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Qt6 modules
find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)
qt_standard_project_setup()

# std::thread for the image resampler
find_package(Threads REQUIRED)

# Find Exiv2 for EXIF data
find_package(exiv2 REQUIRED CONFIG NAMES exiv2)

add_executable(imgview
    src/main.cpp
    src/ImageViewer.cpp
    src/Resample.cpp
)

target_include_directories(imgview PRIVATE src)

# Link Qt6 libraries
target_link_libraries(imgview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Threads::Threads)

# Link Exiv2
target_link_libraries(imgview PRIVATE Exiv2::exiv2lib)

# Enable C++17
set_target_properties(imgview PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Installation (optional)
include(GNUInstallDirs)
install(TARGETS imgview RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Desktop entry, so file managers offer imgview under "Open With". Exec uses
# the full install path so it works for prefixes that aren't on PATH.
set(IMGVIEW_INSTALL_BINARY "${CMAKE_INSTALL_FULL_BINDIR}/imgview")
configure_file(data/imgview.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/imgview.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/imgview.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# `cmake --build build --target uninstall` removes everything the last
# `cmake --install build` installed (listed in install_manifest.txt).
configure_file(cmake/cmake_uninstall.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Uninstalling imgview")
