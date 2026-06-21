cmake_minimum_required(VERSION 3.16)
project(adaedit VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets PrintSupport)

# QScintilla ships no CMake config on Debian; locate it by hand.
# Headers live in .../qt5/Qsci/, the lib is libqscintilla2_qt5.
find_path(QSCINTILLA_INCLUDE_DIR
    NAMES Qsci/qsciscintilla.h
    PATH_SUFFIXES qt5
    HINTS /usr/include /usr/include/x86_64-linux-gnu/qt5)

find_library(QSCINTILLA_LIBRARY
    NAMES qscintilla2_qt5 qt5scintilla2
    HINTS /usr/lib /usr/lib/x86_64-linux-gnu)

if(NOT QSCINTILLA_INCLUDE_DIR OR NOT QSCINTILLA_LIBRARY)
    message(FATAL_ERROR
        "QScintilla (Qt5) not found. Install it with:\n"
        "    sudo apt install libqscintilla2-qt5-dev")
endif()

add_executable(adaedit
    src/main.cpp
    src/mainwindow.cpp
    src/adalexer.cpp
    src/project.cpp
    src/debugger.cpp
    src/lspclient.cpp
)

target_include_directories(adaedit PRIVATE ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(adaedit PRIVATE
    Qt5::Widgets
    Qt5::PrintSupport
    ${QSCINTILLA_LIBRARY}
)
