# SPDX-License-Identifier: GPL-2.0-or-later
# Build ste-recomp.exe from source. See BUILD.md.
#   .\ste\scripts\build.ps1 -Xbe "D:\path\to\your\extracted\default.xbe" [-Sdl3Src <SDL 3.4.x source>] [-Out build]
# Needs: Visual Studio 2022 Build Tools (C++ x64 workload + Windows SDK), CMake >= 3.20, Python >= 3.10, and
# your own default.xbe (read only).  Nothing is downloaded unless -Sdl3Src is omitted and deps\SDL is missing
# (then git clones the SDL release tag).  Refuses to overwrite an existing output directory.
param(
  [Parameter(Mandatory=$true)][string]$Xbe,
  [string]$Out = "build",
  [string]$Sdl3Src = "",
  [string]$Version = "0.1.0",
  [int]$BuildNumber = 260,             # printed by --version and stored in the version resource (STE_CANDIDATE_ID)
  [string]$Python = "python",
  [string]$CMakeGenerator = "",         # default: chosen from the Visual Studio version that is used
  [string]$Vcvars = "",
  [switch]$SkipGenerate                # reuse <Out>\gen from an earlier run
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path "$PSScriptRoot\..\..").Path
$Out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $Root $Out }
$Xbe = (Resolve-Path $Xbe).Path
$want = ((Get-Content "$Root\ste\gen\xbe.sha256" | Select-Object -First 1) -split '\s+')[0]
$have = (Get-FileHash $Xbe -Algorithm SHA256).Hash
if ($want -and $have -ne $want) { Write-Warning "default.xbe SHA-256 is $have, expected $want (ste/gen/xbe.sha256). Only the supported build of the game is known to work." }if (-not $SkipGenerate -and (Test-Path "$Out\gen")) { throw "$Out\gen exists; remove it or pass -SkipGenerate" }
if (Test-Path "$Out\ste-recomp.exe") { throw "$Out\ste-recomp.exe exists; remove it first" }
$Obj = "$Out\obj"; $RtBuild = "$Out\runtime"; $SdlBuild = "$Out\sdl3"; $Gen = "$Out\gen"
New-Item -ItemType Directory -Force $Out, $Obj | Out-Null

