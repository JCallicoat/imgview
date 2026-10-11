# imgview

A small, fast image viewer for **close inspection of pixels and metadata**, written in C++17 with **Qt 6 Widgets** (no QML) and **Exiv2**.

It was built for examining images such as Mars rover frames for artifacts like cosmic-ray hits, so by default it shows the image's stored pixel values as they are when zoomed in, and adds a few display-only tools (color inversion, gamma) that make faint features easier to spot without changing the file.

## Features

- **Zoom and pan** – zoom with the mouse wheel around the pointer, or with the keyboard; drag with the mouse or use the arrow keys to move around a zoomed image.
- **True-pixel zoom** – when zoomed in, every screen pixel is an unmodified copy of an image pixel (nearest neighbour). A smoother **bicubic** mode is also available.
- **Single-pixel panning** – step the view by exactly one image pixel to walk across fine detail.
- **Folder browsing** – step to the previous or next image in the same folder, in natural order (`frame2` before `frame10`), keeping your view settings.
- **color inversion** – turns a faint light streak on a dark background into a dark streak on a light one.
- **Gamma adjustment** – step a gamma curve to stretch contrast in the bright or dark end of the image.
- **Info panel** – file size, dimensions and the full EXIF data table, with selectable values.
- **Status in the window title** – file name and position in the folder, resampling mode, tone settings and exact zoom factor.
- **Key hints** in a status bar at the bottom of the window; in fullscreen it also shows the title's file and view info.

## Usage

```bash
imgview path/to/image.jpg
iv path/to/image.jpg        # short alias, installed alongside imgview
imgview path/to/folder      # opens the folder's first image
```

Given a folder, imgview opens its first image in the same natural order that `←` / `→` step through (only that folder, not subfolders); `←` / `→` then browse the rest. If the folder has no images, it says so, and `←` / `→` pick up any images added to it later.

Any format Qt can read (JPEG, PNG, TIFF, BMP, … depending on the installed Qt image plugins) can be opened.

### Keys and mouse

