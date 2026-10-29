param([int]$Samples=90)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$results=Join-Path $root 'results';$app=Join-Path $root 'dist\PyroScreen\PyroScreen.exe'
trap{[IO.File]::WriteAllText((Join-Path $results '165-frame-drops.error'),($_|Out-String));exit 1}
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
$initial=Api '--status' '165-drops-initial';$connection=$initial.connection;$records=@();$phases=@()
if(-not $initial.streaming){throw 'Expected connected video'}
Remove-Item -LiteralPath (Join-Path $results '165-frame-drops.error'),(Join-Path $results '165-frame-drops.jsonl') -Force -ErrorAction SilentlyContinue
$env:DISABLE_RTSS_LAYER='1';$env:RTSSHooksProfileOverride='EnableHooking,0'
$budgets=if($initial.budgetMbps -eq 400){@(400)}else{@($initial.budgetMbps,400,$initial.budgetMbps)}
try{
 foreach($budget in $budgets){
  $phase=$phases.Count;$request=Join-Path $results '165-drops-request.json';[IO.File]::WriteAllText($request,('{"operation":"apply","fps":165,"budgetMbps":'+$budget+'}'))
  $ack=Api ('--control "'+$request+'"') ('165-drops-apply-'+$phase)
  if(-not $ack.passed -or $ack.state.connection -ne $connection){throw 'Control connection changed'}
  Start-Sleep -Seconds 12
  $state=Api '--status' ('165-drops-phase-'+$phase)
  $doc=Start-Process (Join-Path $root 'build\bin\DesktopGpuDocument.exe') -ArgumentList ('"'+$state.display+'" "'+(Join-Path $root 'src\MainWindow.xaml')+'" --seconds '+($Samples+9)) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $results ('165-drops-source-'+$phase+'.json')) -RedirectStandardError (Join-Path $results ('165-drops-source-'+$phase+'.error'))
  try{
   Start-Sleep -Seconds 5
   for($i=0;$i -le $Samples;$i++){
    if($i){Start-Sleep -Seconds 1}
    $tx=SharedJson (Join-Path $state.runtime 'sender-live.json');$rx=SharedJson (Join-Path $state.runtime 'remote-stats.json')
    if($rx.pid -ne $state.receiverPid -or $tx.capture_limit_fps -ne 165 -or $rx.width -ne 2560 -or $rx.height -ne 1440 -or $rx.stage_trace_enabled -or -not ($rx.PSObject.Properties.Name -contains 'network_incomplete_frames') -or -not ($tx.PSObject.Properties.Name -contains 'encoded_frame_bytes_p95')){throw 'Wrong or old telemetry'}
    $row=[pscustomobject]@{Phase=$phase;BudgetMbps=$budget;Sample=$i;Sender=$tx;Receiver=$rx};$records+=$row
    [IO.File]::AppendAllText((Join-Path $results '165-frame-drops.jsonl'),(($row|ConvertTo-Json -Depth 8 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText((Join-Path $results '165-frame-drops-progress.txt'),("Phase $phase; $budget Mbps ceiling; sample $i / $Samples; RX incomplete "+$rx.network_incomplete_frames+'; queue overwrite '+$rx.freshness_dropped_frames+'; decoder not ready '+$rx.decoder_not_ready_frames))
   }
  }finally{if(-not $doc.HasExited){if(-not $doc.WaitForExit(7000)){Stop-Process -Id $doc.Id -Force}}}
  $phases+=[pscustomobject]@{Phase=$phase;BudgetMbps=$budget;ReceiverPid=$state.receiverPid;SenderPid=$tx.pid}
 }
}finally{
 $records|ConvertTo-Json -Depth 10|Set-Content (Join-Path $results '165-frame-drops-samples.json') -Encoding UTF8
 $request=Join-Path $results '165-drops-request.json';[IO.File]::WriteAllText($request,('{"operation":"apply","fps":165,"budgetMbps":'+$initial.budgetMbps+'}'))
 $restored=Api ('--control "'+$request+'"') '165-drops-restored'
}
[pscustomobject]@{Passed=$true;TargetFps=165;ControlConnectionRetained=$true;ConstantWorkload='real project document scrolling';QualityVisuallyValidated=$false;Initial=$initial;Phases=$phases;Final=$restored}|ConvertTo-Json -Depth 10|Set-Content (Join-Path $results '165-frame-drops-validation.json') -Encoding UTF8
exit 0
