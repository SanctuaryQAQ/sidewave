param([Parameter(Mandatory=$true)][string]$AppPath)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
# Remove what the version released under the previous name installed.
Get-NetFirewallRule -PolicyStore PersistentStore -Group 'PyroScreen' -ErrorAction SilentlyContinue | Remove-NetFirewallRule -ErrorAction SilentlyContinue
Get-ScheduledTask -TaskName 'PyroScreen-DisplayLatency-*' -ErrorAction SilentlyContinue | Unregister-ScheduledTask -Confirm:$false -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process -Filter "Name='PyroScreen.exe'" | Where-Object {$_.ExecutablePath -like (Join-Path $env:ProgramFiles 'PyroScreen\*')} | ForEach-Object {Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue}
Remove-Item -LiteralPath (Join-Path $env:ProgramFiles 'PyroScreen') -Recurse -Force -ErrorAction SilentlyContinue
$receiver=Join-Path (Split-Path -LiteralPath $AppPath) 'SideWave-Receiver.exe'
if(-not (Test-Path -LiteralPath $receiver)){throw 'SideWave-Receiver.exe is missing.'}
$specs=@(
 @{Name='SideWave control';Program=$AppPath;Protocol='TCP';Port='55166'},
 # Sender discovery uses an ephemeral reply port; scope by program and subnet.
 @{Name='SideWave discovery';Program=$AppPath;Protocol='UDP';Port='Any'},
 @{Name='SideWave video';Program=$receiver;Protocol='UDP';Port='55165'}
)
foreach($spec in $specs){
 $rules=@(Get-NetFirewallRule -PolicyStore PersistentStore -DisplayName $spec.Name -ErrorAction SilentlyContinue)
 if($rules.Count){$rules | Remove-NetFirewallRule -ErrorAction Stop}
 New-NetFirewallRule -DisplayName $spec.Name -Group 'SideWave' -Direction Inbound -Action Allow -Enabled True -Program $spec.Program -Protocol $spec.Protocol -LocalPort $spec.Port -RemoteAddress LocalSubnet -Profile Private | Out-Null
}
foreach($spec in $specs){
 $rule=Get-NetFirewallRule -PolicyStore ActiveStore -DisplayName $spec.Name -ErrorAction Stop
 $valid=@($rule | Where-Object {
  $app=$_ | Get-NetFirewallApplicationFilter
  $port=$_ | Get-NetFirewallPortFilter
  $addr=$_ | Get-NetFirewallAddressFilter
  $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' -and $_.Profile -eq 'Private' -and $app.Program -eq $spec.Program -and $port.Protocol -eq $spec.Protocol -and $port.LocalPort -eq $spec.Port -and $addr.RemoteAddress -eq 'LocalSubnet'
 })
 if(-not $valid.Count){throw ('Firewall rule verification failed: '+$spec.Name)}
}
Write-Output 'Verified 3 private-network firewall rules for this copy of SideWave.'
