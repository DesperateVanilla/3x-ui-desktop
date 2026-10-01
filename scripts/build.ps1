param([string]$QtRoot = '', [switch]$TestsOnly, [switch]$Package, [string]$DistName = '3X-Control-0.2')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
if ($DistName -notmatch '^3X-Control(?:-[0-9][0-9a-zA-Z.-]*)?$') { throw 'DistName должен быть именем папки 3X-Control или 3X-Control-<версия>.' }
function Invoke-BuildTool([string]$Executable, [string[]]$ToolArguments) {
    # Windows build tools reject duplicated case variants of inherited env keys.
    $toolEnvironment = [System.Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $toolEnvironment[$entry.Key.ToUpperInvariant()] = [string]$entry.Value }
    $toolEnvironment['PATH'] = $env:PATH
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.WorkingDirectory = $taskRoot
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.Environment.Clear()
    foreach ($entry in $toolEnvironment.GetEnumerator()) { $startInfo.Environment[$entry.Key] = $entry.Value }
    foreach ($argument in $ToolArguments) { $startInfo.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($startInfo)
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    Write-Output $stdoutTask.GetAwaiter().GetResult()
    $errorOutput = $stderrTask.GetAwaiter().GetResult()
    if ($errorOutput) { Write-Output $errorOutput }
    if ($process.ExitCode) { throw "Инструмент $(Split-Path -Leaf $Executable) завершился с кодом $($process.ExitCode)." }
}
if (-not $QtRoot) { $QtRoot = Join-Path $taskRoot '.tools\Qt\6.8.3\msvc2022_64' }
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) { throw 'Qt не найден. Укажите -QtRoot C:\Qt\6.8.3\msvc2022_64.' }
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakeExe = $cmakeCommand.Source } else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Установите Visual Studio Build Tools с Desktop development with C++.' }
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakeExe = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakeExe)) { throw 'CMake не найден.' }
$ctestExe = Join-Path (Split-Path -Parent $cmakeExe) 'ctest.exe'
$buildRoot = Join-Path $taskRoot 'build'
Invoke-BuildTool $cmakeExe @('-S',$taskRoot,'-B',$buildRoot,"-DCMAKE_PREFIX_PATH=$QtRoot",'-DBUILD_TESTING=ON')
if ($TestsOnly) { Invoke-BuildTool $cmakeExe @('--build',$buildRoot,'--config','Release','--target','fleet_core_tests','fleet_storage_tests','fleet_dialog_tests','fleet_provision_tests','fleet_backup_tests','--parallel','4') } else { Invoke-BuildTool $cmakeExe @('--build',$buildRoot,'--config','Release','--parallel','4') }
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
Invoke-BuildTool $ctestExe @('--test-dir',$buildRoot,'-C','Release','--output-on-failure')
if ($Package) {
    $distRoot = Join-Path (Join-Path $taskRoot 'dist') $DistName
    New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $buildRoot 'Release\3X-Control.exe') -Destination $distRoot
    Invoke-BuildTool (Join-Path $QtRoot 'bin\windeployqt.exe') @('--release','--no-translations','--no-opengl-sw','--no-compiler-runtime','--skip-plugin-types','generic','--include-plugins','qsqlite',(Join-Path $distRoot '3X-Control.exe'))
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $runtimeVsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $runtimeRoot = Join-Path $runtimeVsRoot 'VC\Redist\MSVC'
    $crtRoot = Get-ChildItem -LiteralPath $runtimeRoot -Directory | Where-Object { $_.Name -match '^\d' } | Sort-Object { [version]$_.Name } -Descending | ForEach-Object { Get-ChildItem -LiteralPath (Join-Path $_.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' } | Select-Object -First 1
    if (-not $crtRoot) { throw 'MSVC runtime не найден; установите компонент C++ Redistributable в Visual Studio.' }
    Get-ChildItem -LiteralPath $crtRoot.FullName -File -Filter '*.dll' | Copy-Item -Destination $distRoot
    Copy-Item -LiteralPath (Join-Path $taskRoot 'README.md') -Destination $distRoot
    Copy-Item -LiteralPath (Join-Path $taskRoot 'THIRD-PARTY-NOTICES.md') -Destination $distRoot
    Copy-Item -LiteralPath (Join-Path $taskRoot 'licenses') -Destination $distRoot -Recurse -Force
    Write-Output "Приложение готово: $distRoot\3X-Control.exe"
}
