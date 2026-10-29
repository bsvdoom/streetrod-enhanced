# The game's launcher, "Street Rod Enhanced": the game folder, the original cars or the Street Rod SE
# data disk's, the game's own switches and the display options, then streetrod-enhanced.
#
#   cmake -S launcher -B launcher/build -G Ninja && cmake --build launcher/build
#
# or, from the main project (SRE_LAUNCHER, on by default), next to streetrod-enhanced.exe.
# Needs wxWidgets 3.2 (MSYS2: mingw-w64-x86_64-wxwidgets3.2-msw; Debian and Ubuntu: libwxgtk3.2-dev).
cmake_minimum_required(VERSION 3.24)
project(srelauncher LANGUAGES CXX)
if(WIN32)
    enable_language(RC)
endif()

# MSYS2 and wxWidgets' own CMake builds install a package config; distributions
# such as Debian only ship wx-config, which CMake's FindwxWidgets uses. Cross-compiling with mingw-w64
# (cmake/mingw-w64-x86_64.cmake): SRL_WX_MINGW_DIR = the official wxMSW gcc "Dev" + "ReleaseDLL" +
# headers packages unpacked into one folder (include/, lib/gcc1320_x64_dll/).
set(SRL_WX_MINGW_DIR "" CACHE PATH "wxMSW mingw binary package (cross builds)")
if(SRL_WX_MINGW_DIR)
    set(SRL_WX_LIBDIR "${SRL_WX_MINGW_DIR}/lib/gcc1320_x64_dll")
    add_library(srl_wx INTERFACE)
    target_include_directories(srl_wx SYSTEM INTERFACE "${SRL_WX_LIBDIR}/mswu" "${SRL_WX_MINGW_DIR}/include")
    target_compile_definitions(srl_wx INTERFACE WXUSINGDLL __WXMSW__ UNICODE _UNICODE)
    target_link_libraries(srl_wx INTERFACE "${SRL_WX_LIBDIR}/libwxmsw32u_core.a" "${SRL_WX_LIBDIR}/libwxbase32u.a")
    set(SRL_WX_LIBRARIES srl_wx)
else()
find_package(wxWidgets 3.2 CONFIG QUIET COMPONENTS core base)
if(wxWidgets_FOUND AND TARGET wx::wxcore)
    set(SRL_WX_LIBRARIES wx::wxcore wx::wxbase)
else()
    find_package(wxWidgets 3.2 REQUIRED COMPONENTS core base)
    include(${wxWidgets_USE_FILE})
    set(SRL_WX_LIBRARIES ${wxWidgets_LIBRARIES})
endif()
endif()

add_executable(srelauncher WIN32
    app.cpp
    game.cpp
    icon.cpp
    launcher.cpp
    settings.cpp
)
set_target_properties(srelauncher PROPERTIES OUTPUT_NAME "Street Rod Enhanced" CXX_EXTENSIONS OFF)
target_compile_features(srelauncher PRIVATE cxx_std_17)
target_link_libraries(srelauncher PRIVATE ${SRL_WX_LIBRARIES})
# Built from the main project: beside streetrod-enhanced.exe, where it looks for it.
if(NOT PROJECT_IS_TOP_LEVEL)
    set_target_properties(srelauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

if(WIN32)
    target_sources(srelauncher PRIVATE app.rc)
    target_link_libraries(srelauncher PRIVATE comctl32)
    if(MINGW)
        # The C++ runtime inside the exe, so the build needs fewer DLLs beside it.
        target_link_options(srelauncher PRIVATE -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
    endif()
    # The wxWidgets DLLs and everything they load, found where the compiler's own DLLs are (or, cross
    # building, in the wx package).
    if(SRL_WX_MINGW_DIR)
        # The wx gcc13.2 DLLs load the gcc runtime (libgcc_s_seh-1, libstdc++-6, posix threads:
        # libwinpthread-1), which the wx package does not ship: take the same gcc 13's from the mingw-w64
        # cross compiler (posix flavour).
        file(GLOB SRL_GCC_POSIX_DIR LIST_DIRECTORIES true "/usr/lib/gcc/x86_64-w64-mingw32/*-posix")
        set(SRL_DLL_DIR "${SRL_WX_LIBDIR}" ${SRL_GCC_POSIX_DIR} "/usr/x86_64-w64-mingw32/lib")
    else()
        get_filename_component(SRL_DLL_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    endif()
    list(JOIN SRL_DLL_DIR "|" SRL_DLL_DIRS)        # one argument; copy_dlls.cmake splits it at "|"
    add_custom_command(TARGET srelauncher POST_BUILD
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:srelauncher> "-DSEARCH=${SRL_DLL_DIRS}"
                -DCMAKE_OBJDUMP=${CMAKE_OBJDUMP} -P ${CMAKE_CURRENT_SOURCE_DIR}/copy_dlls.cmake
        VERBATIM)
endif()

if(MSVC)
    target_compile_options(srelauncher PRIVATE /W4 /utf-8)
else()
    target_compile_options(srelauncher PRIVATE -Wall -Wextra)
endif()

# Test of the command line the launcher builds (tests/retry/t6_launcher.sh): GameArguments only.
option(SRL_TESTS "Build the launcher's argument test" OFF)
if(SRL_TESTS)
    add_executable(srl-test-args test_args.cpp game.cpp settings.cpp)
    target_compile_features(srl-test-args PRIVATE cxx_std_17)
    target_link_libraries(srl-test-args PRIVATE ${SRL_WX_LIBRARIES})
endif()