| Input | Action |
|---|---|
| Mouse wheel | Zoom in / out, keeping the point under the pointer fixed |
| `+` (or `=`) / `-` | Zoom in / out around the centre of the view |
| `0` | Fit the whole image to the window |
| `R` | Zoom to exactly 1:1 (one image pixel per screen pixel), centred on the pointer if it's over the image |
| Left-drag | Pan (when the image is larger than the window) |
| `←` `↑` `→` `↓` | Pan by 10 % of the view (when the image view has focus) |
| `Shift` + arrows | Pan by exactly one image pixel |
| `←` / `→` (nothing to pan sideways) | Previous / next image in the folder |
| `I` | Toggle zoom-in resampling: **nearest** (true pixels, default) ⇄ **bicubic** (smooth) |
| `N` | Toggle color inversion |
| `.` | Gamma up: darker, more contrast among bright values |
| `,` | Gamma down: brighter, more contrast among dark values |
| `/` | Reset gamma to 1 (inversion is left as it is) |
| `M` | Show / hide the info panel |
| `F` | Toggle fullscreen (the status bar then shows the title's info) |
| `Esc` | Leave fullscreen; outside fullscreen, quit |
| `Q` | Quit |

All keys except the arrow keys work wherever keyboard focus is, including in the info panel (while you are typing in an EXIF cell, keys go into the cell). The arrow keys pan only while the image has focus (click the image to give it focus), so they still move between cells in the EXIF table.

**Previous / next image:** `←` and `→` switch images whenever they have nothing to pan, i.e. when the image isn't wider than the window (at fit-to-window, or a tall image at 1:1). When zoomed in, they pan as usual; press `0` to fit, then `←` / `→` to move on. The folder's list is every file Qt can read, matched by extension (case-insensitively), sorted naturally, and rescanned on every press, so files added or removed meanwhile are picked up. It wraps around: `→` on the last image goes to the first and `←` on the first goes to the last, with a short message in the status bar when that happens. Interpolation mode, inversion and gamma stay as they are; zoom resets to fit; the info panel updates for the new file.

### Window title

The title shows the file, its position in the folder, and the current view settings, for example:

```
frame_0012.png (12/48) – Image Viewer [nearest, inverted, γ 1.68, 3.138×]
frame_0013.png (13/48) – Image Viewer [bicubic, fit 0.421×]
```

- **nearest / bicubic** – how the image is resampled when zoomed in.
- **inverted** – shown when color inversion is on.
- **γ** – shown when gamma isn't 1.
- **zoom factor** – image pixels to screen pixels, with three decimals so a near-1 zoom such as `0.998×` can't be mistaken for exactly `1.000×`. `fit` means the image is being fitted to the window.

In fullscreen there's no title bar, so the same information appears on the right of the status bar, e.g. `frame_0012.png (12/48)  [nearest, inverted, γ 1.68, 3.138×]`. If space is short, the key hints are cut off rather than this.

## Viewing true pixels

**Zoomed in (above 100 %)** the view is resampled in one of two ways, toggled with `I`:

- **Nearest** (default): each screen pixel shows exactly the image pixel under it. No values are blended or invented, so what you see are the stored pixel values. At whole-number zooms each image pixel is an exact square block.
- **Bicubic** (Catmull-Rom): smoother and easier on the eye, but it interpolates between pixels and can add a faint halo along hard edges. Not suitable for judging individual pixel values.

**Zoomed out (below 100 %)** the image is shrunk with a smooth, area-averaging filter in both modes.

Only the visible part of the image is resampled, at the display's real pixel resolution, using all CPU cores. Memory use therefore doesn't grow with the zoom level. If a bicubic render is too slow to keep up while dragging (very large windows on slower machines), a quick preview is shown during the drag and the full render appears as soon as the view is still.

## Tone adjustments: invert and gamma

These change only what is displayed, never the file. They are applied after resampling, to what is on screen:

1. **Inversion** (`N`) maps every channel value *v* to 255 − *v*. On color images each color becomes its complement (red becomes cyan); on greyscale images light and dark simply swap.
2. **Gamma** (`.` `,` `/`) applies *out* = 255 · (*in* / 255)^γ. Each key press multiplies or divides γ by 2^(1/4) ≈ 1.19, up to 12 steps either way (γ from 0.13 to 8.00). Black and white always stay black and white, so nothing is clipped.
   - **γ > 1** (`.`) darkens the midtones and stretches differences among bright values. On an inverted dark frame this is what makes a faint streak stand out: a streak at level 240 on a 250 background becomes 157 on 218 at γ 8.
   - **γ < 1** (`,`) does the opposite, stretching differences among dark values. That suits a light feature on a dark background without inversion.

With inversion off and γ = 1 the image is drawn untouched, so nearest mode still shows the exact stored values.

Gamma only redistributes the 256 levels that are already there. Strong settings make the stretched end look stepped and merge levels at the other end, and they amplify noise along with real features, so a moderate setting often reads best.

## Info panel

Press `M` to show the panel beside the image:

- **File size**, for example `184.9 KiB (189345 bytes)`, using binary units (KiB, MiB, GiB = 1024-based).
- **Dimensions** of the original image in pixels.
- **EXIF data** read with Exiv2, one tag per row.

The file size and dimension values can be selected with the mouse and copied. The EXIF table cells can be edited in place for convenience, but edits are never written back to the file.

## Building

### Requirements

- A C++17 compiler
- **CMake** 3.24 or newer
- **Qt 6** (Core, Gui, Widgets), 6.3 or newer
- **Exiv2** 0.28 or newer, with its CMake package (the code uses the 0.28 API)

On Linux, Qt's SVG icon plugin (usually packaged as `qt6-svg` or `qt6-qtsvg`) is needed if your icon theme uses SVG icons, which most do. The app takes its icon, `image-x-generic`, from the current icon theme.

### Build

```bash
git clone https://github.com/JCallicoat/imgview
cd imgview
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This produces `build/imgview`. A Release build is recommended because resampling runs noticeably faster with optimizations.

### Install

```bash
cmake --install build
```

This installs three files, under `/usr/local` by default:

- `bin/imgview` – the program.
- `bin/iv` – a short alias: a symlink to `imgview` in the same directory.
- `share/applications/imgview.desktop` – a desktop entry, so file managers offer imgview under **Open With** for image files. It lists every image type handled by Qt's own image plugins (Qt GUI's built-in formats, Qt Image Formats and Qt SVG), whether or not those plugins are installed on your system: BMP, GIF, JPEG, PNG, PBM/PGM/PPM, XBM, XPM, ICO, SVG/SVGZ, ICNS, JPEG 2000 (JP2/JPX/JPM), TGA, TIFF, WBMP and WebP. Opening a type whose plugin is missing shows a "Failed to load image" message.

The entry is hidden from application menus (`NoDisplay=true`) because imgview needs a file to open; it still appears in **Open With**. Its `Exec` line uses the full install path, so it works for prefixes that aren't on your `PATH`.

To install just for your user instead:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build
cmake --install build
update-desktop-database ~/.local/share/applications   # refresh Open With lists
```

Most desktops pick up new entries automatically; if imgview doesn't appear in **Open With**, run `update-desktop-database` on the `applications` directory as shown above.

### Uninstall

From the repository root, using the same build directory you installed from:

```bash
cmake --build build --target uninstall
```

This removes exactly the files the last `cmake --install build` put in place (CMake lists them in `build/install_manifest.txt`) and refreshes the **Open With** cache. Use `sudo` if the install did, e.g. for the default `/usr/local` prefix. If the build directory is gone, delete `bin/imgview`, `bin/iv` and `share/applications/imgview.desktop` under the install prefix by hand.


### Arch Linux

The `arch/` directory has two AUR-style packages. Both install to the system directories (`/usr/bin`, `/usr/share/applications`, `/usr/share/licenses`) and conflict with each other, so install one or the other:

| Directory | Package | Builds |
|---|---|---|
| `arch/` | `imgview` | the latest release tag (e.g. `v1.0.0`) |
| `arch/imgview-git/` | `imgview-git` | the latest commit on `main` |

```bash
cd arch                 # or: cd arch/imgview-git
makepkg -si
```

Remove it again with `sudo pacman -R imgview` (or `imgview-git`). `qt6-imageformats` (TIFF, WebP, …) and `qt6-svg` are optional dependencies for the extra formats.

**Version churn in `imgview-git`:** makepkg writes the version it computes from git (e.g. `1.0.0.r2.gc0ffee1`) back into `arch/imgview-git/PKGBUILD`. Don't commit that change here; the version moves with every commit, so it never settles. Discard it with `git restore arch/imgview-git/PKGBUILD`, or build from a copy (`cp -r arch/imgview-git /tmp/ && cd /tmp/imgview-git && makepkg -si`). The release package has no such churn: its version only changes when you release.

#### Releasing a new version

1. Tag and push the release: `git tag -a v1.1.0 -m "imgview 1.1.0" && git push origin v1.1.0`.
2. In `arch/PKGBUILD`, set `pkgver=1.1.0` and reset `pkgrel=1`.
3. In `arch/`, run `updpkgsums` (from `pacman-contrib`) to download the new tarball and update `sha256sums`.
4. Regenerate the metadata: `makepkg --printsrcinfo > .SRCINFO`.
5. Commit `arch/PKGBUILD` and `arch/.SRCINFO`. This commit comes after the tag, since the checksum needs the tag's tarball; that's normal.

For packaging-only fixes (dependencies, build steps) without a new release, bump `pkgrel` instead of `pkgver`.

#### Publishing to the AUR

Each AUR package lives in its own git repo, separate from this one. For the release package:

```bash
git clone ssh://aur@aur.archlinux.org/imgview.git ~/aur/imgview      # once
cp arch/PKGBUILD arch/.SRCINFO ~/aur/imgview/
cd ~/aur/imgview && git add PKGBUILD .SRCINFO && git commit -m "Update to 1.1.0" && git push
```

For `imgview-git`, use `~/aur/imgview-git` and `arch/imgview-git/PKGBUILD`, then run `makepkg -o` (fetch and update the version, no build) and `makepkg --printsrcinfo > .SRCINFO` there before committing. Only push it when the PKGBUILD itself changes; users building it always get the latest commit anyway.

### Editor support (clangd)

CMake writes `build/compile_commands.json` (`CMAKE_EXPORT_COMPILE_COMMANDS` is on), and the `.clangd` file in the repository root points clangd at it, so Qt and Exiv2 symbols resolve in editors such as Neovim once the project has been configured into `build/`.

## Known limitations

- **Images are displayed at 8 bits per channel.** 16-bit PNG or TIFF data is reduced to 8 bits when loaded, so the gamma stretch can't recover the extra precision. 8-bit images such as JPEGs lose nothing.
- **Zoomed out, single pixels can disappear.** The area-averaging downscale dilutes an isolated hot pixel, so zoom in (or press `R`) to check individual pixels.
- **Non-whole zoom factors give uneven blocks in nearest mode.** At, say, 3.3× some image pixels are 3 screen pixels wide and some 4. The values are still exact.
- **HiDPI:** 1:1 means one image pixel per *logical* screen pixel. With fractional display scaling (e.g. 125 %) image pixels map to uneven numbers of physical pixels.
- **Wayland:** some compositors (GNOME in particular) take the window's icon and name from the installed `imgview.desktop`, so a build run from `build/` without installing may show a generic icon there.

## Project layout

```
src/main.cpp                    Application entry point, Exiv2 init, app icon
src/ImageViewer.h/.cpp          Main window (info panel, keys, title, status bar)
                                and the zoomable, pannable image view
src/Resample.h/.cpp             Nearest and bicubic resamplers, tone (invert/gamma) tables
data/imgview.desktop.in         Desktop entry template (installed as imgview.desktop)
cmake/cmake_uninstall.cmake.in  Script behind the uninstall target
arch/PKGBUILD, arch/.SRCINFO    Arch Linux package for the latest release (imgview)
arch/imgview-git/               Arch Linux package for the latest commit (imgview-git)
CMakeLists.txt                  Build configuration
LICENSE                         MIT license
.clangd                         Points clangd at build/compile_commands.json
```

## License

MIT; see [LICENSE](LICENSE).
