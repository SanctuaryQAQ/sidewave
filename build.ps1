param([string]$EngineRoot='', [switch]$UseExistingEngineBuild, [switch]$SkipDriverDownload)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if($UseExistingEngineBuild){
 if(-not $EngineRoot){throw 'Specify -EngineRoot when reusing a built shared library.'}
 & (Join-Path $root 'tools\build_native.ps1') -EngineRoot $EngineRoot
}else{
 $vendor=Join-Path $root 'vendor\pyrowave'
 if(-not(Test-Path $vendor)){
  New-Item -ItemType Directory -Path (Split-Path $vendor) -Force|Out-Null
  & git clone --no-checkout https://github.com/Themaister/pyrowave.git $vendor
  if($LASTEXITCODE){throw 'PyroWave clone failed'}
 }
 & git -C $vendor checkout a0d2e71e98b85f6a23f62407df21a39e9745b875
 if($LASTEXITCODE){throw 'Cannot select pinned PyroWave commit'}
 $granite=Join-Path $vendor 'Granite'
 if(-not(Test-Path (Join-Path $granite '.git'))){
  & git clone --no-checkout https://github.com/Themaister/Granite.git $granite
  if($LASTEXITCODE){throw 'Granite clone failed'}
 }
 & git -C $granite checkout fb178c8080d163419e8d20f10715c61c53c1ec9b
 if($LASTEXITCODE){throw 'Cannot select pinned Granite commit'}
 & git -C $granite submodule update --init third_party/volk third_party/khronos/vulkan-headers
 if($LASTEXITCODE){throw 'Granite header submodule checkout failed'}
 $cmake=& (Join-Path $root 'tools\find_cmake.ps1')
 & $cmake -S $root -B (Join-Path $root 'build\cmake') -A x64
 if($LASTEXITCODE){throw 'CMake configure failed'}
 & $cmake --build (Join-Path $root 'build\cmake') --config Release --target PyroScreen-Sender PyroScreen-Receiver PyroScreen-Display
 if($LASTEXITCODE){throw 'Native compilation failed'}
 $bin=Join-Path $root 'build\bin';New-Item -ItemType Directory -Path $bin -Force|Out-Null
 Get-ChildItem (Join-Path $root 'build\cmake') -Recurse -File|Where-Object {($_.Name -like 'PyroScreen-*.exe' -or $_.Name -eq 'libpyrowave-shared-1.dll') -and $_.Directory.Name -eq 'Release'}|Copy-Item -Destination $bin -Force
}
& (Join-Path $root 'tools\build_app.ps1')
if(-not $SkipDriverDownload){& (Join-Path $root 'tools\get_driver.ps1')}
& (Join-Path $root 'tools\package.ps1')
