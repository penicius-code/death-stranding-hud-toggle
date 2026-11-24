@echo off
rem Builds build\DSHudToggle.asi with Zig 0.16.0.
rem Uses zig.exe from PATH, or set ZIG to the full path of zig.exe first:
rem   set "ZIG=D:\tools\zig-x86_64-windows-0.16.0\zig.exe"
setlocal
if not defined ZIG set "ZIG=zig"
if not exist "%~dp0build" mkdir "%~dp0build"
"%ZIG%" cc -target x86_64-windows-gnu -shared -O2 -s -Wall -Wextra -o "%~dp0build\DSHudToggle.asi" "%~dp0src\dshudtoggle.c" -luser32
if errorlevel 1 (echo Build failed.) else (echo Built build\DSHudToggle.asi)
endlocal
