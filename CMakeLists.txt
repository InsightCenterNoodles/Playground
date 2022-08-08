cmake_minimum_required(VERSION 3.16)

project(Playground VERSION 0.2 LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# CMake Modules ================================================================

include(cmake/CPM.cmake)

CPMAddPackage(
  NAME GroupSourcesByFolder.cmake
  GITHUB_REPOSITORY TheLartians/GroupSourcesByFolder.cmake
  VERSION 1.0
)

CPMAddPackage(
    NAME glm
    URL  https://github.com/g-truc/glm/releases/download/0.9.9.8/glm-0.9.9.8.7z
    VERSION 0.9.9.8
)

CPMAddPackage(
    NAME noodles
    GITHUB_REPOSITORY InsightCenterNoodles/NoodlesPlusPlus
    GIT_TAG cd0680bcc90112bb9323c8cc04fc21736be58f44
)

CPMAddPackage(
    NAME quazip
    GITHUB_REPOSITORY stachenov/quazip
    GIT_TAG 403fbcb56626551fe911af0c3a5ee2b5122df664
    OPTIONS
        "BUILD_SHARED_LIBS OFF"
        "QUAZIP_USE_QT_LIB ON"
)

CPMAddPackage(
    NAME assimp
    URL https://github.com/assimp/assimp/archive/refs/heads/master.zip
    OPTIONS
        "BUILD_SHARED_LIBS OFF"
        "ASSIMP_BUILD_ASSIMP_TOOLS OFF"
        "ASSIMP_BUILD_TESTS OFF"
        "ASSIMP_BUILD_MINIZIP OFF"
        "ASSIMP_INJECT_DEBUG_POSTFIX OFF"
)

find_package(Qt6 COMPONENTS WebSockets Core Gui)

if (NOT Qt6_FOUND)
    find_package(Qt5 COMPONENTS Gui Widgets Core Websockets)
endif()
LINK_DIRECTORIES(/usr/local/lib)
# Options ======================================================================

set(SANITIZER "none" CACHE STRING "Build with a given sanitizer")
set_property(CACHE SANITIZER PROPERTY STRINGS none address)

SET(sanitizer_compile_flag "")

if (${SANITIZER} STREQUAL "address")
    SET(sanitizer_compile_flag "-fsanitize=address")
endif()

add_executable(Playground "")
target_compile_features(Playground PUBLIC cxx_std_20)
target_compile_options(Playground PUBLIC ${sanitizer_compile_flag})
target_link_options(Playground PUBLIC ${sanitizer_compile_flag})

target_link_libraries(Playground PRIVATE glm)

target_link_libraries(Playground PRIVATE noodles quazip)

target_link_libraries(Playground PRIVATE assimp)

target_link_libraries(Playground PUBLIC
    Qt::Core Qt::WebSockets Qt::Gui
)

add_subdirectory(src)
GroupSourcesByFolder(Playground)
