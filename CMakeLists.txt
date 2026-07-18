cmake_minimum_required(VERSION 3.24)
project(NeverloseLoader VERSION 1.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "This launcher targets Windows 10/11 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(NL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_executable(NeverloseLoader WIN32
    src/main.cpp
    src/App.cpp
    src/LauncherCore.cpp
    src/ManifestService.cpp
    src/ModrinthService.cpp
    src/GameInstaller.cpp
    src/ArtifactDownloader.cpp
    src/SettingsService.cpp
    src/MiniJson.cpp
    resources.rc
)

target_include_directories(NeverloseLoader PRIVATE src)
target_compile_definitions(NeverloseLoader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(NeverloseLoader PRIVATE /W4 /permissive- /utf-8 /EHsc)
if(NL_WARNINGS_AS_ERRORS)
    target_compile_options(NeverloseLoader PRIVATE /WX)
endif()
target_link_libraries(NeverloseLoader PRIVATE
    d2d1 dwrite dwmapi windowscodecs winhttp bcrypt
    shell32 shlwapi ole32 comctl32 user32 gdi32
)

set_property(TARGET NeverloseLoader PROPERTY VS_DPI_AWARE "PerMonitor")

add_custom_command(TARGET NeverloseLoader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:NeverloseLoader>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/neverlose-loader-logo-1024.png"
        "$<TARGET_FILE_DIR:NeverloseLoader>/assets/neverlose-loader-logo-1024.png"
    COMMAND ${CMAKE_COMMAND} -DINPUT="$<TARGET_FILE:NeverloseLoader>" -DMINIMUM_SIZE=1400000 -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PadExecutable.cmake"
)

install(TARGETS NeverloseLoader RUNTIME DESTINATION .)
install(DIRECTORY assets DESTINATION .)
