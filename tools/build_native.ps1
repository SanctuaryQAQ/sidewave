param([string]$EngineRoot = (Join-Path $PSScriptRoot '..\vendor\pyrowave'))
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output=Join-Path $root 'build\bin'
New-Item -ItemType Directory -Path $output -Force|Out-Null
$programsX86=[Environment]::GetFolderPath('ProgramFilesX86')
$vswhere=Join-Path $programsX86 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs= & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $vs){throw 'Install Visual Studio 2022 C++ tools and Windows SDK.'}
$vc=Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory|Sort-Object Name -Descending|Select-Object -First 1
$sdk=Get-ChildItem (Join-Path $programsX86 'Windows Kits\10\Include') -Directory|Where-Object {Test-Path (Join-Path $_.FullName 'um\winsock2.h')}|Sort-Object Name -Descending|Select-Object -First 1
$sdkBase=Split-Path (Split-Path $sdk.FullName)
$env:PATH="$($vc.FullName)\bin\Hostx64\x64;$env:PATH"
$env:INCLUDE="$($vc.FullName)\include;$($sdk.FullName)\ucrt;$($sdk.FullName)\shared;$($sdk.FullName)\um;$($sdk.FullName)\winrt"
$env:LIB="$($vc.FullName)\lib\x64;$sdkBase\Lib\$($sdk.Name)\ucrt\x64;$sdkBase\Lib\$($sdk.Name)\um\x64"
$lib=Join-Path $EngineRoot 'build-minimal\Release\pyrowave-shared.lib'
if(-not(Test-Path $lib)){throw 'Build the pinned PyroWave shared library first.'}
$includes=@("/I$EngineRoot","/I$(Join-Path $EngineRoot 'Granite\third_party\khronos\vulkan-headers\include')")
Push-Location $output
try{
 foreach($job in @(@('pyrowave_2k165_gpu_bench.cpp','PyroScreen-Sender.exe'),@('pyrowave_udp_client.cpp','PyroScreen-Receiver.exe'))){
  & cl.exe /nologo /O2 /MT /EHsc /std:c++17 @includes (Join-Path $root "native\$($job[0])") "/Fe:$($job[1])" /link $lib d3d11.lib d3d12.lib d3dcompiler.lib dxgi.lib ws2_32.lib user32.lib gdi32.lib /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
  if($LASTEXITCODE){throw "Native build failed: $($job[0])"}
 }
 & cl.exe /nologo /O2 /MT /EHsc /std:c++17 (Join-Path $root 'native\display_lease.cpp') /Fe:PyroScreen-Display.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
 if($LASTEXITCODE){throw 'Display helper build failed'}
 & cl.exe /nologo /O2 /MT /EHsc /std:c++17 (Join-Path $root 'tests\crypto_test.cpp') /Fe:CryptoTests.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup
 if($LASTEXITCODE){throw 'Crypto test build failed'}
 Copy-Item (Join-Path $EngineRoot 'build-minimal\Release\libpyrowave-shared-1.dll') $output -Force
}finally{Pop-Location}
