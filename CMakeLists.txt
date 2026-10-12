cmake_minimum_required(VERSION 3.24)
# The version must match the release tag (vX.Y.Z); the release workflow
# checks this.
project(imgview VERSION 1.1.1
        DESCRIPTION "Image viewer for close inspection of pixels and EXIF metadata"
        HOMEPAGE_URL "https://github.com/JCallicoat/imgview"
        LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Qt6 modules
find_package(Qt6 COMPONENTS Core Gui Widgets REQUIRED)
qt_standard_project_setup()

# std::thread for the image resampler
find_package(Threads REQUIRED)

# Find Exiv2 for EXIF data
find_package(exiv2 REQUIRED CONFIG NAMES exiv2)

set(IMGVIEW_SOURCES
    src/main.cpp
    src/ImageViewer.cpp
    src/Resample.cpp
)

if(APPLE)
  # macOS: an imgview.app bundle with its own icon and an Info.plist that
  # registers it as a viewer for images (and folders) in Finder.
  set(IMGVIEW_ICON ${CMAKE_SOURCE_DIR}/packaging/macos/imgview.icns)
  set_source_files_properties(${IMGVIEW_ICON} PROPERTIES
      MACOSX_PACKAGE_LOCATION Resources)
  add_executable(imgview MACOSX_BUNDLE ${IMGVIEW_SOURCES} ${IMGVIEW_ICON})
  if(CMAKE_OSX_DEPLOYMENT_TARGET)
    set(IMGVIEW_MACOS_MIN ${CMAKE_OSX_DEPLOYMENT_TARGET})
  else()
    set(IMGVIEW_MACOS_MIN 12.0) # Qt 6.8's minimum
  endif()
  set_target_properties(imgview PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/packaging/macos/Info.plist.in
      MACOSX_BUNDLE_GUI_IDENTIFIER io.github.jcallicoat.imgview
      MACOSX_BUNDLE_BUNDLE_NAME imgview
      MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
      MACOSX_BUNDLE_ICON_FILE imgview.icns
      MACOSX_BUNDLE_COPYRIGHT "Copyright (c) 2026 Jordan Callicoat")
else()
  add_executable(imgview ${IMGVIEW_SOURCES})
endif()

target_include_directories(imgview PRIVATE src)

# Link Qt6 libraries
target_link_libraries(imgview PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Threads::Threads)

# Link Exiv2. 0.28 exports Exiv2::exiv2lib; 0.27 (Ubuntu 24.04, Debian 12)
# exports a plain exiv2lib target.
if(TARGET Exiv2::exiv2lib)
  target_link_libraries(imgview PRIVATE Exiv2::exiv2lib)
else()
  target_link_libraries(imgview PRIVATE exiv2lib)
endif()

target_compile_definitions(imgview PRIVATE
    IMGVIEW_VERSION="${PROJECT_VERSION}")

# Enable C++17
set_target_properties(imgview PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# Installation (optional)
include(GNUInstallDirs)
install(TARGETS imgview
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        BUNDLE DESTINATION .)

# Packaging format the build is for, set by the release workflow: DEB or RPM
# (empty for a plain `cmake --install`). Selects the CPack generator and the
# license file location each format expects.
set(IMGVIEW_PACKAGE_FORMAT "" CACHE STRING "Package format: DEB, RPM or empty")
# Package release, e.g. "1~ubuntu24.04" for DEB or "1" for RPM (the RPM
# generator appends the distro's dist tag, e.g. ".fc44").
set(IMGVIEW_PACKAGE_RELEASE "1" CACHE STRING "Package release number/suffix")

if(NOT APPLE)
  # Short alias: `iv` -> `imgview`, a relative symlink next to the binary.
  # It's made in the build directory and installed like a normal file, so it
  # lands in install_manifest.txt and the uninstall target removes it too.
  file(CREATE_LINK imgview ${CMAKE_CURRENT_BINARY_DIR}/iv SYMBOLIC)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/iv
          DESTINATION ${CMAKE_INSTALL_BINDIR})

  # Desktop entry, so file managers offer imgview under "Open With". Exec
  # uses the full install path so it works for prefixes that aren't on PATH.
  set(IMGVIEW_INSTALL_BINARY "${CMAKE_INSTALL_FULL_BINDIR}/imgview")
  configure_file(data/imgview.desktop.in
                 ${CMAKE_CURRENT_BINARY_DIR}/imgview.desktop @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/imgview.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

  # Man page (gzipped, as packages expect), with iv(1) linking to it.
  file(ARCHIVE_CREATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/imgview.1.gz
       PATHS ${CMAKE_SOURCE_DIR}/data/imgview.1 FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
  file(CREATE_LINK imgview.1.gz ${CMAKE_CURRENT_BINARY_DIR}/iv.1.gz SYMBOLIC)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/imgview.1.gz
                ${CMAKE_CURRENT_BINARY_DIR}/iv.1.gz
          DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

  # License: Debian expects /usr/share/doc/<package>/copyright; Fedora and
  # Arch use /usr/share/licenses/<package>/.
  if(IMGVIEW_PACKAGE_FORMAT STREQUAL "DEB")
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR} RENAME copyright)
    # Debian packages must carry a changelog; a one-entry one pointing at
    # the GitHub release notes. The date honours SOURCE_DATE_EPOCH.
    string(TIMESTAMP IMGVIEW_CHANGELOG_DATE "%a, %d %b %Y %H:%M:%S +0000" UTC)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian
"imgview (${PROJECT_VERSION}-${IMGVIEW_PACKAGE_RELEASE}) unstable; urgency=medium

  * Release ${PROJECT_VERSION}; see
    ${PROJECT_HOMEPAGE_URL}/releases

 -- Jordan Callicoat <jordan.callicoat@gmail.com>  ${IMGVIEW_CHANGELOG_DATE}
")
    file(ARCHIVE_CREATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.gz
         PATHS ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian
         FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/changelog.Debian.gz
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
  else()
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/imgview)
  endif()
endif()

# Cut a release (bump the version, commit, tag, push):
#   RELEASE_VERSION=1.2.0 RELEASE_NOTES="Short description" \
#     cmake --build build --target release
# See cmake/release.cmake for details and a dry-run option.
add_custom_target(release
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/release.cmake
    USES_TERMINAL
    COMMENT "Cutting an imgview release")

# `cmake --build build --target uninstall` removes everything the last
# `cmake --install build` installed (listed in install_manifest.txt).
configure_file(cmake/cmake_uninstall.cmake.in
               ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    COMMENT "Uninstalling imgview")

# Linux packages via CPack: `cpack` in the build directory. Configure with
# -DIMGVIEW_PACKAGE_FORMAT=DEB or RPM and -DCMAKE_INSTALL_PREFIX=/usr.
# (The macOS .dmg is made by packaging/macos/make-dmg.sh instead, since the
# bundle must first be filled with Qt by macdeployqt.)
if(IMGVIEW_PACKAGE_FORMAT)
  set(CPACK_GENERATOR ${IMGVIEW_PACKAGE_FORMAT})
  set(CPACK_PACKAGE_NAME imgview)
  set(CPACK_PACKAGE_VENDOR "Jordan Callicoat")
  set(CPACK_PACKAGE_CONTACT "Jordan Callicoat <jordan.callicoat@gmail.com>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
  string(CONCAT CPACK_PACKAGE_DESCRIPTION
      "imgview shows images with true-pixel (nearest neighbour) zoom,\n"
      "panning, color inversion and gamma adjustment for spotting faint\n"
      "details, and an info panel with the file's EXIF metadata.")
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
  set(CPACK_STRIP_FILES ON)

  # .deb: dependencies are worked out from the binary by dpkg-shlibdeps.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION graphics)
  set(CPACK_DEBIAN_PACKAGE_RELEASE ${IMGVIEW_PACKAGE_RELEASE})
  # TIFF/WebP/TGA/ICNS/WBMP/JPEG 2000 and SVG support. Debian may ship the
  # SVG plugin separately; Ubuntu 24.04 has it in libqt6svg6.
  set(CPACK_DEBIAN_PACKAGE_RECOMMENDS
      "qt6-image-formats-plugins, qt6-svg-plugins | libqt6svg6")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

  # .rpm: dependencies are worked out by rpmbuild's automatic requires.
  set(CPACK_RPM_PACKAGE_LICENSE MIT)
  set(CPACK_RPM_PACKAGE_URL ${PROJECT_HOMEPAGE_URL})
  set(CPACK_RPM_PACKAGE_RELEASE ${IMGVIEW_PACKAGE_RELEASE})
  set(CPACK_RPM_PACKAGE_RELEASE_DIST ON) # appends e.g. ".fc44"
  set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
  # CPack has no setting for Recommends (installed by default by dnf, unlike
  # Suggests), so add it to the generated spec directly.
  set(CPACK_RPM_SPEC_MORE_DEFINE
      "Recommends: qt6-qtimageformats\nRecommends: qt6-qtsvg")
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  # Directories owned by Fedora's base "filesystem" package; don't claim them.
  set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
      /usr/share/applications /usr/share/licenses
      /usr/share/man /usr/share/man/man1)

  include(CPack)
endif()
