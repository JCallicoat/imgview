# imgview

A small, fast image viewer for **close inspection of pixels and metadata**, written in C++17 with **Qt 6 Widgets** (no QML) and **Exiv2**.

It was built for examining images such as Mars rover frames for artifacts like cosmic-ray hits, so by default it shows the image's stored pixel values as they are when zoomed in, and adds a few display-only tools (color inversion, gamma, saturation) that make faint features easier to spot without changing the file.

## Features

- **Zoom and pan** – zoom with the mouse wheel around the pointer, or with the keyboard; drag with the mouse or use the arrow keys to move around a zoomed image.
- **True-pixel zoom** – when zoomed in, every screen pixel is an unmodified copy of an image pixel (nearest neighbour). Smoother **bicubic** and sharper **Lanczos3** modes are also available.
- **Single-pixel panning** – step the view by exactly one image pixel to walk across fine detail.
- **Folder browsing** – step to the previous or next image in the same folder, in natural order (`frame2` before `frame10`), keeping your view settings.
- **color inversion** – turns a faint light streak on a dark background into a dark streak on a light one.
- **Gamma adjustment** – step a gamma curve to stretch contrast in the bright or dark end of the image.
- **Saturation adjustment** – boost color saturation up to 16× to make faint color differences stand out (such as mineral colors on the Moon), or reduce it down to greyscale.
- **Info panel** – file size, dimensions and a table of the EXIF, XMP and IPTC metadata, with selectable values.
- **Save the view** – save exactly what's shown, with zoom and tone adjustments applied, as a lossless PNG.
- **Status in the window title** – file name and position in the folder, resampling mode, tone settings and exact zoom factor.
- **Help panel** (`?`) listing every key with the current view settings, and a status bar with hints for the most used keys and a brief confirmation of each setting change; in fullscreen it also shows the title's file and view info.

## Usage

```bash
imgview path/to/image.jpg
iv path/to/image.jpg        # short alias, installed alongside imgview
imgview path/to/folder      # opens the folder's first image
imgview a.png b.png c.png   # ←/→ step through just these files
iv sol_04*/*.png            # ...or whatever a shell glob matches
imgview                     # opens a file picker
imgview --version           # also --help
```

Started without a path (for example from the application menu, or by opening the app on macOS), imgview shows a file picker; press `O` to open another image at any time. On macOS, double-clicking an image or using **Open With** in Finder works too.

Given a folder, imgview opens its first image in the same natural order that `←` / `→` step through (only that folder, not subfolders); `←` / `→` then browse the rest. If the folder has no images, it says so, and `←` / `→` pick up any images added to it later.

Given **several** paths, `←` / `→` step through exactly those images, in the order given, instead of a folder. That makes a shell glob or a hand-picked set of frames easy to compare. A folder among them contributes its images; paths that don't exist or aren't readable images are skipped with a note on the terminal, and duplicates are dropped. The same happens when several images are opened together from a file manager (Open With on Linux, Open in Finder on macOS). Opening a single image later (`O`, or from the file manager) switches back to browsing its folder.

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
| `←` / `→` (whole image fits in the view) | Previous / next image in the folder |
| `I` | Cycle zoom-in resampling: **nearest** (true pixels, default) → **bicubic** (smooth) → **Lanczos3** (smooth, sharper) |
| `N` | Toggle color inversion |
| `.` | Gamma up: darker, more contrast among bright values |
| `,` | Gamma down: brighter, more contrast among dark values |
| `/` | Reset gamma to 1 (inversion is left as it is) |
| `;` | Saturation up: more vivid colors, up to 1600 % |
| `L` | Saturation down: towards grey, down to 0 % |
| `'` | Reset saturation to 100 % |
| `Backspace` | Reset inversion, gamma and saturation at once |
| `?` | Show / hide the help panel |
| `M` | Show / hide the info panel |
| `O` | Open an image with a file picker |
| `S` | Save the view as shown (PNG, next to the image) |
| `Shift` + `S` | Save the view, choosing where |
| `F` | Toggle fullscreen |
| `Esc` | Close the help panel, or leave fullscreen, or quit |
| `Q` | Quit |

**Help panel:** `?` opens a panel on the left listing every key, with the current resampling mode, inversion, gamma and saturation beside their keys. All keys keep working while it's open, so you can try a key and watch its effect; `?` or `Esc` closes it.

**Status bar:** hints for the most used keys (cut off from the right when the window is narrow, so `help: ?` stays visible); in fullscreen, the title's file and view info on the right (see below). Changing a view setting shows the new value there for two seconds, e.g. `Saturation 336%`, or `Gamma 8.00 (maximum)` at a limit.

