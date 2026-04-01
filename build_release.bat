@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 (
    echo FAILED: vcvars64.bat
    exit /b 1
)
set PATH=C:\Qt\5.15.2\msvc2019_64\bin;%PATH%
cd /d "D:\projects\src\google driver\CorneaController\build\Desktop_Qt_5_15_2_MSVC2019_64bit-Release"
echo Building...
nmake -f Makefile.Release 2>&1
echo Done. Exit code: %errorlevel%
