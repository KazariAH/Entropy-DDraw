# Builds ddraw.dll (Release|Win32, /MT) with MSBuild from VS 2022 and runs the import self-check.
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1 [-UnitTest] [-Test] [-Deploy]
#   -UnitTest: window-free checks only (tests\childtest.cpp); -Test: those plus the selftest (hidden window + GL)
param([switch]$Deploy, [switch]$Test, [switch]$UnitTest)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio 2022 with C++ tools not found' }
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'

& $msbuild (Join-Path $root 'ddraw.vcxproj') /nologo /m /t:Rebuild /p:Configuration=Release /p:Platform=Win32 /v:minimal
if ($LASTEXITCODE -ne 0) { throw "MSBuild failed ($LASTEXITCODE)" }
Copy-Item (Join-Path $root 'build\ddraw.dll') (Join-Path $root 'ddraw.dll') -Force

# import self-check: only kernel32/user32/gdi32/opengl32/advapi32 are allowed
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$env:PATH = "$installer;$env:PATH"
$imports = cmd /c "`"$vcvars`" x86 >nul && dumpbin /nologo /imports `"$root\ddraw.dll`""
$imports | Set-Content (Join-Path $root 'imports.txt') -Encoding utf8
$dlls = $imports | Select-String -Pattern '^\s{4}(\S+\.dll)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.ToLower() }
$allowed = 'kernel32.dll','user32.dll','gdi32.dll','opengl32.dll','advapi32.dll'
if (-not $dlls) { throw 'dumpbin produced no import list' }
$bad = $dlls | Where-Object { $allowed -notcontains $_ }
Write-Host "Imported DLLs: $($dlls -join ', ')"
if ($bad) { throw "Disallowed imports: $($bad -join ', ')" }
Write-Host 'Import check OK'

if ($Test -or $UnitTest) {
    # window-free checks of the fixchilds parts (no window, no OpenGL: memory DCs only)
    $obj = Join-Path $root 'build\obj'
    $cexe = Join-Path $root 'build\childtest.exe'
    cmd /c "`"$vcvars`" x86 >nul && cl /nologo /W4 /WX /EHsc /O2 /MT /D_WIN32_WINNT=0x0601 /Fo`"$obj\\`" /Fe`"$cexe`" `"$root\tests\childtest.cpp`" `"$root\src\childwin.cpp`" user32.lib gdi32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'childtest build failed' }
    & $cexe
    if ($LASTEXITCODE -ne 0) { throw 'childtest failed' }
    # Q149: present planning (write-watch pages -> row bands, present coalescing)
    $fexe = Join-Path $root 'build\frametest.exe'
    cmd /c "`"$vcvars`" x86 >nul && cl /nologo /W4 /WX /EHsc /O2 /MT /Fo`"$obj\\`" /Fe`"$fexe`" `"$root\tests\frametest.cpp`" `"$root\src\frameplan.cpp`""
    if ($LASTEXITCODE -ne 0) { throw 'frametest build failed' }
    & $fexe
    if ($LASTEXITCODE -ne 0) { throw 'frametest failed' }
    # Q150: copy kernels and stretch columns (the timing is tests\blitbench.cpp, run by hand)
    $bexe = Join-Path $root 'build\blittest.exe'
    cmd /c "`"$vcvars`" x86 >nul && cl /nologo /W4 /WX /EHsc /O2 /MT /Fo`"$obj\\`" /Fe`"$bexe`" `"$root\tests\blittest.cpp`" `"$root\src\blit.cpp`""
    if ($LASTEXITCODE -ne 0) { throw 'blittest build failed' }
    & $bexe
    if ($LASTEXITCODE -ne 0) { throw 'blittest failed' }
}

if ($Test) {
    # self-test of paths the game does not use (8-bit palette, flip chain, colour keys, ...)
    $obj = Join-Path $root 'build\obj'
    $exe = Join-Path $root 'build\selftest.exe'
    cmd /c "`"$vcvars`" x86 >nul && cl /nologo /W4 /WX /EHsc /O2 /MT /D_WIN32_WINNT=0x0601 /Fo`"$obj\\`" /Fe`"$exe`" `"$root\tests\selftest.cpp`" user32.lib gdi32.lib"
    if ($LASTEXITCODE -ne 0) { throw 'selftest build failed' }
    & $exe (Join-Path $root 'build\ddraw.dll')
    if ($LASTEXITCODE -ne 0) { throw 'selftest failed' }
    # second pass with flipclear=true (ddraw.ini next to the DLL, removed afterwards)
    $ini = Join-Path $root 'build\ddraw.ini'
    Set-Content $ini "[ddraw]`r`nflipclear=true`r`nlog=false" -Encoding ascii
    $env:QDD_FLIPCLEAR = '1'
    try { & $exe (Join-Path $root 'build\ddraw.dll') } finally { Remove-Item Env:QDD_FLIPCLEAR; Remove-Item $ini -Force }
    if ($LASTEXITCODE -ne 0) { throw 'selftest (flipclear=true) failed' }
    # third pass: guard_lines=true (deliberate writes past a surface must be reported and repaired) and a
    # limited mode list (resolutions + max_resolutions); the alarms are read back from build\ddraw.log
    Set-Content $ini "[ddraw]`r`nguard_lines=true`r`nlog=true`r`nwritewatch=true`r`nresolutions=2560x1600,1920x1080,1280x1024,640x480`r`nmax_resolutions=2" -Encoding ascii
    $env:QDD_GUARD = '1'
    try { & $exe (Join-Path $root 'build\ddraw.dll') } finally { Remove-Item Env:QDD_GUARD; Remove-Item $ini -Force }
    if ($LASTEXITCODE -ne 0) { throw 'selftest (guard_lines=true) failed' }
}

if ($Deploy) {
    Copy-Item (Join-Path $root 'ddraw.dll') (Join-Path $root 'game\ddraw.dll') -Force
    Copy-Item (Join-Path $root 'ddraw.ini') (Join-Path $root 'game\ddraw.ini') -Force
    Write-Host 'Deployed to game\'
}
