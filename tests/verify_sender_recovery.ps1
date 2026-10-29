$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$folder=Join-Path $root 'build\sender-recovery-tests'
New-Item -ItemType Directory -Path $folder -Force|Out-Null
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$arguments=@('/nologo','/target:exe','/platform:x64','/optimize+','/main:PyroScreen.SenderRecoveryTests')
foreach($r in @('System.dll','System.Core.dll','System.Security.dll','System.Web.Extensions.dll','System.Drawing.dll','System.Windows.Forms.dll','Microsoft.CSharp.dll')){$arguments+='/reference:'+(Join-Path $framework $r)}
foreach($r in @('PresentationCore','PresentationFramework','WindowsBase','System.Xaml')){$p=Join-Path $framework "WPF\$r.dll";if(-not(Test-Path $p)){$p=Join-Path $framework "$r.dll"};$arguments+='/reference:'+$p}
$test=Join-Path $folder 'SenderRecoveryTests.exe'
$sources=@(Get-ChildItem (Join-Path $root 'src') -Filter '*.cs'|Select-Object -ExpandProperty FullName)
& (Join-Path $framework 'csc.exe') @arguments ('/out:'+$test) @sources (Join-Path $PSScriptRoot 'SenderRecoveryTests.cs') *> (Join-Path $folder 'build.log')
if($LASTEXITCODE -ne 0){throw 'Sender recovery test build failed'}
$appArguments=@($arguments|Where-Object {$_ -notlike '/main:*' -and $_ -notlike '/target:*'})
& (Join-Path $framework 'csc.exe') @appArguments /target:winexe /main:PyroScreen.App ('/win32icon:'+(Join-Path $root 'assets\app.ico')) ('/out:'+(Join-Path $folder 'PyroScreen.exe')) @sources *> (Join-Path $folder 'app-build.log')
if($LASTEXITCODE -ne 0){throw 'Application build failed'}
$report=Join-Path $folder 'test.json'
$p=Start-Process -FilePath $test -ArgumentList ('"'+$report+'"') -WindowStyle Hidden -PassThru
if(-not $p.WaitForExit(15000)){throw 'Sender recovery tests timed out'}
$result=[IO.File]::ReadAllText($report)|ConvertFrom-Json
if(-not $result.passed){throw $result.error}
$result|ConvertTo-Json
