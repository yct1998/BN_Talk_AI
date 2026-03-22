@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64
if errorlevel 1 exit /b 1

if not exist build\objs mkdir build\objs

cl /nologo /std:c++17 /EHsc /c /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /I. /Ibridge_dll /Icommon /Fo:build\objs\ bridge_dll\bridge_dll.cpp bridge_dll\lua_runtime_bridge.cpp common\bntalk_protocol.cpp
if errorlevel 1 exit /b 1

link /nologo /DLL /OUT:build\bntalk_bridge_lua.dll /IMPLIB:build\bntalk_bridge_lua.lib build\objs\bridge_dll.obj build\objs\lua_runtime_bridge.obj build\objs\bntalk_protocol.obj ..\..\..\lapi.obj ..\..\..\lauxlib.obj ..\..\..\lbaselib.obj ..\..\..\lbitlib.obj ..\..\..\lcode.obj ..\..\..\lcorolib.obj ..\..\..\lctype.obj ..\..\..\ldblib.obj ..\..\..\ldebug.obj ..\..\..\ldo.obj ..\..\..\ldump.obj ..\..\..\lfunc.obj ..\..\..\lgc.obj ..\..\..\linit.obj ..\..\..\liolib.obj ..\..\..\llex.obj ..\..\..\lmathlib.obj ..\..\..\lmem.obj ..\..\..\loadlib.obj ..\..\..\lobject.obj ..\..\..\lopcodes.obj ..\..\..\loslib.obj ..\..\..\lparser.obj ..\..\..\lstate.obj ..\..\..\lstring.obj ..\..\..\lstrlib.obj ..\..\..\ltable.obj ..\..\..\ltablib.obj ..\..\..\ltm.obj ..\..\..\lundump.obj ..\..\..\lutf8lib.obj ..\..\..\lvm.obj ..\..\..\lzio.obj user32.lib winhttp.lib
if errorlevel 1 exit /b 1

cl /nologo /std:c++17 /EHsc /DWIN32_LEAN_AND_MEAN injector\main.cpp /link /OUT:build\bntalk_injector.exe
if errorlevel 1 exit /b 1

dir build\bntalk_bridge_lua.dll
dir build\bntalk_injector.exe
endlocal
