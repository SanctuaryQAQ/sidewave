$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$folder=Join-Path $root 'results\165-reconnect-lease';$app=Join-Path $root 'dist\PyroScreen\PyroScreen.exe'
New-Item -ItemType Directory -Path $folder -Force|Out-Null
if(Test-Path (Join-Path $folder 'test.error')){Move-Item -LiteralPath (Join-Path $folder 'test.error') -Destination (Join-Path $folder 'test.previous.error') -Force}
trap{[IO.File]::WriteAllText((Join-Path $folder 'test.error'),($_|Out-String));exit 1}
$framework=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319';$arguments=@('/nologo','/target:winexe','/platform:x64','/optimize+','/main:PyroScreen.ReconnectLeaseTests')
foreach($r in @('System.dll','System.Core.dll','System.Security.dll','System.Web.Extensions.dll','System.Drawing.dll','System.Windows.Forms.dll','Microsoft.CSharp.dll')){$arguments+='/reference:'+(Join-Path $framework $r)}
foreach($r in @('PresentationCore','PresentationFramework','WindowsBase','System.Xaml')){$p=Join-Path $framework "WPF\$r.dll";if(-not(Test-Path $p)){$p=Join-Path $framework "$r.dll"};$arguments+='/reference:'+$p}
$test=Join-Path $root 'build\bin\ReconnectLeaseTests.exe';$sources=@(Get-ChildItem (Join-Path $root 'src') -Filter '*.cs'|Select-Object -ExpandProperty FullName)
& (Join-Path $framework 'csc.exe') @arguments ('/out:'+$test) @sources (Join-Path $PSScriptRoot 'ReconnectLeaseTests.cs') *> (Join-Path $folder 'build.log');if($LASTEXITCODE -ne 0){throw 'Lease test build failed'}
function Api($operation,$name){$file=Join-Path $folder ($name+'.json');$p=Start-Process $app -ArgumentList ($operation+' --result "'+$file+'"') -WindowStyle Hidden -PassThru;if(-not $p.WaitForExit(95000)){throw 'API timeout'};[IO.File]::ReadAllText($file)|ConvertFrom-Json}
$stopped=Api '--stop-video' 'stop-video';if(-not $stopped.passed){throw 'Stop rejected'}
try{
 $report=Join-Path $folder 'test.json';Remove-Item -LiteralPath $report -Force -ErrorAction SilentlyContinue
 # The desktop app is outside the tool's filesystem view. Run this read-only
 # credential reuse test in the same real interactive Windows user context.
 $task='PyroScreen-165-LeaseVerification-20261006'
 if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue){throw 'Unexpected previous verification task'}
 try{
  Register-ScheduledTask -TaskName $task -Action (New-ScheduledTaskAction -Execute $test -Argument ('"'+$report+'"') -WorkingDirectory (Split-Path $test)) -Principal (New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest) -Settings (New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds 30)) -Force|Out-Null
  Start-ScheduledTask $task;$deadline=(Get-Date).AddSeconds(30)
  do{Start-Sleep -Milliseconds 200}until((Test-Path $report) -or (Get-Date) -gt $deadline)
  if(-not(Test-Path $report)){throw 'Lease verification timed out'}
 }finally{Stop-ScheduledTask $task -ErrorAction SilentlyContinue;Unregister-ScheduledTask $task -Confirm:$false -ErrorAction SilentlyContinue}
 $result=[IO.File]::ReadAllText($report)|ConvertFrom-Json;if(-not $result.passed){throw $result.error}
}finally{$restored=Api '--apply-current' 'video-restored';if(-not $restored.passed -or -not $restored.state.streaming){throw 'Video restoration failed'}}
[pscustomobject]@{Passed=$true;Takeover=$result;Restored=$restored}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $folder 'validation.json') -Encoding UTF8
exit 0
