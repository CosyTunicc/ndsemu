cmake_minimum_required(VERSION 3.16)
# Runtime Visual C++ lie dans l'exe : le programme tourne chez un ami sans installer les "redistribuables" Visual C++
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Version du programme : a augmenter avant chaque publication (voir "Publier une mise a jour" dans le README)
project(ndsemu VERSION 1.0.0 LANGUAGES CXX)
# Depot GitHub ou ndsemu cherche ses mises a jour (GitHub Releases) : cmake -S . -B build -DNDSEMU_GITHUB_REPO=compte/depot
set(NDSEMU_GITHUB_REPO "CosyTunicc/ndsemu" CACHE STRING "Depot GitHub des mises a jour (compte/depot)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  set(WARN /W4)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  set(WARN -Wall -Wextra)
endif()

add_library(core
  src/core/nds_header.cpp
  src/core/rom.cpp
  src/core/memory.cpp
  src/core/arm.cpp
  src/core/video.cpp
  src/core/gx.cpp
  src/core/sound.cpp
  src/core/savestate.cpp
  src/core/emulator.cpp
  src/core/wifi.cpp
)
target_include_directories(core PUBLIC src)
target_compile_options(core PRIVATE ${WARN})

add_executable(ndsemu src/main.cpp)
target_link_libraries(ndsemu PRIVATE core)
target_compile_options(ndsemu PRIVATE ${WARN})

if(WIN32)
  # Dear ImGui (third_party/imgui, licence MIT) : interface de la bibliotheque de jeux
  add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
  )
  target_include_directories(imgui PUBLIC third_party/imgui)

  target_sources(ndsemu PRIVATE
    src/platform/window_win32.cpp
    src/platform/audio_win32.cpp
    src/platform/input_win32.cpp
    src/ui/library.cpp
    src/ui/library_ui.cpp
    src/ui/library_win32.cpp
    src/ui/updater.cpp
    src/ui/updater_win32.cpp
  )
  target_compile_definitions(ndsemu PRIVATE NDSEMU_VERSION="${PROJECT_VERSION}" NDSEMU_GITHUB_REPO="${NDSEMU_GITHUB_REPO}")
  target_link_libraries(ndsemu PRIVATE imgui user32 gdi32 winmm shell32 ole32 dwmapi d3d11 d3dcompiler dxgi hid setupapi winhttp)
endif()
# Version SDL2 (Linux ; la PS5 se compile avec tools/build_ps5.sh) : cmake -DNDSEMU_SDL=ON
option(NDSEMU_SDL "Compiler la version SDL2 (ndsemu_sdl)" OFF)
if(NDSEMU_SDL)
  find_package(SDL2 REQUIRED)
  add_executable(ndsemu_sdl
    src/platform/sdl/main_sdl.cpp
    src/platform/sdl/soft_imgui.cpp
    src/ui/library.cpp
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
  )
  target_include_directories(ndsemu_sdl PRIVATE third_party/imgui ${SDL2_INCLUDE_DIRS})
  target_link_libraries(ndsemu_sdl PRIVATE core ${SDL2_LIBRARIES})
endif()
