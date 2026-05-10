@echo off
rem FreeDOS(98) NEC98 Build Script
rem Usage: build98.bat [debug]

set WATCOM=C:\WATCOM
set PATH=C:\WATCOM\binnt64;%PATH%
set INCLUDE=C:\WATCOM\h
set EDPATH=C:\WATCOM\eddat

set COMPILER=owwin
set XCPU=86
set XFAT=16
set XNASM=nasm
set XLINK=wlink
set MAKE=wmake -ms -h -e
set XUPX=

if "%1" == "debug" set ALLCFLAGS=-DDEBUG -wcd303

echo ====== FreeDOS(98) NEC98 Build ======
echo COMPILER=%COMPILER%
echo ALLCFLAGS=%ALLCFLAGS%

cd /D %~dp0

echo.
echo === UTILS ===
cd utils
%MAKE% -f makefile.wc production
if errorlevel 1 goto fail

echo.
echo === LIB ===
cd ..\lib
if not exist libm.lib wtouch libm.lib
%MAKE% -f makefile.wc production
if errorlevel 1 goto fail

echo.
echo === DRIVERS ===
cd ..\drivers
%MAKE% -f makefile.wc production
if errorlevel 1 goto fail

echo.
echo === BOOT ===
cd ..\boot
%MAKE% -f makefile.wc production
if errorlevel 1 goto fail

rem SYS skipped (16bit DOS EXE cannot run on 64bit Windows)
rem echo.
rem echo === SYS ===
rem cd ..\sys
rem %MAKE% -f makefile.wc production
rem if errorlevel 1 goto fail

echo.
echo === KERNEL ===
cd ..\kernel
%MAKE% -f makefile.wc production
if errorlevel 1 goto fail

cd ..
echo.
echo ====== Build Complete! ======
dir /B bin\*.sys 2>nul
goto done

:fail
cd ..
echo.
echo !!! Build FAILED !!!
exit /b 1

:done