# --- Visual Studio environment
$VsInstance = ""
if (-not $Vcvars) {
  $vw = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vw)) { throw "vswhere not found; pass -Vcvars <path to vcvars64.bat>" }
  $all = & $vw -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json
  if (-not $all) { throw "no Visual Studio with the C++ x64 tools found" }
  # prefer Visual Studio 2022 (what the published binaries were built with), else the newest
  $pick = @($all | Where-Object { $_.installationVersion -like "17.*" } | Select-Object -First 1) + @($all | Sort-Object { [version]$_.installationVersion } -Descending) | Select-Object -First 1
  $VsInstance = $pick.installationPath
  $Vcvars = "$VsInstance\VC\Auxiliary\Build\vcvars64.bat"
  if (-not $CMakeGenerator) { $CMakeGenerator = @{ "17" = "Visual Studio 17 2022"; "18" = "Visual Studio 18 2026" }[($pick.installationVersion -split '\.')[0]] }
}
if (-not $CMakeGenerator) { $CMakeGenerator = "Visual Studio 17 2022" }
if (-not $VsInstance) { $VsInstance = (Resolve-Path "$(Split-Path $Vcvars)\..\..\..").Path }
$GenArgs = @("-G", $CMakeGenerator, "-A", "x64", "-DCMAKE_GENERATOR_INSTANCE=$VsInstance")
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {   # fall back to the CMake that ships with Visual Studio
  $vsRoot = $VsInstance
  $bundled = "$vsRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
  if (Test-Path "$bundled\cmake.exe") { $env:PATH = "$bundled;$env:PATH" } else { throw "cmake not found: install CMake or add it to PATH" }
}function Invoke-VS([string]$Cmd) {   # run a command line inside the VS developer environment
  $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"   # vcvars prints harmless warnings on stderr
  cmd /c "`"$Vcvars`" >nul 2>nul && $Cmd" 2>&1 | Out-Host
  $code = $LASTEXITCODE; $ErrorActionPreference = $old
  if ($code -ne 0) { throw "failed: $Cmd" }
}

# --- 1. runtime libraries (kernel, d3d8, nv2a, apu, ...)
Write-Host "== runtime libraries"
& cmake -S "$Root\xboxrecomp" -B $RtBuild @GenArgs | Out-Host; if ($LASTEXITCODE) { throw "cmake configure (runtime)" }
& cmake --build $RtBuild --config Release --target xbox_kernel xbox_d3d8 xbox_dsound xbox_input xbox_usb xbox_apu xbox_nv2a xbox_video recomp_exit_hook -j | Out-Host; if ($LASTEXITCODE) { throw "cmake build (runtime)" }

# --- 2. SDL3 (static; gamepad input)
Write-Host "== SDL3"
if (-not $Sdl3Src) {
  $Sdl3Src = "$Root\deps\SDL"
  if (-not (Test-Path $Sdl3Src)) { & git clone --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL $Sdl3Src; if ($LASTEXITCODE) { throw "git clone SDL" } }
}
$Sdl3Src = (Resolve-Path $Sdl3Src).Path
& cmake -S $Sdl3Src -B $SdlBuild @GenArgs -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF `
  -DSDL_AUDIO=OFF -DSDL_CAMERA=OFF -DSDL_RENDER=OFF -DSDL_GPU=OFF -DSDL_DIALOG=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL | Out-Host; if ($LASTEXITCODE) { throw "cmake configure (SDL3)" }
& cmake --build $SdlBuild --config Release -j | Out-Host; if ($LASTEXITCODE) { throw "cmake build (SDL3)" }
$SdlLib = (Get-ChildItem $SdlBuild -Recurse -Filter SDL3-static.lib | Select-Object -First 1).FullName
if (-not $SdlLib) { throw "SDL3-static.lib not found under $SdlBuild" }

# --- 3. generate the recompiled C code from default.xbe
if (-not $SkipGenerate) {
  Write-Host "== generating C from default.xbe (several minutes)"
  $ErrorActionPreference = "Continue"   # the generator prints warnings on stderr; they must not abort the script
  & $Python "$Root\ste\scripts\generate.py" --xbe $Xbe --out $Gen --python $Python 2>&1 | Out-Host; $genExit = $LASTEXITCODE
  $ErrorActionPreference = "Stop"
  if ($genExit) { throw "generate.py failed" }
}
$exp = "$Root\ste\gen\expected_generated.sha256"
if (Test-Path $exp) {   # generation is deterministic: compare every generated file with the recorded hash
  $bad = 0
  foreach ($ln in Get-Content $exp) {
    $w, $n = $ln -split '\s+', 2
    if (-not $w) { continue }
    $p = "$Gen\generated\$($n.Trim().TrimStart('*'))"
    if (-not (Test-Path $p) -or (Get-FileHash $p -Algorithm SHA256).Hash -ne $w) { Write-Warning "generated file differs from the recorded hash: $n"; $bad++ }
  }
  # every generated chunk must be listed: a file that exists but is not in the list was added or patched outside the recipe
  $known = @(Get-Content $exp | ForEach-Object { ($_ -split '\s+', 2)[1] } | Where-Object { $_ } | ForEach-Object { $_.Trim().TrimStart('*') })
  foreach ($gf in Get-ChildItem "$Gen\generated" -Filter "recomp_*.c" -ErrorAction SilentlyContinue) { if ($known -notcontains $gf.Name) { Write-Warning "generated file not in expected_generated.sha256: $($gf.Name)"; $bad++ } }
  if ($bad -eq 0) { Write-Host "generated code matches ste/gen/expected_generated.sha256" }
  elseif ($env:STE_ALLOW_HASH_MISMATCH -eq "1") { Write-Warning "$bad generated file(s) differ from the recipe (STE_ALLOW_HASH_MISMATCH=1: continuing)" }
  else { throw "$bad generated file(s) differ from ste/gen/expected_generated.sha256. Generated code must come only from default.xbe + ste/gen/ste_hooks.toml (+ exclude_manual.c); a hook patched into the C by hand will not exist for anyone else (the D3DDevice_Swap hook was such a case). Fix ste_hooks.toml and regenerate; update the hashes only for an intended change (STE_ALLOW_HASH_MISMATCH=1 bypasses for experiments)." }
}
& $Python "$Root\ste\scripts\verify_generated.py" --root $Gen | Out-Host; if ($LASTEXITCODE) { throw "generated-code validator reported FAIL" }

# --- 4. compile
Write-Host "== compiling"
$X = "$Root\xboxrecomp"; $Src = "$Root\ste\src"
$Inc = @("$Gen\generated", "$X\templates\runtime", "$X\include", "$X\include\xbox", "$X\src", "$X\src\kernel", "$X\src\d3d", "$X\src\audio", "$X\src\input", "$X\src\apu", "$X\src\nv2a", "$X\src\usb", "$X\src\platform") | ForEach-Object { "/I `"$_`"" }
$Inc = $Inc -join " "
"// SPDX-License-Identifier: GPL-2.0-or-later`r`nconst char ste_candidate_id[] = `"STE_CANDIDATE_ID=$BuildNumber;`";`r`n" | Set-Content "$Obj\ste_candidate_id.c" -Encoding ascii
$v = $Version.Split('.')
@"
// SPDX-License-Identifier: GPL-2.0-or-later
#include <winver.h>
VS_VERSION_INFO VERSIONINFO
 FILEVERSION $($v[0]),$($v[1]),$($v[2]),$BuildNumber
 PRODUCTVERSION $($v[0]),$($v[1]),$($v[2]),$BuildNumber
 FILEFLAGSMASK 0x3fL
 FILEFLAGS 0x0L
 FILEOS VOS_NT_WINDOWS32
 FILETYPE VFT_APP
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "CompanyName", "Godzilla STE Recomp project"
   VALUE "FileDescription", "Godzilla: Save the Earth - static recompilation"
   VALUE "FileVersion", "$Version.$BuildNumber"
   VALUE "InternalName", "ste-recomp"
   VALUE "LegalCopyright", "GPL-2.0-or-later. Contains no game code or data."
   VALUE "OriginalFilename", "ste-recomp.exe"
   VALUE "ProductName", "ste-recomp"
   VALUE "ProductVersion", "$Version+c$BuildNumber"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
"@ | Set-Content "$Obj\ste_version.rc" -Encoding ascii
$gdef = "/nologo /c /W3 /Zi /MD /D RECOMP_GENERATED_CODE /D WIN32_LEAN_AND_MEAN /bigobj $Inc"
$chunks = (Get-ChildItem "$Gen\generated\recomp_0*.c" | Sort-Object Name | ForEach-Object { "`"$($_.FullName)`"" }) -join " "
$c0 = "`"$Gen\generated\recomp_0000.c`""; $rest = ((Get-ChildItem "$Gen\generated\recomp_0*.c" | Where-Object Name -ne "recomp_0000.c" | Sort-Object Name | ForEach-Object { "`"$($_.FullName)`"" }) -join " ")
# the generated chunks are optimised (/O2); recomp_0000.c carries sub_0001F740, which recomp_manual.c wraps (the original stays reachable as ste_orig_sub_0001F740)
$bat = @(
  "cl $gdef /FS /MP /O2 /Fo`"$Obj\\`" $rest",
  "cl $gdef /FS /O2 /D sub_0001F740=ste_orig_sub_0001F740 /Fo`"$Obj\recomp_0000.obj`" $c0",
  "cl $gdef /Od /Fo`"$Obj\recomp_dispatch.obj`" `"$Gen\generated\recomp_dispatch.c`"",
  "cl $gdef /Od /Fo`"$Obj\recomp_stubs_unresolved.obj`" `"$Gen\generated\recomp_stubs_unresolved.c`""
)
$hdef = "/nologo /c /W3 /Od /Zi /MD /D WIN32_LEAN_AND_MEAN $Inc"
$bat += "cl $hdef /D STE_VERSION=\`"$Version\`" /Fo`"$Obj\main.obj`" `"$Src\main.c`""
$bat += "cl $hdef /Fo`"$Obj\recomp_manual.obj`" `"$Src\recomp_manual.c`""
$bat += "cl $hdef /Fo`"$Obj\ste_pb_hook.obj`" `"$Src\ste_pb_hook.c`""
$bat += "cl $hdef /I `"$Sdl3Src\include`" /Fo`"$Obj\ste_sdlinput.obj`" `"$Src\ste_sdlinput.c`""
$bat += "cl $hdef /Fo`"$Obj\ste_candidate_id.obj`" `"$Obj\ste_candidate_id.c`""
$bat += "rc /nologo /fo `"$Obj\ste_version.res`" `"$Obj\ste_version.rc`""
foreach ($c in $bat) { Invoke-VS $c }

# --- 5. link
Write-Host "== linking"
$L = "$RtBuild\src"
$libs = @("apu\Release\xbox_apu.lib", "audio\Release\xbox_dsound.lib", "d3d\Release\xbox_d3d8.lib", "input\Release\xbox_input.lib", "kernel\Release\xbox_kernel.lib",
          "nv2a\Release\xbox_nv2a.lib", "usb\Release\xbox_usb.lib", "video\Release\xbox_video.lib", "platform\Release\recomp_exit_hook.lib") | ForEach-Object { "`"$L\$_`"" }
$objs = (Get-ChildItem "$Obj\*.obj" | ForEach-Object { "`"$($_.FullName)`"" }) -join " "
$sys = "setupapi.lib imm32.lib version.lib oleaut32.lib advapi32.lib cfgmgr32.lib psapi.lib d3d11.lib dxgi.lib dxguid.lib xinput.lib winmm.lib dbghelp.lib user32.lib gdi32.lib kernel32.lib shell32.lib ole32.lib d3dcompiler.lib"
Invoke-VS "link /nologo /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /DEBUG /LARGEADDRESSAWARE /PDBALTPATH:ste-recomp.pdb /OUT:`"$Out\ste-recomp.exe`" $objs `"$Obj\ste_version.res`" $($libs -join ' ') `"$SdlLib`" $sys"
$h = (Get-FileHash "$Out\ste-recomp.exe" -Algorithm SHA256).Hash
Write-Host "built $Out\ste-recomp.exe  SHA-256 $h"