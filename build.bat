@echo off
rem Local MSVC build: ExBlend.dll for AviSynth+ and VapourSynth (x64) in build\
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
set FLAGS=/nologo /O2 /EHsc /std:c++17 /fp:precise /W4 /MT /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Iinclude\avisynth /Iinclude\vapoursynth /Isrc
cl %FLAGS% /LD src\core\exblend.cpp src\avs_plugin.cpp src\vs_plugin.cpp ^
  /Fobuild\ /Fdbuild\ /Febuild\ExBlend.dll /link /NOLOGO || exit /b 1
cl %FLAGS% test\core_test.cpp src\core\exblend.cpp /Fobuild\ /Fdbuild\ /Febuild\core_test.exe /link /NOLOGO || exit /b 1
