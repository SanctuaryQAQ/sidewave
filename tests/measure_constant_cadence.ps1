param([int]$Samples=15,[int[]]$Targets=@(60,120,165,240))
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$results=Join-Path $root 'results';$app=Join-Path $root 'dist\PyroScreen\PyroScreen.exe'
New-Item -ItemType Directory -Path $results -Force|Out-Null
trap{[IO.File]::WriteAllText((Join-Path $results 'constant-cadence-measure.error'),($_|Out-String));exit 1}
function Api($commandLine,$name){
 $file=Join-Path $results ($name+'.json');Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
 $process=Start-Process $app -ArgumentList ($commandLine+' --result "'+$file+'"') -WindowStyle Hidden -PassThru
 if(-not $process.WaitForExit(95000)){throw 'Application API timeout'}
 [IO.File]::ReadAllText($file)|ConvertFrom-Json
}
function SharedJson($path){
 for($attempt=0;$attempt -lt 10;++$attempt){try{
  $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
  try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()};return ($text|ConvertFrom-Json)
 }catch{if($attempt -eq 9){throw};Start-Sleep -Milliseconds 25}}
}
& (& (Join-Path $root 'tools\find_cmake.ps1')) --build (Join-Path $root 'build\clean-cmake') --config Release --target DesktopGpuDocument *> (Join-Path $results 'constant-cadence-document-build.log')
if($LASTEXITCODE -ne 0){throw 'Document workload build failed'}
Copy-Item (Join-Path $root 'build\clean-cmake\Release\DesktopGpuDocument.exe') (Join-Path $root 'build\bin\DesktopGpuDocument.exe') -Force
$initial=Api '--status' 'constant-cadence-measure-start';$controlConnection=$initial.connection;$records=@()
$env:DISABLE_RTSS_LAYER='1';$env:RTSSHooksProfileOverride='EnableHooking,0'
try{
 foreach($fps in $Targets){
  $request=Join-Path $results 'constant-cadence-measure-request.json';[IO.File]::WriteAllText($request,('{"operation":"apply","fps":'+$fps+'}'))
  $ack=Api ('--control "'+$request+'"') ('constant-cadence-apply-'+$fps)
  if(-not $ack.passed -or $ack.state.effectiveFps -ne $fps -or $ack.state.connection -ne $controlConnection){throw 'Frame target apply failed or changed control connection'}
  Start-Sleep -Seconds 12
  $state=Api '--status' ('constant-cadence-target-'+$fps)
  foreach($phase in @('scroll','idle')){
   [IO.File]::WriteAllText((Join-Path $results 'constant-cadence-progress.txt'),("Target $fps FPS; phase $phase"))
   $doc=$null
   if($phase -eq 'scroll'){$doc=Start-Process (Join-Path $root 'build\bin\DesktopGpuDocument.exe') -ArgumentList ('"'+$state.display+'" "'+(Join-Path $root 'src\MainWindow.xaml')+'" --seconds '+($Samples+6)) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $results ("constant-cadence-$fps-source.json")) -RedirectStandardError (Join-Path $results ("constant-cadence-$fps-source.error"))}
   try{
    Start-Sleep -Seconds 4
    for($i=0;$i -lt $Samples;++$i){
     Start-Sleep -Seconds 1
     $sender=SharedJson (Join-Path $state.runtime 'sender-live.json');$receiver=SharedJson (Join-Path $state.runtime 'remote-stats.json')
     if($receiver.pid -ne $state.receiverPid -or $sender.frame_rate_policy -ne 'constant' -or $sender.capture_limit_fps -ne $fps){throw 'Wrong or stale cadence telemetry'}
     $record=[pscustomobject]@{Target=$fps;Phase=$phase;Sample=$i;Sender=$sender;Receiver=$receiver};$records+=$record
     [IO.File]::AppendAllText((Join-Path $results 'constant-cadence-samples.jsonl'),(($record|ConvertTo-Json -Depth 8 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
    }
   }finally{if($doc -and -not $doc.HasExited){if(-not $doc.WaitForExit(8000)){Stop-Process -Id $doc.Id -Force}}}
  }
 }
}finally{
 $request=Join-Path $results 'constant-cadence-measure-request.json';[IO.File]::WriteAllText($request,'{"operation":"apply","fps":165}')
 $restored=Api ('--control "'+$request+'"') 'constant-cadence-restored'
}
Start-Sleep -Seconds 5
$final=Api '--status' 'constant-cadence-final-measured-status'
if(-not $final.streaming -or $final.stats.hud_latency_endpoint -ne 'system_display' -or $final.stats.capture_timestamp_kind -ne 'desktop_update' -or $final.effectiveFps -ne 165){throw 'Final full-chain endpoint or target validation failed'}
# A fully idle desktop need not provide fresh source updates for latency. Verify
# availability during the scrolling workload rather than requiring an old value.
foreach($target in $Targets){
 $valid=@($records|Where-Object {$_.Target -eq $target -and $_.Phase -eq 'scroll' -and $_.Receiver.hud_latency_available -and $_.Receiver.display_feedback_kind -eq 'etw'})
 if($valid.Count -lt [Math]::Ceiling($Samples*0.8)){throw ('Insufficient full-chain display samples at '+$target)}
}
$records|ConvertTo-Json -Depth 10|Set-Content (Join-Path $results 'constant-cadence-samples.json') -Encoding UTF8
[pscustomobject]@{Passed=$true;ControlConnectionRetained=$true;Final=$final}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $results 'constant-cadence-validation.json') -Encoding UTF8
exit 0
