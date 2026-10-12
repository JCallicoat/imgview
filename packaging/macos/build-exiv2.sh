#!/usr/bin/env bash
# Build Exiv2 as a static library for the macOS app bundle.
#
#   build-exiv2.sh <exiv2-source-dir> <install-prefix>
#
# CMAKE_OSX_ARCHITECTURES / CMAKE_OSX_DEPLOYMENT_TARGET are taken from the
# environment (the release workflow sets "arm64;x86_64" for a universal
# build). Only what imgview uses is enabled: no command-line tool, network
# access, translations, inih or Brotli, so the result depends only on zlib,
# expat and iconv, which macOS provides.
set -euo pipefail

src=${1:?usage: build-exiv2.sh <exiv2-source-dir> <install-prefix>}
prefix=${2:?usage: build-exiv2.sh <exiv2-source-dir> <install-prefix>}
build="${src}/build-imgview"

cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  ${CMAKE_OSX_ARCHITECTURES:+-DCMAKE_OSX_ARCHITECTURES="$CMAKE_OSX_ARCHITECTURES"} \
  ${CMAKE_OSX_DEPLOYMENT_TARGET:+-DCMAKE_OSX_DEPLOYMENT_TARGET="$CMAKE_OSX_DEPLOYMENT_TARGET"} \
  -DBUILD_SHARED_LIBS=OFF \
  -DEXIV2_BUILD_EXIV2_COMMAND=OFF \
  -DEXIV2_BUILD_SAMPLES=OFF \
  -DEXIV2_BUILD_UNIT_TESTS=OFF \
  -DEXIV2_BUILD_DOC=OFF \
  -DEXIV2_ENABLE_NLS=OFF \
  -DEXIV2_ENABLE_WEBREADY=OFF \
  -DEXIV2_ENABLE_CURL=OFF \
  -DEXIV2_ENABLE_INIH=OFF \
  -DEXIV2_ENABLE_BROTLI=OFF
cmake --build "$build"
cmake --install "$build"
