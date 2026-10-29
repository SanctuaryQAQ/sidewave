$ErrorActionPreference='Stop'
$publisher=Join-Path $PSScriptRoot '..\tools\publish_release.ps1'
$fixture=Join-Path ([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build'))) ('release-test-'+[Guid]::NewGuid().ToString('N'))
$release=Join-Path $fixture 'dist'
$source=Join-Path $fixture 'bundle'
$checks=0
function Assert([bool]$Condition,[string]$Message){
 if(-not $Condition){throw "Assertion failed: $Message"}
 $script:checks++
}
function Write-Manifest {
 Get-ChildItem -LiteralPath $source -Recurse -File|Where-Object {$_.Name -ne 'SHA256SUMS.txt'}|ForEach-Object {
  "$((Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant())  $($_.FullName.Substring($source.Length+1))"
 }|Set-Content -LiteralPath (Join-Path $source 'SHA256SUMS.txt') -Encoding ASCII
}
function Publish {& $publisher -SourceDirectory $source -ReleaseRoot $release}
try {
 New-Item -ItemType Directory -Path $source -Force|Out-Null
 foreach($file in @('PyroScreen.exe','MainWindow.xaml','libpyrowave-shared-1.dll','PyroScreen-Sender.exe','PyroScreen-Receiver.exe','PyroScreen-Display.exe','obsolete.txt')){
  'version-one'|Set-Content -LiteralPath (Join-Path $source $file) -Encoding ASCII
 }
 Write-Manifest
 $first=Publish
 $target=$first.Executable
 Assert ($first.Status -eq 'Updated' -and $null -eq $first.Archive) 'first installation needs no archive'
 Assert ((Get-Content -LiteralPath $target -Raw).Trim() -eq 'version-one') 'first installation content'
 $same=Publish
 Assert ($same.Status -eq 'Unchanged') 'identical update is skipped'
 Assert (-not(Test-Path -LiteralPath (Join-Path $release 'archive'))) 'identical update does not create an archive'

 'version-two'|Set-Content -LiteralPath (Join-Path $source 'PyroScreen.exe') -Encoding ASCII
 Remove-Item -LiteralPath (Join-Path $source 'obsolete.txt')
 Write-Manifest
 $second=Publish
 Assert ($second.Executable -eq $target) 'executable path is unchanged'
 Assert ((Get-Content -LiteralPath $target -Raw).Trim() -eq 'version-two') 'new release replaces current release'
 Assert ((Get-Content -LiteralPath (Join-Path $second.Archive 'PyroScreen.exe') -Raw).Trim() -eq 'version-one') 'old executable is archived intact'
 Assert (Test-Path -LiteralPath (Join-Path $second.Archive 'obsolete.txt')) 'complete old bundle is archived'
 Assert (-not(Test-Path -LiteralPath (Join-Path (Split-Path $target) 'obsolete.txt'))) 'removed files do not survive in the new release'

 'corrupt-without-updating-manifest'|Set-Content -LiteralPath (Join-Path $source 'PyroScreen.exe') -Encoding ASCII
 $rejected=$false
 try{Publish|Out-Null}catch{$rejected=$_.Exception.Message -like '*checksum mismatch*'}
 Assert $rejected 'corrupt source is rejected'
 Assert ((Get-Content -LiteralPath $target -Raw).Trim() -eq 'version-two') 'failed validation preserves installed release'

 Write-Manifest
 $held=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 try {
  $rejected=$false
  try{Publish|Out-Null}catch{$rejected=$_.Exception.Message -like '*Locked executable*'}
  Assert $rejected 'locked executable is rejected before the swap'
 } finally {$held.Dispose()}
 Assert ((Get-Content -LiteralPath $target -Raw).Trim() -eq 'version-two') 'locked update leaves current release intact'
 Assert (@(Get-ChildItem -LiteralPath (Join-Path $release 'archive') -Directory).Count -eq 1) 'failed updates create no extra archives'

 # Simulate a failure after archiving the old directory, during the final rename.
 function Move-Item {
  [CmdletBinding()]param([string]$LiteralPath,[string]$Destination)
  if((Split-Path $LiteralPath -Leaf) -like '.release-*' -and $Destination -eq (Split-Path $target)){
   throw 'Simulated final rename failure.'
  }
  Microsoft.PowerShell.Management\Move-Item -LiteralPath $LiteralPath -Destination $Destination
 }
 try {
  $rejected=$false
  try{Publish|Out-Null}catch{$rejected=$_.Exception.Message -eq 'Simulated final rename failure.'}
  Assert $rejected 'final rename failure is reported'
 } finally {Remove-Item -LiteralPath Function:\Move-Item}
 Assert ((Get-Content -LiteralPath $target -Raw).Trim() -eq 'version-two') 'failed swap restores previous release'
 Assert (@(Get-ChildItem -LiteralPath (Join-Path $release 'archive') -Directory).Count -eq 1) 'rollback does not consume the previous archive'

 $manifest=Join-Path $source 'SHA256SUMS.txt'
 Add-Content -LiteralPath $manifest -Value (('0'*64)+'  ..\outside.exe') -Encoding ASCII
 $rejected=$false
 try{Publish|Out-Null}catch{$rejected=$_.Exception.Message -like '*checksum mismatch*'}
 Assert $rejected 'manifest cannot reference a file outside the bundle'
 Assert (@(Get-ChildItem -LiteralPath $release -Directory -Filter '.release-*').Count -eq 0) 'staging directories are cleaned'
 [pscustomobject]@{Passed=$true;Checks=$checks}
} finally {
 if(Test-Path -LiteralPath Function:\Move-Item){Remove-Item -LiteralPath Function:\Move-Item}
 $build=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build')).TrimEnd('\')+'\'
 if(-not $fixture.StartsWith($build,[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture cleanup is outside the build directory.'}
 if(Test-Path -LiteralPath $fixture){Remove-Item -LiteralPath $fixture -Recurse -Force}
}
