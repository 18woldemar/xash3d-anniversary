# The build VM's dependencies, into C:\work\anniversary\deps: SDL2 for MSVC, the Vulkan headers and glslang.
# Run once on the VM: powershell -ExecutionPolicy Bypass -File deps.ps1
$ErrorActionPreference = 'Stop'; $ProgressPreference = 'SilentlyContinue'
$d = 'C:\work\anniversary\deps'; New-Item -ItemType Directory -Force $d | Out-Null; Set-Location $d
Invoke-WebRequest https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-VC.zip -OutFile sdl2.zip
Expand-Archive -Force sdl2.zip .
Invoke-WebRequest https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.zip -OutFile vkh.zip
Expand-Archive -Force vkh.zip .
Invoke-WebRequest https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip -OutFile glslang.zip
Expand-Archive -Force glslang.zip glslang
Get-ChildItem -Name; Get-ChildItem -Name glslang\bin
