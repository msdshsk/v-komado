[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$build = Join-Path $root 'artifacts/regression-build'
& $cmake -S "$root/tests" -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'Test configure failed.' }
& $cmake --build $build --config Release
if ($LASTEXITCODE -ne 0) { throw 'Test build failed.' }
& "$build/Release/hotkeys_tests.exe" "$root/artifacts" "$root/artifacts/viewer-build/Release/lgwl_viewer.exe" 2>&1 | Tee-Object "$root/artifacts/hotkeys-test-results.txt"
if ($LASTEXITCODE -ne 0) { throw 'Shortcut tests failed.' }
$log = Join-Path $root ('artifacts/viewer-regression-{0}.log' -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
Write-Host 'This integration test controls only its own viewer window. Close the viewer before running.'
$msvc = Get-ChildItem "$vs/VC/Tools/MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$dumpbin = Join-Path $msvc.FullName 'bin/Hostx64/x64/dumpbin.exe'
$imports = & $dumpbin /imports "$root/artifacts/viewer-build/Release/lgwl_viewer.exe"
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect viewer imports.' }
if ($imports -match '\b(ClipCursor|SendInput|SetWindowsHookExW|SetCapture)\b') {
    throw 'Viewer must not import input confinement, injection, or hook APIs.'
}
Write-Host 'PASS viewer contains no input confinement, injection, or hook imports'
& "$build/Release/viewer_regression.exe" "$root/artifacts/viewer-build/Release/lgwl_viewer.exe" $log 2>&1 |
    Tee-Object "$root/artifacts/viewer-regression-results.txt"
if ($LASTEXITCODE -ne 0) { throw "Regression failed. Review $log" }
& "$build/Release/rescue_regression.exe" "$root/artifacts/viewer-build/Release/lgwl_viewer.exe" "$root/artifacts/rescue-regression.log" 2>&1 |
    Tee-Object "$root/artifacts/rescue-regression-results.txt"
if ($LASTEXITCODE -ne 0) { throw 'Window rescue regression failed.' }
& "$build/Release/picker_regression.exe" "$root/artifacts/viewer-build/Release/lgwl_viewer.exe" "$root/artifacts/picker-regression.log" "$root/artifacts/picker-ui.png" 2>&1 |
    Tee-Object "$root/artifacts/picker-regression-results.txt"
if ($LASTEXITCODE -ne 0) { throw 'Window picker regression failed.' }
& "$build/Release/render_benchmark.exe" "$root/artifacts/viewer-build/Release/lgwl_viewer.exe" "$root/artifacts/drag-background-regression.log" --drag-after-motion 2>&1 |
    Tee-Object "$root/artifacts/drag-background-regression-results.txt"
if ($LASTEXITCODE -ne 0) { throw 'Dragging after another app has focus failed.' }
