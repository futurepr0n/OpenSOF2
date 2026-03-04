@echo off
setlocal

set "CMAKE_EXE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD_DIR=E:\SOF2\OpenSOF2\build_test"

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Debug --target openjk_sp.x86
if errorlevel 1 exit /b %errorlevel%

endlocal
