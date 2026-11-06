# Builds CarrierCommand.exe with MSVC. Run: powershell -File build.ps1
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$sdl = Join-Path $root 'third_party\SDL2'
$vc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$out = Join-Path $root 'dist'
New-Item -ItemType Directory -Force $out, (Join-Path $root 'build') | Out-Null
$cmd = "`"$vc`" >nul && cd /d `"$root\build`" && cl /nologo /O2 /GL /W3 /MT /D_CRT_SECURE_NO_WARNINGS /DSDL_MAIN_HANDLED /I`"$sdl\include`" ..\src\cpu.c ..\src\vga.c ..\src\machine.c ..\src\hires.c ..\src\textures.c ..\src\main.c /Fe:`"$out\CarrierCommand.exe`" /link /LTCG /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup `"$sdl\lib\x64\SDL2.lib`" shell32.lib"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Copy-Item "$sdl\lib\x64\SDL2.dll" $out -Force
$gameDir = (New-Item -ItemType Directory -Force "$out\game").FullName
if (Test-Path "$root\game\CARRIER.EXE") {
    Copy-Item "$root\game\CARRIER.EXE", "$root\game\CARRIER.DAT" $gameDir -Force
} else {
    Write-Warning "Game files not found: copy CARRIER.EXE and CARRIER.DAT into $gameDir before running."
}
Write-Output "built $out\CarrierCommand.exe"
