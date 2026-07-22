cmake_minimum_required(VERSION 3.16)
project(cyberpet_sim CXX C)

set(CMAKE_CXX_STANDARD 17)

# LVGL v8.3 - fetched automatically, pinned to the version this project
# is verified against (same major.minor the Waveshare examples use).
include(FetchContent)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
FetchContent_Declare(lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG        v8.3.11
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lvgl)

set(FW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../CyberPet)

add_executable(cyberpet_sim
  main_web.cpp
  dict_backend_stdio.cpp
  ${FW_DIR}/pet.cpp
  ${FW_DIR}/habits.cpp
  ${FW_DIR}/ui.cpp
  ${FW_DIR}/sprites.c
  ${FW_DIR}/dict.cpp
  ${FW_DIR}/dict_ui.cpp
)

target_include_directories(cyberpet_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}        # lv_conf.h
  ${CMAKE_CURRENT_SOURCE_DIR}/shim   # Arduino.h shim
  ${FW_DIR}                          # pet.h / habits.h / ui.h
)

target_link_libraries(cyberpet_sim PRIVATE lvgl)

if(EMSCRIPTEN)
  set(CMAKE_EXECUTABLE_SUFFIX ".html")
  target_compile_options(cyberpet_sim PRIVATE -sUSE_SDL=2)
  target_link_options(cyberpet_sim PRIVATE
    -sUSE_SDL=2
    -sALLOW_MEMORY_GROWTH=1
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html
  )
else()
  # Native desktop build (quick local testing without a browser)
  find_package(SDL2 REQUIRED)
  target_link_libraries(cyberpet_sim PRIVATE SDL2::SDL2)
  # Short Pomodoro intervals (30s focus / 10s break) so you can test without waiting 25 min
  target_compile_definitions(cyberpet_sim PRIVATE POMODORO_SIM_MODE)
endif()
