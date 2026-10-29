$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'));$results=Join-Path $root 'results'
New-Item -ItemType Directory -Path $results -Force|Out-Null
$cmake=& (Join-Path $root 'tools\find_cmake.ps1')
$ErrorActionPreference='Continue'
& $cmake -S $root -B (Join-Path $root 'build\clean-cmake') *> (Join-Path $results 'frame-schedule-configure.log')
if($LASTEXITCODE -ne 0){exit 1}
& $cmake --build (Join-Path $root 'build\clean-cmake') --config Release --target FrameDatagramScheduleTests DecoderSequenceRegressionTests DatagramSlotPoolTests DatagramSenderLoopbackTests *> (Join-Path $results 'frame-schedule-build.log')
if($LASTEXITCODE -ne 0){exit 1}
& (Join-Path $root 'build\clean-cmake\Release\FrameDatagramScheduleTests.exe') *> (Join-Path $results 'frame-schedule-test.log')
if($LASTEXITCODE -ne 0){exit 1}
& (Join-Path $root 'build\clean-cmake\Release\DecoderSequenceRegressionTests.exe') *> (Join-Path $results 'decoder-sequence-test.log')
if($LASTEXITCODE -ne 0){exit 1}
& (Join-Path $root 'build\clean-cmake\Release\DatagramSlotPoolTests.exe') *> (Join-Path $results 'datagram-slot-pool-test.log')
if($LASTEXITCODE -ne 0){exit 1}
& (Join-Path $root 'build\clean-cmake\Release\DatagramSenderLoopbackTests.exe') *> (Join-Path $results 'datagram-sender-loopback-test.log')
exit $LASTEXITCODE
