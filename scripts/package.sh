#!/usr/bin/env bash
# Builds KumDB Studio and packages it into two installers:
#   easy  = kumdb_studio only
#   full  = kumdb_studio + kumdb_cli + kumdb_dump + C headers/static lib
#
# Run natively on each target OS -- this doesn't cross-compile Qt, and it
# doesn't bundle the Qt runtime for you on Windows/macOS (that needs
# windeployqt / macdeployqt, which only exist on those platforms; see
# DOCUMENTATION.md).
set -euo pipefail

cd "$(dirname "$0")/../app"
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

cd build
version="1.1.0"  # keep in sync with CPACK_PACKAGE_VERSION_* in CMakeLists.txt

echo "==> packaging easy tier (Studio only)"
cpack -D CPACK_COMPONENTS_ALL=studio \
      -D "CPACK_PACKAGE_FILE_NAME=KumDBStudio-${version}-Linux-easy"

echo "==> packaging full tier (Studio + CLI tools + dev headers/lib)"
cpack -D CPACK_COMPONENTS_ALL="studio;devtools" \
      -D "CPACK_PACKAGE_FILE_NAME=KumDBStudio-${version}-Linux-full"

echo "==> done, artifacts in app/build/"
ls -1 KumDBStudio-*
