$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$refs=@('System.dll','System.Core.dll','System.Security.dll','System.Web.Extensions.dll')
$arguments=@('/nologo','/target:winexe','/platform:x64','/optimize+')
foreach($r in $refs){$arguments+="/reference:$(Join-Path $framework $r)"}
& (Join-Path $framework 'csc.exe') @arguments "/out:$(Join-Path $root 'build\bin\ProtocolTests.exe')" (Join-Path $root 'src\Core.cs') (Join-Path $root 'src\Sessions.cs') (Join-Path $PSScriptRoot 'ProtocolTests.cs')
if($LASTEXITCODE){throw 'Protocol test build failed'}
& (Join-Path $framework 'csc.exe') @arguments "/out:$(Join-Path $root 'build\bin\TwoMachineTests.exe')" (Join-Path $root 'src\Core.cs') (Join-Path $root 'src\Sessions.cs') (Join-Path $PSScriptRoot 'TwoMachineTests.cs')
if($LASTEXITCODE){throw 'Two-machine test build failed'}
& (Join-Path $framework 'csc.exe') @arguments "/out:$(Join-Path $root 'build\bin\DiscoveryTests.exe')" (Join-Path $root 'src\Core.cs') (Join-Path $PSScriptRoot 'DiscoveryTests.cs')
if($LASTEXITCODE){throw 'Discovery test build failed'}
& (Join-Path $framework 'csc.exe') @arguments "/out:$(Join-Path $root 'build\bin\PrimaryDisplayProbe.exe')" (Join-Path $root 'src\Core.cs') (Join-Path $PSScriptRoot 'PrimaryDisplayProbe.cs')
if($LASTEXITCODE){throw 'Primary display probe build failed'}
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/reference:$framework\WPF\UIAutomationClient.dll" "/reference:$framework\WPF\UIAutomationTypes.dll" "/reference:$framework\WPF\WindowsBase.dll" "/out:$(Join-Path $root 'build\bin\PairCodeProbe.exe')" (Join-Path $PSScriptRoot 'PairCodeProbe.cs')
if($LASTEXITCODE){throw 'UI probe build failed'}
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/reference:$framework\System.Drawing.dll" "/reference:$framework\WPF\UIAutomationClient.dll" "/reference:$framework\WPF\UIAutomationTypes.dll" "/reference:$framework\WPF\WindowsBase.dll" "/out:$(Join-Path $root 'build\bin\GuiProbe.exe')" (Join-Path $PSScriptRoot 'GuiProbe.cs')
if($LASTEXITCODE){throw 'GUI probe build failed'}
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/reference:$framework\WPF\UIAutomationClient.dll" "/reference:$framework\WPF\UIAutomationTypes.dll" "/reference:$framework\WPF\WindowsBase.dll" "/out:$(Join-Path $root 'build\bin\GuiActions.exe')" (Join-Path $PSScriptRoot 'GuiActions.cs')
if($LASTEXITCODE){throw 'GUI actions build failed'}
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/reference:$framework\System.Windows.Forms.dll" "/reference:$framework\System.Drawing.dll" "/out:$(Join-Path $root 'build\bin\HostCursorProbe.exe')" (Join-Path $PSScriptRoot 'HostCursorProbe.cs')
if($LASTEXITCODE){throw 'Host cursor probe build failed'}
& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/out:$(Join-Path $root 'build\bin\ClientCursorProbe.exe')" (Join-Path $PSScriptRoot 'ClientCursorProbe.cs')
if($LASTEXITCODE){throw 'Client cursor probe build failed'}

& (Join-Path $framework 'csc.exe') /nologo /target:winexe /platform:x64 "/out:$(Join-Path $root 'build\bin\StatusProbe.exe')" (Join-Path $PSScriptRoot 'StatusProbe.cs')
if($LASTEXITCODE){throw 'Status probe build failed'}
