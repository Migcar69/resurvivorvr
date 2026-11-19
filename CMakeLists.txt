cmake_minimum_required(VERSION 3.20)
# Name of the program and window title: change them for each game.
set(GAME_EXE resurvivor)
set(GAME_TITLE "Resident Evil Survivor VR")
# PAL discs (SLES/SCES...) run at 50 Hz, NTSC ones (SLUS/SCUS/SLPS/SCPS...) at 60 Hz.
option(PSX_PAL "the disc is PAL (50 Hz)" ON)
# Development build: the VR options' save / load a state for analysis, F5, the crash recorder. Off for releases.
option(PSXVR_DEV "development tools" OFF)
# Release build: VR only, the American disc only (its boot file in SYSTEM.CNF; another disc is refused with a message)
option(RESVR_RELEASE "release build" OFF)
set(GAME_DISC "SLUS_010.87")

project(${GAME_EXE} CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W3 /MP /utf-8 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN)
endif()
set(OPENXR_DIR ${CMAKE_SOURCE_DIR}/third_party/openxr)
file(GLOB CORE_SRC src/core/*.cpp)
file(GLOB VR_SRC src/vr/*.cpp)
file(GLOB GAME_SRC src/game/*.cpp)
add_executable(${GAME_EXE} src/platform/main_win32.cpp ${CORE_SRC} ${VR_SRC} ${GAME_SRC})
# Development: the weapons' models made from the developer's own game saves built into the program (never published;
# releases rebuild them from the player's game, weapon_harvest.cpp)
option(RESVR_EMBED_WEAPONS "build the weapon model headers in (development only, not for releases)" OFF)
target_compile_definitions(${GAME_EXE} PRIVATE GAME_TITLE="${GAME_TITLE}" $<$<BOOL:${PSX_PAL}>:PSX_PAL>
  $<$<BOOL:${RESVR_EMBED_WEAPONS}>:RESVR_EMBED_WEAPONS> $<$<BOOL:${PSXVR_DEV}>:PSXVR_DEV>
  $<$<BOOL:${RESVR_RELEASE}>:RESVR_RELEASE> $<$<BOOL:${RESVR_RELEASE}>:GAME_DISC="${GAME_DISC}">)
target_include_directories(${GAME_EXE} PRIVATE ${OPENXR_DIR}/include)
target_link_libraries(${GAME_EXE} PRIVATE d3d11 dxgi d3dcompiler xaudio2 xinput ${OPENXR_DIR}/x64/lib/openxr_loader.lib)
add_custom_command(TARGET ${GAME_EXE} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENXR_DIR}/x64/bin/openxr_loader.dll $<TARGET_FILE_DIR:${GAME_EXE}>)
