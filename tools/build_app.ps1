$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output=Join-Path $root 'build\bin'
New-Item -ItemType Directory -Path $output -Force|Out-Null
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$refs=@('System.dll','System.Core.dll','System.Security.dll','System.Web.Extensions.dll','System.Drawing.dll','System.Windows.Forms.dll','Microsoft.CSharp.dll')
$args=@('/nologo','/target:winexe','/platform:x64','/optimize+')
foreach($r in $refs){$args+="/reference:$(Join-Path $framework $r)"}
foreach($r in @('PresentationCore','PresentationFramework','WindowsBase','System.Xaml')){
 $p=Join-Path $framework "WPF\$r.dll"
 if(-not(Test-Path $p)){$p=Join-Path $framework "$r.dll"}
 $args+="/reference:$p"
}
$sources=Get-ChildItem (Join-Path $root 'src') -Filter '*.cs'|Select-Object -ExpandProperty FullName
& (Join-Path $framework 'csc.exe') @args "/win32icon:$(Join-Path $root 'assets\app.ico')" "/out:$(Join-Path $output 'PyroScreen.exe')" @sources
if($LASTEXITCODE){throw 'Application compilation failed'}
Add-Type -AssemblyName PresentationFramework
$markup=[IO.File]::OpenRead((Join-Path $root 'src\MainWindow.xaml'))
try{[Windows.Markup.XamlReader]::Load($markup)|Out-Null}finally{$markup.Dispose()}
Copy-Item (Join-Path $root 'src\MainWindow.xaml') $output -Force
Copy-Item (Join-Path $root 'assets\*.png'),(Join-Path $root 'assets\*.ico') $output -Force
Copy-Item -LiteralPath (Join-Path $root 'assets\fonts') -Destination $output -Recurse -Force
foreach($name in @('Install-Driver.ps1','Install-Network.ps1','Inspect-System.ps1')){Copy-Item (Join-Path $root ('tools\'+$name)) $output -Force}
& (Join-Path $PSScriptRoot 'get_presentmon.ps1')
