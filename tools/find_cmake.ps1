# Returns cmake.exe from PATH, falling back to the CMake bundled with Visual Studio.
$ErrorActionPreference='Stop'
$cmake=(Get-Command cmake -ErrorAction SilentlyContinue).Source
if($cmake){return $cmake}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(Test-Path -LiteralPath $vswhere){$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath}
if($vs){$cmake=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'}
if(-not $cmake -or -not(Test-Path -LiteralPath $cmake)){throw 'CMake not found: install CMake or the Visual Studio "C++ CMake tools" component'}
$cmake
