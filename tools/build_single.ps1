param([Parameter(Mandatory=$true)][string]$BundleDirectory,[Parameter(Mandatory=$true)][string]$Output)
# Builds the one-file SideWave.exe: the same application with the whole release bundle embedded.
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$args=@('/nologo','/target:winexe','/platform:x64','/optimize+')
foreach($r in @('System.dll','System.Core.dll','System.Security.dll','System.Web.Extensions.dll','System.Drawing.dll','System.Windows.Forms.dll','Microsoft.CSharp.dll','System.IO.Compression.dll','System.IO.Compression.FileSystem.dll')){$args+="/reference:$(Join-Path $framework $r)"}
foreach($r in @('PresentationCore','PresentationFramework','WindowsBase','System.Xaml')){
 $p=Join-Path $framework "WPF\$r.dll"
 if(-not(Test-Path $p)){$p=Join-Path $framework "$r.dll"}
 $args+="/reference:$p"
}
$sources=Get-ChildItem (Join-Path $root 'src') -Filter '*.cs'|Select-Object -ExpandProperty FullName
$bundle=[IO.Path]::Combine([IO.Path]::GetTempPath(),'SideWave-bundle-'+[Guid]::NewGuid().ToString('N')+'.zip')
Add-Type -AssemblyName System.IO.Compression.FileSystem
try{
 [IO.Compression.ZipFile]::CreateFromDirectory([IO.Path]::GetFullPath($BundleDirectory),$bundle,[IO.Compression.CompressionLevel]::Optimal,$false)
 & (Join-Path $framework 'csc.exe') @args "/win32icon:$(Join-Path $root 'assets\app.ico')" "/resource:$bundle,SideWave.bundle.zip" "/out:$([IO.Path]::GetFullPath($Output))" @sources
 if($LASTEXITCODE){throw 'Single-file compilation failed'}
}finally{Remove-Item -LiteralPath $bundle -Force -ErrorAction SilentlyContinue}
