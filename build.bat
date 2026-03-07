@echo off
REM Build script for Filament Red Cone Demo
REM Adjust FILAMENT_DIR and SDL2_DIR to match your installations

setlocal

REM === CONFIGURATION - Edit these paths ===
set FILAMENT_DIR=C:\filament
set SDL2_DIR=C:\SDL2

REM Check if Filament exists
if not exist "%FILAMENT_DIR%" (
    echo ERROR: Filament not found at %FILAMENT_DIR%
    echo Please download Filament from https://github.com/google/filament/releases
    echo and update FILAMENT_DIR in this script.
    exit /b 1
)

REM Compile material
echo Compiling material...
if not exist "materials\red_material.filamat" (
    "%FILAMENT_DIR%\bin\matc.exe" -o materials\red_material.filamat -p desktop materials\red_material.mat
    if errorlevel 1 (
        echo ERROR: Failed to compile material
        exit /b 1
    )
    echo Material compiled successfully.
) else (
    echo Material already compiled, skipping.
)

REM Create build directory
if not exist "build" mkdir build
cd build

REM Configure with CMake
echo Configuring with CMake...
cmake .. -DFILAMENT_DIR="%FILAMENT_DIR%" -DSDL2_DIR="%SDL2_DIR%"
if errorlevel 1 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

REM Build
echo Building...
cmake --build . --config Release
if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

REM Copy material to build directory
copy /Y "..\materials\red_material.filamat" "Release\" >nul 2>&1

echo.
echo Build complete! Run: build\Release\FilamentCone.exe