All keys except the arrow keys work wherever keyboard focus is, including in the info panel (while you are typing in a metadata cell, keys go into the cell). The arrow keys pan only while the image has focus (click the image to give it focus), so they still move between cells in the metadata table.

**Previous / next image:** `←` and `→` switch images when the whole image fits in the window (at fit-to-window, for example). When the image is wider than the window, they pan as usual. When it's only taller than the window (say a tall image at 1:1), they do nothing, so you can't jump to another image by accident while panning up and down. Press `0` to fit, then `←` / `→` to move on. The folder's list is every file Qt can read, matched by extension (case-insensitively), sorted naturally, and rescanned on every press, so files added or removed meanwhile are picked up. (With several paths given, it's that list instead; listed files deleted meanwhile are skipped.) It wraps around: `→` on the last image goes to the first and `←` on the first goes to the last, with a short message in the status bar when that happens. Interpolation mode, inversion, gamma and saturation stay as they are; zoom resets to fit; the info panel updates for the new file.

### Window title

The title shows the file, its position in the folder, and the current view settings, for example:

```
frame_0012.png (12/48) – Image Viewer [nearest, inverted, γ 1.68, sat 60%, 3.138×]
frame_0013.png (13/48) – Image Viewer [area, fit 0.421×]
```

- **nearest / bicubic / lanczos3 / area** – the resampling actually applied: the selected mode when zoomed in, `area` when zoomed out (where every mode uses the same area-averaging downscale). The help panel (`?`) shows the selected mode.
- **inverted** – shown when color inversion is on.
- **γ** – shown when gamma isn't 1.
- **sat** – shown when saturation isn't 100 %.
- **zoom factor** – image pixels to screen pixels, with three decimals so a near-1 zoom such as `0.998×` can't be mistaken for exactly `1.000×`. `fit` means the image is being fitted to the window.

In fullscreen there's no title bar, so the same information appears on the right of the status bar, e.g. `frame_0012.png (12/48)  [nearest, inverted, γ 1.68, sat 60%, 3.138×]`. If space is short, the key hints are cut off rather than this.

## Viewing true pixels

**Zoomed in (above 100 %)** the view is resampled in one of three ways; `I` cycles through them:

- **Nearest** (default): each screen pixel shows exactly the image pixel under it. No values are blended or invented, so what you see are the stored pixel values. At whole-number zooms each image pixel is an exact square block.
- **Bicubic** (Catmull-Rom): smoother and easier on the eye, but it interpolates between pixels and can add a faint halo along hard edges. Not suitable for judging individual pixel values.
- **Lanczos3** (windowed sinc, three lobes): smooth like bicubic but sharper, keeping more of the fine detail that varies smoothly, such as surface texture in lunar or planetary images. In exchange it rings a little more at hard edges (a faint halo or ripple along, say, a limb against black sky), and it also blends neighbouring pixels, so it hides single-pixel features such as hot pixels. Also not suitable for judging individual pixel values.

**Zoomed out (below 100 %)** the image is shrunk with a smooth, area-averaging filter in all modes, so the title and saved views say `area` rather than the selected mode. (With display scaling, "100 %" means one image pixel per physical screen pixel.)

Only the visible part of the image is resampled, at the display's real pixel resolution, using all CPU cores. Memory use therefore doesn't grow with the zoom level. If a bicubic or Lanczos3 render is too slow to keep up while dragging (very large windows on slower machines), a quick preview is shown during the drag and the full render appears as soon as the view is still.

## Tone adjustments: invert, gamma and saturation

These change only what is displayed, never the file. They are applied after resampling, to what is on screen:

1. **Inversion** (`N`) maps every channel value *v* to 255 − *v*. On color images each color becomes its complement (red becomes cyan); on greyscale images light and dark simply swap.
2. **Gamma** (`.` `,` `/`) applies *out* = 255 · (*in* / 255)^γ. Each key press multiplies or divides γ by 2^(1/4) ≈ 1.19, up to 12 steps either way (γ from 0.13 to 8.00). Black and white always stay black and white, so nothing is clipped.
   - **γ > 1** (`.`) darkens the midtones and stretches differences among bright values. On an inverted dark frame this is what makes a faint streak stand out: a streak at level 240 on a 250 background becomes 157 on 218 at γ 8.
   - **γ < 1** (`,`) does the opposite, stretching differences among dark values. That suits a light feature on a dark background without inversion.
