cmake_minimum_required(VERSION 3.24)

project(colotaches VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    find_package(Qt6 REQUIRED COMPONENTS Core Network Quick QuickControls2 Sql WebSockets)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Network Quick QuickControls2 Sql WebSockets Test)
    find_package(Qt6 OPTIONAL_COMPONENTS Widgets)
endif()

# Caméra = scan du QR d'appairage : bonus, pas fondation. Sans Qt Multimedia (distro
# qui ne le package pas), l'app build sans scanner et le collage de lien suffit.
find_package(Qt6 OPTIONAL_COMPONENTS Multimedia)
if(TARGET Qt6::Multimedia)
    set(COLO_HAS_CAMERA ON)
else()
    set(COLO_HAS_CAMERA OFF)
    message(STATUS "Qt Multimedia absent → scan du QR désactivé (appairage par lien seulement)")
endif()

qt_standard_project_setup()

# zxing-cpp : décodage du QR d'appairage (qrcodegen, vendored, ne fait qu'encoder).
# Récupéré à la compilation — statique, lecteurs seuls, pas d'écriture ni d'exemples.
# Hors du garde COLO_HAS_CAMERA : le décodeur doit rester compilé (et testé) même
# quand Qt Multimedia manque, sinon le seul test de décodage ne tournerait jamais.
include(FetchContent)
set(ZXING_READERS  ON  CACHE BOOL "" FORCE)
set(ZXING_WRITERS  OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zxing
    GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
    GIT_TAG        v2.2.1
)
FetchContent_MakeAvailable(zxing)

add_subdirectory(src)

# Tests run on the host only.
if(NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()
