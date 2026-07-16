@echo off
setlocal
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Install "Desktop development with C++" in Visual Studio Installer.
  pause
  exit /b 1
)
cmake -S . -B build -A x64
if errorlevel 1 goto :fail
cmake --build build --config Release
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
