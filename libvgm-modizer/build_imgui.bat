@echo off
REM Build script for vgmplay_imgui.exe using MinGW

echo Building libvgm ImGui Player...

REM Create build directory
if not exist build_imgui mkdir build_imgui
cd build_imgui

REM Run CMake with proper Windows paths
cmake .. ^
  -G "MinGW Makefiles" ^
  -DCMAKE_C_COMPILER=gcc ^
  -DCMAKE_CXX_COMPILER=g++ ^
  -DZLIB_LIBRARY=D:/msys64/mingw64/lib/libz.a ^
  -DZLIB_INCLUDE_DIR=D:/msys64/mingw64/include ^
  -DBUILD_LIBAUDIO=ON ^
  -DAUDIODRV_DSOUND=ON ^
  -DAUDIODRV_WINMM=ON ^
  -DUTIL_CHARCNV_WINAPI=ON ^
  -DBUILD_PLAYER=OFF ^
  -DBUILD_VGM2WAV=OFF ^
  -DBUILD_VGMPLAY_GUI=OFF ^
  -DBUILD_VGMPLAY_IMGUI=ON

if errorlevel 1 (
  echo CMake configuration failed!
  pause
  exit /b 1
)

REM Build the project
mingw32-make vgmplay_imgui -j4

if errorlevel 1 (
  echo Build failed!
  pause
  exit /b 1
)

echo.
echo Build successful!
echo Executable: build_imgui\bin\vgmplay_imgui.exe
pause
