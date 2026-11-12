@echo off
rem xash3d-anniversary on the Windows build VM: unpack src.tgz, build the engine and hlsdk with MSVC, install into out\.
rem C:\work\anniversary holds tools\python (a private Python 3.12 from python.org, not on PATH) and deps\ (deps.ps1).
rem tools/package (PACKAGE_WIN=1) sends src.tgz and this file, runs it and fetches out\.
setlocal
set TOP=C:\work\anniversary
set PY=%TOP%\tools\python\python.exe
rem tools\git.bat answers waf for the git the VM does not have
set PATH=%TOP%\deps\glslang\bin;%TOP%\tools;%PATH%
rem waf takes CFLAGS from the environment; MSVC wants forward slashes here
set CFLAGS=-IC:/work/anniversary/deps/Vulkan-Headers-vulkan-sdk-1.4.363.0/include
cd /d %TOP%
if exist src rmdir /s /q src
if exist out rmdir /s /q out
mkdir src
tar -xzf src.tgz -C src || exit /b 1
cd /d %TOP%\src\xash3d-anniversary
"%PY%" waf configure -s %TOP%\deps\SDL2-2.32.10 -T release -8 || exit /b 1
"%PY%" waf build || exit /b 1
"%PY%" waf install --destdir=%TOP%\out || exit /b 1
copy /y %TOP%\deps\SDL2-2.32.10\lib\x64\SDL2.dll %TOP%\out\ >nul
copy /y %TOP%\deps\SDL2-2.32.10\LICENSE.txt %TOP%\out\SDL2-LICENSE.txt >nul
cd /d %TOP%\src\hlsdk-anniversary
"%PY%" waf configure -T release -8 || exit /b 1
"%PY%" waf build || exit /b 1
"%PY%" waf install --destdir=%TOP%\out || exit /b 1
echo BUILD OK
