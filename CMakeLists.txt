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
install(TARGETS imgview RUNTIME DESTINATION bin)
