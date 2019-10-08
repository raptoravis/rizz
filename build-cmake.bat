@echo off

echo "=========================================================="
echo "Usage: build-cmake.bat <vs2017|vs2019|mingw> [open|build] [target]"
echo "=========================================================="

set GENERATOR=%1
set ACTION=%2 
set TARGET=%3 

set BUILD_DIR=./build/win64
 
if "%GENERATOR%"=="" (set GENERATOR=vs2017)
if "%ACTION%"=="" (set ACTION=open) 

if "%TARGET%"==" " set TARGET=_none_
if "%TARGET%"=="" set TARGET=_none_

REM echo "%TARGET%"

call :Trim GENERATOR %GENERATOR%
call :Trim ACTION %ACTION%

if "%TARGET%"=="_none_" (
    set TARGET=
) else (
    call :Trim TARGET %TARGET%
    set TARGET=--target %TARGET%
)

call :Trim GENERATOR %GENERATOR%
call :Trim ACTION %ACTION%

if "%GENERATOR%"=="vs2017" (
    set CMAKE_GENERATOR="Visual Studio 15 2017 Win64"
) else (
    if "%GENERATOR%"=="vs2019" (
        set CMAKE_GENERATOR="Visual Studio 16 2019"
    ) else (
        if "%GENERATOR%"=="mingw" (
            set CMAKE_GENERATOR="MinGW Makefiles"
            set BUILD_DIR=./build/mingw
        ) else (
            echo "invalid GENERATOR: %GENERATOR%"
        )
    )
)

echo "=========================================================="
echo "GENERATOR: %GENERATOR% ACTION: %ACTION% %TARGET%"
echo "=========================================================="

cmake -S . -B %BUILD_DIR% -G %CMAKE_GENERATOR% 

echo "=========================================================="

if "%ACTION%"=="open" (
    cmake --open %BUILD_DIR%
) else (
    if "%ACTION%"=="build" (
        cmake --build %BUILD_DIR% %TARGET%
    ) else (
       echo "invalid ACTION: %ACTION%"
    )
)
    
echo ""

:Trim
SetLocal EnableDelayedExpansion
set Params=%*
for /f "tokens=1*" %%a in ("!Params!") do EndLocal & set %1=%%b
exit /b