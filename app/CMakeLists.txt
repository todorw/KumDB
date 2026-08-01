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
