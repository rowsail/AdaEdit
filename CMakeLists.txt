cmake_minimum_required(VERSION 3.16)
project(adaedit VERSION 0.1.5 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets PrintSupport SerialPort)

# QScintilla ships no upstream CMake config, so locate it by hand. To work on
# any distro / Homebrew (not just Debian multiarch), ask Qt's own qmake where its
# headers + libs live and search there, plus the usual prefixes. Override with
# -DQSCINTILLA_ROOT=<prefix> (expects <prefix>/include/Qsci and <prefix>/lib).
if(TARGET Qt5::qmake)
    get_target_property(_QMAKE Qt5::qmake IMPORTED_LOCATION)
    execute_process(COMMAND "${_QMAKE}" -query QT_INSTALL_HEADERS
        OUTPUT_VARIABLE _QT_HEADERS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${_QMAKE}" -query QT_INSTALL_LIBS
        OUTPUT_VARIABLE _QT_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

find_path(QSCINTILLA_INCLUDE_DIR
    NAMES Qsci/qsciscintilla.h
    HINTS "${QSCINTILLA_ROOT}/include" "${_QT_HEADERS}" ENV QSCINTILLA_ROOT
    PATHS /usr/include "/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}"   # Debian multiarch
          /usr/local/include /opt/homebrew/include
          /opt/homebrew/opt/qscintilla2/include /usr/local/opt/qscintilla2/include
    PATH_SUFFIXES qt5 qt)

find_library(QSCINTILLA_LIBRARY
    NAMES qscintilla2_qt5 qt5scintilla2 qscintilla2
    HINTS "${QSCINTILLA_ROOT}/lib" "${_QT_LIBS}" ENV QSCINTILLA_ROOT
    PATHS /usr/lib "/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}"           # Debian multiarch
          /usr/local/lib /opt/homebrew/lib
          /opt/homebrew/opt/qscintilla2/lib /usr/local/opt/qscintilla2/lib)

if(NOT QSCINTILLA_INCLUDE_DIR OR NOT QSCINTILLA_LIBRARY)
    message(FATAL_ERROR
        "QScintilla (Qt5) not found. Install the dev package:\n"
        "    Debian/Ubuntu: sudo apt install libqscintilla2-qt5-dev\n"
        "    Fedora:        sudo dnf install qscintilla-qt5-devel\n"
        "    Arch:          sudo pacman -S qscintilla-qt5\n"
        "    macOS:         brew install qscintilla2\n"
        "or pass -DQSCINTILLA_ROOT=<prefix>.")
endif()

add_executable(adaedit
    src/main.cpp
    src/mainwindow.cpp
    src/adalexer.cpp
    src/project.cpp
    src/debugger.cpp
    src/lspclient.cpp
    src/serialmonitor.cpp
    resources/adaedit.qrc
)

target_include_directories(adaedit PRIVATE ${QSCINTILLA_INCLUDE_DIR})
target_link_libraries(adaedit PRIVATE
    Qt5::Widgets
    Qt5::PrintSupport
    Qt5::SerialPort
    ${QSCINTILLA_LIBRARY}
)
