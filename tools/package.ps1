param([string]$DistRoot='')
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dist=if($DistRoot){[IO.Path]::GetFullPath($DistRoot)}else{Join-Path $root 'dist'}
$bin=Join-Path $root 'build\bin'
New-Item -ItemType Directory -Path $dist -Force|Out-Null
$artifactArchive=$null
function Save-PackageArtifact([string]$Path){
 if(-not(Test-Path -LiteralPath $Path)){return}
 $prefix=$dist.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
 if(-not [IO.Path]::GetFullPath($Path).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Artifact archive source is outside DistRoot.'}
 if((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Cannot archive an artifact reparse point.'}
 if(-not $script:artifactArchive){
  $script:artifactArchive=Join-Path $dist ('archive\Packages-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'Z-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
  if(-not [IO.Path]::GetFullPath($script:artifactArchive).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Artifact archive target is outside DistRoot.'}
  New-Item -ItemType Directory -Path $script:artifactArchive -Force|Out-Null
 }
 $target=Join-Path $script:artifactArchive (Split-Path $Path -Leaf)
 if(Test-Path -LiteralPath $target){throw 'Artifact archive target already exists.'}
 Move-Item -LiteralPath $Path -Destination $target
}
& {
 $staging=Join-Path $dist ('.package-'+[Guid]::NewGuid().ToString('N'))
 $out=Join-Path $staging 'PyroScreen'
 New-Item -ItemType Directory -Path $out -Force|Out-Null
 try {
 $files=@('PyroScreen.exe','MainWindow.xaml','app.png','app.ico','host.png','client.png','host.ico','client.ico','libpyrowave-shared-1.dll')
 $files+=@('PyroScreen-Sender.exe','PyroScreen-Receiver.exe','PyroScreen-Display.exe','Install-Driver.ps1','Install-Network.ps1','Inspect-System.ps1')
 $files+='PresentMon.exe'
 foreach($f in $files){
  $inputFile=Join-Path $bin $f;$outputFile=Join-Path $out $f
  if((Test-Path -LiteralPath $outputFile) -and (Get-FileHash -LiteralPath $inputFile).Hash -eq (Get-FileHash -LiteralPath $outputFile).Hash){continue}
  Copy-Item -LiteralPath $inputFile -Destination $out -Force
 }
 Copy-Item -LiteralPath (Join-Path $bin 'driver') -Destination $out -Recurse -Force
 Copy-Item (Join-Path $root 'README.md'),(Join-Path $root 'LICENSE') $out -Force
 Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $out -Recurse -Force
 Copy-Item -LiteralPath (Join-Path $root 'third_party\licenses') -Destination $out -Recurse -Force
 $hashes=Get-ChildItem $out -Recurse -File|Where-Object {$_.Name -ne 'SHA256SUMS.txt'}|ForEach-Object {
  $relative=$_.FullName.Substring($out.Length+1)
  "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
 }
 $hashes|Set-Content (Join-Path $out 'SHA256SUMS.txt') -Encoding ASCII
 & (Join-Path $PSScriptRoot 'publish_release.ps1') -SourceDirectory $out -ReleaseRoot $dist
 $out=Join-Path $dist 'PyroScreen'
 Save-PackageArtifact (Join-Path $dist 'PyroScreen-0.1-preview.zip')
 Compress-Archive -Path $out -DestinationPath (Join-Path $dist 'PyroScreen-0.1-preview.zip')
 } finally {
  $prefix=$dist.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
  if(-not [IO.Path]::GetFullPath($staging).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Package staging directory is outside DistRoot.'}
  if(Test-Path -LiteralPath $staging){Remove-Item -LiteralPath $staging -Recurse -Force}
 }
}
$source=Join-Path $dist 'PyroScreen-Source'
Save-PackageArtifact $source
New-Item -ItemType Directory -Path $source -Force|Out-Null
foreach($d in @('src','native','assets','docs','.github')){if(Test-Path (Join-Path $root $d)){Copy-Item -LiteralPath (Join-Path $root $d) -Destination $source -Recurse -Force}}
New-Item -ItemType Directory -Path (Join-Path $source 'tools'),(Join-Path $source 'third_party'),(Join-Path $source 'tests') -Force|Out-Null
foreach($f in @('SenderRecoveryTests.cs','verify_sender_recovery.ps1')){Copy-Item -LiteralPath (Join-Path $root "tests\$f") -Destination (Join-Path $source 'tests') -Force}
Copy-Item -LiteralPath (Join-Path $root 'tests\test_publish_release.ps1') -Destination (Join-Path $source 'tests') -Force
foreach($f in @('MaintenanceUiTests.cs','test_network_setup.ps1')){if(Test-Path (Join-Path $root "tests\$f")){Copy-Item -LiteralPath (Join-Path $root "tests\$f") -Destination (Join-Path $source 'tests') -Force}}
foreach($f in @('analyze_windows.py','build_app.ps1','build_native.ps1','find_cmake.ps1','get_driver.ps1','get_presentmon.ps1','package.ps1','publish_release.ps1','Install-Driver.ps1','Install-Network.ps1','Inspect-System.ps1','render_icons.py')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $f) -Destination (Join-Path $source 'tools') -Force}
foreach($f in @('StatusProbe.cs','DiscoveryTests.cs','PrimaryDisplayProbe.cs','ProtocolTests.cs','TwoMachineTests.cs','crypto_test.cpp','rio_crypto_transport_test.cpp','build_tests.ps1','PairCodeProbe.cs','GuiProbe.cs','GuiActions.cs','HostCursorProbe.cs','ClientCursorProbe.cs','DesktopGpuDocument.cpp')){Copy-Item -LiteralPath (Join-Path $root "tests\$f") -Destination (Join-Path $source 'tests') -Force}
foreach($f in @('measure_constant_cadence.ps1','analyze_constant_cadence.py','validate_full_chain_hud.ps1','measure_165_frame_drops.ps1','analyze_165_frame_drops.py','frame_datagram_schedule_test.cpp','datagram_slot_pool_test.cpp','datagram_sender_loopback_test.cpp','decoder_sequence_regression_test.cpp','verify_frame_schedule.ps1','ReconnectLeaseTests.cs','verify_reconnect_lease.ps1')){Copy-Item -LiteralPath (Join-Path $root "tests\$f") -Destination (Join-Path $source 'tests') -Force}
Copy-Item -LiteralPath (Join-Path $root 'third_party\licenses') -Destination (Join-Path $source 'third_party') -Recurse -Force
Copy-Item (Join-Path $root 'README.md'),(Join-Path $root 'LICENSE'),(Join-Path $root '.gitignore'),(Join-Path $root 'CMakeLists.txt'),(Join-Path $root 'build.ps1') $source -Force
$sourceHashes=Get-ChildItem $source -Recurse -File|Where-Object {$_.Name -ne 'SHA256SUMS.txt'}|ForEach-Object {
 "$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $($_.FullName.Substring($source.Length+1))"
}
$sourceHashes|Set-Content (Join-Path $source 'SHA256SUMS.txt') -Encoding ASCII
Save-PackageArtifact (Join-Path $dist 'PyroScreen-Source-0.1-preview.zip')
Compress-Archive -Path $source -DestinationPath (Join-Path $dist 'PyroScreen-Source-0.1-preview.zip')
