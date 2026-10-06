@echo off
rem Configure and build with the bundled MSVC toolchain. See docs/commands.md.
rem   build.cmd [Debug|Release] [extra cmake args...]
setlocal EnableDelayedExpansion

if "%VS_ROOT%"=="" set "VS_ROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "CMAKE_BIN=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "NINJA_BIN=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"

rem The LunarG installer sets VULKAN_SDK machine-wide, but a shell started before it ran will not
rem have it. Fall back to the newest SDK under C:\VulkanSDK so a fresh terminal still works.
if "%VULKAN_SDK%"=="" (
    for /f "delims=" %%d in ('dir /b /ad /o-n "C:\VulkanSDK" 2^>nul') do (
        if "%VULKAN_SDK%"=="" set "VULKAN_SDK=C:\VulkanSDK\%%d"
    )
)

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

rem Everything after the build type is forwarded to cmake.
set "EXTRA="
:collect
shift
if "%~1"=="" goto configure
set "EXTRA=!EXTRA! %1"
goto collect

:configure
call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%CMAKE_BIN%;%NINJA_BIN%;%PATH%"

cmake -S "%~dp0." -B "%~dp0build\%BUILD_TYPE%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl !EXTRA! || exit /b 1
cmake --build "%~dp0build\%BUILD_TYPE%" || exit /b 1
