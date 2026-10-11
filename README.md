# Image Viewer

This project is a simple image viewer written in C++ using **Qt6**.

* Displays an image and scales it to the available window size while keeping its aspect ratio
* Supports zooming in/out with the mouse wheel or **+ / - / 0** keys
* Shows an information panel (toggled with the **M** key) that contains file size, image dimensions, and EXIF data

## Build Requirements

* **Qt6** (Core, Gui, Widgets) – use the system-installed libraries. The project uses CMake 3.24+.
* **Exiv2** – library for reading EXIF metadata (must be installed on the system).

## Building

```bash
# From the repository root
mkdir build
cd build
cmake ..
cmake --build .
```

`imgview` will be produced in the build directory.

## Usage

```bash
./imgview path/to/image.jpg
```

Press **M** to toggle the info panel. Use the mouse wheel or **+ / - / 0** to zoom.
