param(
 [Parameter(Mandatory=$true)][string]$SourceDirectory,
 [string]$ReleaseRoot=''
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$release=if($ReleaseRoot){[IO.Path]::GetFullPath($ReleaseRoot)}else{Join-Path $project 'dist'}
$source=[IO.Path]::GetFullPath($SourceDirectory)
$current=Join-Path $release 'PyroScreen'
$archives=Join-Path $release 'archive'
$stage=Join-Path $release ('.release-'+[Guid]::NewGuid().ToString('N'))

function Assert-ChildPath([string]$Path){
 $prefix=$release.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
 if(-not [IO.Path]::GetFullPath($Path).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){
  throw "Path is outside the release directory: $Path"
 }
}
function Get-BundleFiles([string]$Directory){
 if(-not(Test-Path -LiteralPath $Directory -PathType Container)){throw "Bundle directory is missing: $Directory"}
 $pending=New-Object 'Collections.Generic.Stack[string]'
 $pending.Push($Directory)
 while($pending.Count){
  $folder=Get-Item -LiteralPath $pending.Pop() -Force
  if($folder.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse points are not allowed in a release: $($folder.FullName)"}
  foreach($item in Get-ChildItem -LiteralPath $folder.FullName -Force){
   if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse points are not allowed in a release: $($item.FullName)"}
   if($item.PSIsContainer){$pending.Push($item.FullName)}else{$item}
  }
 }
}
function Get-Inventory([string]$Directory){
 $inventory=@{}
 foreach($file in Get-BundleFiles $Directory){
  $name=$file.FullName.Substring($Directory.TrimEnd('\','/').Length+1)
  if($name -eq 'SHA256SUMS.txt'){continue}
  $inventory[$name]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
 }
 return $inventory
}
function Assert-Manifest([string]$Directory,$Inventory){
 $manifest=Join-Path $Directory 'SHA256SUMS.txt'
 if(-not(Test-Path -LiteralPath $manifest -PathType Leaf)){throw 'Bundle SHA256SUMS.txt is missing.'}
 $seen=@{}
 foreach($line in Get-Content -LiteralPath $manifest){
  if($line -notmatch '^([a-fA-F0-9]{64})  (.+)$'){throw 'Invalid bundle checksum entry.'}
  $hash=$Matches[1].ToLowerInvariant();$name=$Matches[2]
  if($seen.ContainsKey($name) -or -not $Inventory.ContainsKey($name) -or $Inventory[$name] -ne $hash){
   throw "Bundle checksum mismatch: $name"
  }
  $seen[$name]=$true
 }
 if($seen.Count -ne $Inventory.Count){throw 'Bundle checksum manifest does not cover every file.'}
}
function Test-SameInventory($Left,$Right){
 if($Left.Count -ne $Right.Count){return $false}
 foreach($name in $Left.Keys){if(-not $Right.ContainsKey($name) -or $Left[$name] -ne $Right[$name]){return $false}}
 return $true
}

foreach($path in @($current,$archives,$stage)){Assert-ChildPath $path}
# Check all existing ancestors before creating or moving anything through them.
$ancestor=$release
while($ancestor){
 if(Test-Path -LiteralPath $ancestor){
  if((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Release path contains a reparse point: $ancestor"}
 }
 $ancestor=Split-Path $ancestor -Parent
}
if(Test-Path -LiteralPath $archives){
 if((Get-Item -LiteralPath $archives -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Archive directory must not be a reparse point.'}
}
$inventory=Get-Inventory $source
foreach($required in @('PyroScreen.exe','MainWindow.xaml','libpyrowave-shared-1.dll','PyroScreen-Sender.exe','PyroScreen-Receiver.exe','PyroScreen-Display.exe')){
 if(-not $inventory.ContainsKey($required)){throw "Incomplete release bundle: $required"}
}
Assert-Manifest $source $inventory
New-Item -ItemType Directory -Path $release -Force|Out-Null
$lock=$null;$oldArchive=$null
try {
 $lock=[IO.File]::Open((Join-Path $release '.publish.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
 if(Test-Path -LiteralPath $current){
  if(-not(Test-Path -LiteralPath (Join-Path $current 'PyroScreen.exe') -PathType Leaf)){throw 'Existing PyroScreen directory is not a release bundle.'}
  $oldInventory=Get-Inventory $current
  if(Test-SameInventory $inventory $oldInventory){
   [pscustomobject]@{Status='Unchanged';Executable=(Join-Path $current 'PyroScreen.exe');Archive=$null;Files=$inventory.Count}
   return
  }
  # Fail before the swap if a running engine or application locks its executable.
  foreach($exe in Get-ChildItem -LiteralPath $current -Filter '*.exe' -File){
   try{$probe=[IO.File]::Open($exe.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None);$probe.Dispose()}
   catch{
    if($_.Exception.GetBaseException() -is [UnauthorizedAccessException]){throw "Cannot access release executable: $($exe.FullName)"}
    throw "Exit PyroScreen from its tray menu before updating. Locked executable: $($exe.Name)"
   }
  }
 }
 New-Item -ItemType Directory -Path $stage -Force|Out-Null
 foreach($item in Get-ChildItem -LiteralPath $source -Force){Copy-Item -LiteralPath $item.FullName -Destination $stage -Recurse -Force}
 $stagedInventory=Get-Inventory $stage
 Assert-Manifest $stage $stagedInventory
 if(-not(Test-SameInventory $inventory $stagedInventory)){throw 'Staged release differs from the source bundle.'}
 if(Test-Path -LiteralPath $current){
  New-Item -ItemType Directory -Path $archives -Force|Out-Null
  $oldArchive=Join-Path $archives ('PyroScreen-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')+'Z-'+[Guid]::NewGuid().ToString('N').Substring(0,8))
  Assert-ChildPath $oldArchive
  if(Test-Path -LiteralPath $oldArchive){throw 'Archive path already exists.'}
  Move-Item -LiteralPath $current -Destination $oldArchive
 }
 try {
  Move-Item -LiteralPath $stage -Destination $current
  if(-not(Test-SameInventory $inventory (Get-Inventory $current))){throw 'Installed release checksum verification failed.'}
 } catch {
  if(Test-Path -LiteralPath $current){
   $failed=Join-Path $release ('.failed-release-'+[Guid]::NewGuid().ToString('N'))
   Assert-ChildPath $failed
   Move-Item -LiteralPath $current -Destination $failed
  }
  if($oldArchive -and (Test-Path -LiteralPath $oldArchive)){Move-Item -LiteralPath $oldArchive -Destination $current}
  throw
 }
 [pscustomobject]@{Status='Updated';Executable=(Join-Path $current 'PyroScreen.exe');Archive=$oldArchive;Files=$inventory.Count}
} finally {
 if($lock){$lock.Dispose()}
 if(Test-Path -LiteralPath $stage){
  Assert-ChildPath $stage
  Get-BundleFiles $stage|Out-Null
  Remove-Item -LiteralPath $stage -Recurse -Force
 }
}
