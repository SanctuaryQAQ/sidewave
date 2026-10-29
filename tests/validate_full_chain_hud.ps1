$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$results=Join-Path $root 'results';$app=Join-Path $root 'dist\PyroScreen\PyroScreen.exe'
trap{[IO.File]::WriteAllText((Join-Path $results 'full-chain-validation.error'),($_|Out-String));exit 1}
function Api($commandLine,$name){
 $file=Join-Path $results ($name+'.json');Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
 $p=Start-Process $app -ArgumentList ($commandLine+' --result "'+$file+'"') -WindowStyle Hidden -PassThru
 if(-not $p.WaitForExit(20000)){throw 'API timeout'}
 [IO.File]::ReadAllText($file)|ConvertFrom-Json
}
function SharedJson($path){
 for($i=0;$i -lt 10;$i++){try{
  $stream=[IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete));$reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
  try{$value=$reader.ReadToEnd()}finally{$reader.Dispose()};return ($value|ConvertFrom-Json)
 }catch{if($i -eq 9){throw};Start-Sleep -Milliseconds 25}}
}
$initial=Api '--status' 'full-chain-validation-initial';$connection=$initial.connection;$records=@();$summaries=@()
Remove-Item -LiteralPath (Join-Path $results 'full-chain-validation.error'),(Join-Path $results 'full-chain-validation-samples.jsonl') -Force -ErrorAction SilentlyContinue
if(-not $initial.streaming){throw 'Expected connected video'}
$env:DISABLE_RTSS_LAYER='1';$env:RTSSHooksProfileOverride='EnableHooking,0'
try{
 foreach($fps in @(165,240,165)){
  $count=if($summaries.Count -eq 0){30}elseif($fps -eq 240){20}else{12}
  $request=Join-Path $results 'full-chain-validation-request.json';[IO.File]::WriteAllText($request,('{"operation":"apply","fps":'+$fps+'}'))
  $ack=Api ('--control "'+$request+'"') ('full-chain-validation-apply-'+$summaries.Count)
  if(-not $ack.passed -or $ack.state.connection -ne $connection){throw 'Control session changed'}
  Start-Sleep -Seconds 8
  $state=Api '--status' ('full-chain-validation-phase-'+$summaries.Count)
  $doc=Start-Process (Join-Path $root 'build\bin\DesktopGpuDocument.exe') -ArgumentList ('"'+$state.display+'" "'+(Join-Path $root 'src\MainWindow.xaml')+'" --seconds '+($count+9)) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $results ('full-chain-document-'+$summaries.Count+'.json')) -RedirectStandardError (Join-Path $results ('full-chain-document-'+$summaries.Count+'.error'))
  $phase=@()
  try{
   Start-Sleep -Seconds 5
   for($i=0;$i -lt $count;$i++){
    Start-Sleep -Seconds 1
    $tx=SharedJson (Join-Path $state.runtime 'sender-live.json');$rx=SharedJson (Join-Path $state.runtime 'remote-stats.json')
    if($rx.pid -ne $state.receiverPid -or $tx.capture_limit_fps -ne $fps -or $rx.capture_timestamp_kind -ne 'desktop_update' -or $rx.stage_trace_enabled){throw 'Wrong telemetry'}
    $row=[pscustomobject]@{Phase=$summaries.Count;Target=$fps;Sample=$i;Sender=$tx;Receiver=$rx};$records+=$row;$phase+=$row
    [IO.File]::AppendAllText((Join-Path $results 'full-chain-validation-samples.jsonl'),(($row|ConvertTo-Json -Depth 8 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $results 'full-chain-validation-progress.txt'),("Target $fps; sample $i / $count; HUD "+$rx.hud_latency_available+'; endpoint '+$rx.hud_latency_endpoint))
   }
  }finally{if(-not $doc.HasExited){if(-not $doc.WaitForExit(7000)){Stop-Process -Id $doc.Id -Force}}}
  $valid=@($phase|Where-Object {$_.Receiver.hud_latency_available -and $_.Receiver.display_feedback_kind -eq 'etw' -and $_.Receiver.hud_latency_endpoint -eq 'system_display'})
  if($valid.Count -lt [Math]::Ceiling($count*0.8)){throw ('Too few valid full-chain HUD samples: '+$valid.Count+'/'+$count)}
  $first=$phase[0].Receiver;$last=$phase[-1].Receiver;$dt=$last.elapsed_s-$first.elapsed_s
  $summaries+=[pscustomobject]@{Target=$fps;Samples=$count;Valid=$valid.Count;ReceiverPid=$state.receiverPid;ReceiveFps=($last.complete_frames-$first.complete_frames)/$dt;PresentFps=($last.presented_frames-$first.presented_frames)/$dt;HudMeanMs=($valid.Receiver.hud_latency_ms|Measure-Object -Average).Average;DisplayP50Ms=($valid.Receiver.source_to_display_p50_ms|Measure-Object -Average).Average;DisplayP95Ms=($valid.Receiver.source_to_display_p95_ms|Measure-Object -Average).Average;ClockBoundMaxMs=($valid.Receiver.clock_error_bound_ms|Measure-Object -Maximum).Maximum;ObserverLagMaxMs=($valid.Receiver.display_feedback_delay_ms|Measure-Object -Maximum).Maximum;UnmatchedDelta=$last.etw_unmatched_presents-$first.etw_unmatched_presents;DroppedDelta=$last.etw_dropped_presents-$first.etw_dropped_presents;FreshnessDropsDelta=$last.freshness_dropped_frames-$first.freshness_dropped_frames}
 }
}finally{
 $records|ConvertTo-Json -Depth 10|Set-Content (Join-Path $results 'full-chain-validation-samples.json') -Encoding UTF8
 $summaries|ConvertTo-Json -Depth 8|Set-Content (Join-Path $results 'full-chain-validation-phase-summaries.json') -Encoding UTF8
 $request=Join-Path $results 'full-chain-validation-request.json';[IO.File]::WriteAllText($request,'{"operation":"apply","fps":165}')
 $restored=Api ('--control "'+$request+'"') 'full-chain-validation-restored'
}
$records|ConvertTo-Json -Depth 10|Set-Content (Join-Path $results 'full-chain-validation-samples.json') -Encoding UTF8
[pscustomobject]@{Passed=$true;ControlConnectionRetained=$true;FullChainEndpoint='source_desktop_update_to_system_display';PhysicalPanelResponseMeasured=$false;Summaries=$summaries;Final=$restored}|ConvertTo-Json -Depth 12|Set-Content (Join-Path $results 'full-chain-validation.json') -Encoding UTF8
exit 0