3. **Saturation** (`;` `L` `'`) scales each color's distance from its grey level: *out* = *L* + *s* · (*in* − *L*), where *L* is the pixel's Rec. 709 luma (0.2126 R + 0.7152 G + 0.0722 B) and *s* is the saturation, 100 % by default. It works on the stored values, before the gamma curve; inversion doesn't change the result, as inverting and changing saturation can be done in either order. Greyscale images are unaffected.
   - **Above 100 %** (`;`) colors become more vivid. Each press multiplies *s* by 2^(1/4) ≈ 1.19, so it doubles every 4 presses: 119 %, 141 %, 168 %, 200 %, … 400 %, … 800 %, … up to 1600 % after 16 presses. Large boosts bring out color differences of only a few levels, as in the "mineral moon" images of the lunar surface: a nearly grey (120, 122, 125) becomes a clear blue (90, 122, 170) at 1600 %. They amplify color noise and compression artifacts just as much (JPEG's 8 × 8 blocks and color bleeding at edges show up strongly), so compare against other frames or a less compressed source before trusting a faint color. Channels pushed past 0 or 255 are clipped, which shifts the hue of already strong colors and merges the clipped levels.
   - **Below 100 %** (`L`) colors move towards grey in steps of 10 %, reaching greyscale at 0 % after 10 presses. Nothing is clipped here: pure red (255, 0, 0) at 60 % becomes (175, 22, 22).

With inversion off, γ = 1 and saturation at 100 % the image is drawn untouched, so nearest mode still shows the exact stored values.

Gamma only redistributes the 256 levels that are already there. Strong settings make the stretched end look stepped and merge levels at the other end, and they amplify noise along with real features, so a moderate setting often reads best.

## Saving the view

`S` saves what the image area shows right now – the visible part of the image, with the current resampling, inversion, gamma and saturation applied – as a lossless PNG. No window background or interface is included, and it's saved at the display's resolution, so at 1:1 in nearest mode the file holds the exact (tone-adjusted) pixel values.

- **Where:** next to the image, named after it and the view settings, e.g. `frame_0012_view_x3.000_nearest_inv_g1.68_sat60.png`. An existing file is never overwritten: `-2`, `-3`, … is added instead. If the image's folder isn't writable, the file goes to your Pictures folder. The status bar shows where it was saved, and so does the terminal or log: each save prints a line to standard output, e.g. `imgview: saved view to /data/sol42/frame_0012_view_x3.000_nearest_inv.png (nearest, 3.000x, inverted; source region x=812 y=440 w=300 h=220)`. A failed save is reported on standard error.
- **Save as:** `Shift` + `S` opens a save dialog instead (PNG by default; TIFF and other formats by file extension).
- **Provenance:** the PNG's text fields record the source file, the view settings and which part of the source image is visible, e.g. `nearest, 3.000x, inverted, gamma 1.68, saturation 60%; source region x=812 y=440 w=300 h=220` (shown by tools such as `exiftool` or `pngcheck -t`).

On a HiDPI display the file has the display's real pixel count: at 1:1 with 200 % display scaling, each image pixel becomes a 2 × 2 block.

## Info panel

Press `M` to show the panel beside the image:

- **File size**, for example `184.9 KiB (189345 bytes)`, using binary units (KiB, MiB, GiB = 1024-based).
- **Dimensions** of the original image in pixels.
- **Metadata** read with Exiv2, one tag per row, in **EXIF**, **XMP** and **IPTC** sections (each shown only if the file has it). Values are shown interpreted, as `sRGB` or `inch` rather than `1` or `2`; hover over a value to see the raw stored value when it differs, or over a tag to see its full Exiv2 key (e.g. `Exif.Photo.ColorSpace`). XMP properties nested in structures and arrays are labelled by their own name with its namespace prefix, e.g. `stCamera:Author`, with the full path (`Xmp.photoshop.CameraProfiles[1]/stCamera:Author`) in the tooltip; the structures themselves aren't listed as rows of their own. Tags that only point to other parts of the EXIF data (such as the Exif IFD pointer) are left out, since the tags they point to are listed.

The file size and dimension values can be selected with the mouse and copied. The metadata table cells can be edited in place for convenience, but edits are never written back to the file.

## Install a prebuilt package

Each [GitHub release](https://github.com/JCallicoat/imgview/releases) has packages for:

| System | File | Install |
|---|---|---|
| Ubuntu 24.04 (amd64, arm64) | `imgview_<version>-1~ubuntu24.04_<arch>.deb` | `sudo apt install ./imgview_*.deb` |
| Debian 13 (amd64, arm64) | `imgview_<version>-1~debian13_<arch>.deb` | `sudo apt install ./imgview_*.deb` |
| Fedora, the release current at the time (x86_64, aarch64) | `imgview-<version>-1.fc<NN>.<arch>.rpm` | `sudo dnf install ./imgview-*.rpm` |
| macOS 12 or later (Apple Silicon and Intel) | `imgview-<version>-macos-universal.dmg` | open it and drag imgview to Applications |

Each package only works on the system it was built for, because it uses that system's own Qt and Exiv2 libraries. The `.deb` and `.rpm` also recommend the extra Qt image-format plugins (TIFF, WebP, SVG, …), which are installed along with them by default. The macOS app bundles Qt, including those plugins. `SHA256SUMS` lists every file's checksum.

**macOS: the app isn't signed with an Apple Developer ID**, so the first time you open it, macOS refuses because it can't verify the developer. To allow it once:

- **macOS 15 (Sequoia) and later:** try to open imgview, dismiss the warning, then go to **System Settings → Privacy & Security**, scroll down and click **Open Anyway** next to the imgview message.
- **macOS 14 and earlier:** right-click (or Control-click) imgview in Applications, choose **Open**, and confirm.

Or, on any version, remove the download's quarantine flag in Terminal:

```bash
xattr -dr com.apple.quarantine /Applications/imgview.app
```

## Building

### Requirements

- A C++17 compiler
- **CMake** 3.24 or newer
- **Qt 6** (Core, Gui, Widgets), 6.3 or newer
- **Exiv2** 0.27 or newer, with its CMake package (both the 0.27 and 0.28 APIs are supported)

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

This installs, under `/usr/local` by default:

- `bin/imgview` – the program.
- `bin/iv` – a short alias: a symlink to `imgview` in the same directory.
- `share/man/man1/imgview.1.gz` (and `iv.1.gz`) – the man page.
- `share/licenses/imgview/LICENSE` – the license.
- `share/applications/imgview.desktop` – a desktop entry, so file managers offer imgview under **Open With** for image files. It lists every image type handled by Qt's own image plugins (Qt GUI's built-in formats, Qt Image Formats and Qt SVG), whether or not those plugins are installed on your system: BMP, GIF, JPEG, PNG, PBM/PGM/PPM, XBM, XPM, ICO, SVG/SVGZ, ICNS, JPEG 2000 (JP2/JPX/JPM), TGA, TIFF, WBMP and WebP. Opening a type whose plugin is missing shows a "Failed to load image" message.

Started from the application menu, it opens a file picker. Its `Exec` line uses the full install path, so it works for prefixes that aren't on your `PATH`.

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

This removes exactly the files the last `cmake --install build` put in place (CMake lists them in `build/install_manifest.txt`) and refreshes the **Open With** cache. Use `sudo` if the install did, e.g. for the default `/usr/local` prefix. If the build directory is gone, delete the files listed under Install above by hand.


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

For a new release, the Arch package is updated as part of the steps in [Releasing](#releasing). For packaging-only fixes (dependencies, build steps) without a new release, bump `pkgrel` instead of `pkgver`.

#### Publishing to the AUR

Each AUR package lives in its own git repo, separate from this one. For the release package:

```bash
git clone ssh://aur@aur.archlinux.org/imgview.git ~/aur/imgview      # once
cp arch/PKGBUILD arch/.SRCINFO ~/aur/imgview/
cd ~/aur/imgview && git add PKGBUILD .SRCINFO && git commit -m "Update to 1.1.0" && git push
```

For `imgview-git`, use `~/aur/imgview-git` and `arch/imgview-git/PKGBUILD`, then run `makepkg -o` (fetch and update the version, no build) and `makepkg --printsrcinfo > .SRCINFO` there before committing. Only push it when the PKGBUILD itself changes; users building it always get the latest commit anyway.

### macOS

Building on macOS needs Qt 6 (e.g. from Qt's online installer or Homebrew) and Exiv2 (Homebrew's `exiv2`, or `packaging/macos/build-exiv2.sh <exiv2-source> <prefix>` for a static build). `cmake --build build` then produces `build/imgview.app`. To make a self-contained `.dmg` from it, as releases do: `packaging/macos/make-dmg.sh build/imgview.app imgview.dmg "$(uname -m)"` (the last argument lists the architectures every bundled binary must contain).

### Editor support (clangd)

CMake writes `build/compile_commands.json` (`CMAKE_EXPORT_COMPILE_COMMANDS` is on), and the `.clangd` file in the repository root points clangd at it, so Qt and Exiv2 symbols resolve in editors such as Neovim once the project has been configured into `build/`.

## Releasing

Releases are built by GitHub Actions (`.github/workflows/release.yml`) when a version tag is pushed:

1. Cut the release from an up-to-date, clean `main`, with a configured build directory:

   ```bash
   RELEASE_VERSION=1.2.0 RELEASE_NOTES="Short description" cmake --build build --target release
   ```

   This sets the version in `CMakeLists.txt`, commits it as "Release 1.2.0: Short description", pushes `main`, then creates the annotated tag `v1.2.0` with the description and pushes it. Before changing anything it checks that the version is X.Y.Z and higher than the current one, that the tag doesn't exist yet (locally or on GitHub), that you're on `main` and up to date with it, and that there are no uncommitted changes. Add `RELEASE_DRY_RUN=1` to only run the checks and show what would happen. Without a build directory: `cmake -DRELEASE_VERSION=1.2.0 -DRELEASE_NOTES="…" -P cmake/release.cmake`.

   (By hand, it's: set the version in `CMakeLists.txt`, commit, push `main`, then `git tag -a v1.2.0 -m "…" && git push origin v1.2.0`. The tag must match the CMake version, or the workflow stops with an error.)
2. The workflow builds the Ubuntu, Debian, Fedora and macOS packages and publishes a release with them and a `SHA256SUMS` file, marked as the latest release (shown in the repository's Releases sidebar). Nothing is published if any build fails.
3. Arch package: in `arch/PKGBUILD`, set `pkgver=1.2.0` and reset `pkgrel=1`; in `arch/`, run `updpkgsums` (from `pacman-contrib`) to fetch the new tarball and update `sha256sums`, then `makepkg --printsrcinfo > .SRCINFO`; commit both. This commit comes after the tag, since the checksum needs the tag's tarball; that's normal. Then publish to the AUR as described under [Arch Linux](#arch-linux).

To try the builds without releasing, run the workflow by hand: **Actions → Release → Run workflow**. The packages are then attached to that run instead of a release.

If a build fails after tagging, fix it, then move the tag to the fixed commit (`git tag -fa v1.2.0 -m "imgview 1.2.0" && git push -f origin v1.2.0`) to rebuild; the release's files are replaced in place.

## Known limitations

- **Images are displayed at 8 bits per channel.** 16-bit PNG or TIFF data is reduced to 8 bits when loaded, so the gamma stretch can't recover the extra precision. 8-bit images such as JPEGs lose nothing.
- **Zoomed out, single pixels can disappear.** The area-averaging downscale dilutes an isolated hot pixel, so zoom in (or press `R`) to check individual pixels.
- **Non-whole zoom factors give uneven blocks in nearest mode.** At, say, 3.3× some image pixels are 3 screen pixels wide and some 4. The values are still exact.
- **HiDPI:** 1:1 means one image pixel per *logical* screen pixel. With fractional display scaling (e.g. 125 %) image pixels map to uneven numbers of physical pixels.
- **macOS app is unsigned** (no Apple Developer ID), so macOS asks for confirmation the first time; see [Install a prebuilt package](#install-a-prebuilt-package).
- **Wayland:** some compositors (GNOME in particular) take the window's icon and name from the installed `imgview.desktop`, so a build run from `build/` without installing may show a generic icon there.

## Project layout

```
src/main.cpp                    Entry point: options, file picker, macOS open events
src/ImageViewer.h/.cpp          Main window (info and help panels, keys, title, status bar)
                                and the zoomable, pannable image view
src/Resample.h/.cpp             Nearest, bicubic and Lanczos3 resamplers, tone adjustments (invert/gamma tables, saturation)
data/imgview.desktop.in         Desktop entry template (installed as imgview.desktop)
data/imgview.1                  Man page
packaging/macos/                macOS Info.plist, icon (and its generator), Exiv2
                                build script and .dmg script
.github/workflows/release.yml   Release builds (.deb, .rpm, .dmg)
cmake/cmake_uninstall.cmake.in  Script behind the uninstall target
cmake/release.cmake             Script behind the release target
arch/PKGBUILD, arch/.SRCINFO    Arch Linux package for the latest release (imgview)
arch/imgview-git/               Arch Linux package for the latest commit (imgview-git)
CMakeLists.txt                  Build configuration
LICENSE                         MIT license
.clangd                         Points clangd at build/compile_commands.json
```

## License

MIT; see [LICENSE](LICENSE).
