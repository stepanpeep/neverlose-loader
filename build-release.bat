@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Install "Desktop development with C++" in Visual Studio Installer.
  pause
  exit /b 1
)
echo Removing stale build files...
if exist build rmdir /s /q build
if exist out\build rmdir /s /q out\build
cmake -S . -B build -A x64
if errorlevel 1 goto :fail
cmake --build build --config Release --clean-first
if errorlevel 1 goto :fail
echo.
echo Build completed: build\Release\NeverloseLoader.exe
pause
exit /b 0
:fail
echo.
echo Build failed. Open the output above for the exact compiler error.
pause
exit /b 1
