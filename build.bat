@echo off
rem Run from a Visual Studio "x64 Native Tools Command Prompt"
rc /nologo /fo res\app.res res\app.rc || exit /b 1
cl /nologo /O2 /EHsc /utf-8 /std:c++17 /DUNICODE /D_UNICODE src\main.cpp res\app.res /Fe:OLEDTextFix.exe ^
  /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib dwmapi.lib comctl32.lib shell32.lib user32.lib gdi32.lib advapi32.lib ole32.lib
