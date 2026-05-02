cmake_minimum_required(VERSION 3.21)
project(silvertune-companion VERSION 0.2.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
FetchContent_Declare(miniaudio
  GIT_REPOSITORY https://github.com/mackron/miniaudio.git
  GIT_TAG        0.11.21
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(miniaudio)

add_executable(silvertune-companion
  src/main.cpp
  src/ws.cpp
  src/yin.cpp
)

target_include_directories(silvertune-companion PRIVATE
  src
  ${miniaudio_SOURCE_DIR}
)

if(WIN32)
  target_link_libraries(silvertune-companion ws2_32)
elseif(APPLE)
  target_link_libraries(silvertune-companion
    "-framework CoreAudio"
    "-framework AudioToolbox"
    "-framework CoreFoundation"
  )
elseif(UNIX)
  find_package(Threads REQUIRED)
  target_link_libraries(silvertune-companion Threads::Threads dl m)
  find_package(ALSA QUIET)
  if(ALSA_FOUND)
    target_link_libraries(silvertune-companion ALSA::ALSA)
  endif()
endif()

if(MSVC)
  target_compile_options(silvertune-companion PRIVATE /O2 /W3)
else()
  target_compile_options(silvertune-companion PRIVATE -O2 -Wall)
endif()

if(DEFINED COMPANION_VERSION)
  target_compile_definitions(silvertune-companion PRIVATE COMPANION_VERSION="${COMPANION_VERSION}")
endif()
