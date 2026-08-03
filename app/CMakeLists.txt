cmake_minimum_required(VERSION 3.16)
project(KumDBStudio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

enable_testing()

set(KUMDB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(kumdb STATIC
    ${KUMDB_ROOT}/src/types.c
    ${KUMDB_ROOT}/src/error.c
    ${KUMDB_ROOT}/src/record.c
    ${KUMDB_ROOT}/src/lock.c
    ${KUMDB_ROOT}/src/storage.c
    ${KUMDB_ROOT}/src/index.c
    ${KUMDB_ROOT}/src/query.c
    ${KUMDB_ROOT}/src/table.c
    ${KUMDB_ROOT}/src/kumdb.c
    ${KUMDB_ROOT}/src/sql.c
)
target_include_directories(kumdb PUBLIC ${KUMDB_ROOT}/include)
if(UNIX)
    target_compile_definitions(kumdb PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE)
endif()

set(KUMDB_STUDIO_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/RowTableModel.cpp
    src/RowTableModel.h
    src/NewTableDialog.cpp
    src/NewTableDialog.h
    src/QueryPanel.cpp
    src/QueryPanel.h
    src/KumDbHandle.cpp
    src/KumDbHandle.h
    resources/icon.qrc
)

if(WIN32)
    list(APPEND KUMDB_STUDIO_SOURCES resources/win_icon.rc)
endif()

add_executable(kumdb_studio ${KUMDB_STUDIO_SOURCES})

target_link_libraries(kumdb_studio PRIVATE Qt${QT_VERSION_MAJOR}::Widgets kumdb)

if(WIN32)
    set_target_properties(kumdb_studio PROPERTIES WIN32_EXECUTABLE TRUE)
elseif(APPLE)
    set(KUMDB_ICNS ${KUMDB_ROOT}/assets/icon/icon.icns)
    set_source_files_properties(${KUMDB_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(kumdb_studio PRIVATE ${KUMDB_ICNS})
    set_target_properties(kumdb_studio PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE icon.icns
    )
endif()

# Functional check for KumDbHandle's C/C++ interop -- only needs QtCore, so
# it's a much lighter build than the full GUI target.
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
add_executable(kumdb_studio_handle_test
    src/handle_test.cpp
    src/KumDbHandle.cpp
    src/KumDbHandle.h
)
target_link_libraries(kumdb_studio_handle_test PRIVATE Qt${QT_VERSION_MAJOR}::Core kumdb)
add_test(NAME handle_test COMMAND kumdb_studio_handle_test)

# Plain C command-line tools -- no Qt dependency, built here too so the
# "full" install tier (see packaging below) can ship them alongside Studio.
add_executable(kumdb_cli  ${KUMDB_ROOT}/tools/kumdb_cli.c)
add_executable(kumdb_dump ${KUMDB_ROOT}/tools/dump.c)
target_link_libraries(kumdb_cli  PRIVATE kumdb)
target_link_libraries(kumdb_dump PRIVATE kumdb)

# ---------------------------------------------------------------------------
# Packaging: two install tiers via CPack components.
#   easy = kumdb_studio only, for someone who just wants the desktop app.
#   full = easy + the CLI tools + headers/static lib, for embedding KumDB
#          in your own programs.
# Build both with scripts/package.sh (repo root) -- it runs cpack twice,
# once per tier. Bundling the actual Qt runtime into the installer
# (windeployqt / macdeployqt) has to happen on that target OS; this only
# sets up the CPack side, which is platform-generic.
# ---------------------------------------------------------------------------

install(TARGETS kumdb_studio
    RUNTIME DESTINATION bin COMPONENT studio
    BUNDLE  DESTINATION .   COMPONENT studio
)

install(TARGETS kumdb_cli kumdb_dump
    RUNTIME DESTINATION bin
    COMPONENT devtools
)

install(TARGETS kumdb
    ARCHIVE DESTINATION lib
    COMPONENT devtools
)

install(DIRECTORY ${KUMDB_ROOT}/include/
    DESTINATION include/kumdb
    COMPONENT devtools
    FILES_MATCHING PATTERN "*.h"
)

set(CPACK_PACKAGE_NAME "KumDBStudio")
set(CPACK_PACKAGE_VENDOR "KumDB")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "KumDB Studio -- desktop front end for the KumDB embedded database")
set(CPACK_PACKAGE_VERSION_MAJOR 1)
set(CPACK_PACKAGE_VERSION_MINOR 1)
set(CPACK_PACKAGE_VERSION_PATCH 2)
set(CPACK_PACKAGE_INSTALL_DIRECTORY "KumDB Studio")
set(CPACK_RESOURCE_FILE_LICENSE "${KUMDB_ROOT}/LICENSE")

set(CPACK_COMPONENTS_GROUPING ALL_COMPONENTS_IN_ONE)
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_DEB_COMPONENT_INSTALL ON)

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_NSIS_MUI_ICON "${KUMDB_ROOT}/assets/icon/icon.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\kumdb_studio.exe")
    set(CPACK_NSIS_PACKAGE_NAME "KumDB Studio")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "KumDB")
    set(CPACK_DEBIAN_PACKAGE_SECTION "database")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()

include(CPack)

cpack_add_component(studio
    DISPLAY_NAME "KumDB Studio"
    DESCRIPTION "The desktop app -- browse tables, edit cells, run NoSQL or SQL queries."
    REQUIRED
)
cpack_add_component(devtools
    DISPLAY_NAME "Developer tools"
    DESCRIPTION "kumdb_cli, kumdb_dump, and the C headers/static lib for embedding KumDB in your own programs."
    DEPENDS studio
)
