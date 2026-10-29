param([Parameter(Mandatory=$true)][string]$AppPath)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
$receiver=Join-Path (Split-Path -LiteralPath $AppPath) 'PyroScreen-Receiver.exe'
if(-not (Test-Path -LiteralPath $receiver)){throw 'PyroScreen-Receiver.exe is missing.'}
$specs=@(
 @{Name='PyroScreen control';Program=$AppPath;Protocol='TCP';Port='55166'},
 # Sender discovery uses an ephemeral reply port; scope by program and subnet.
 @{Name='PyroScreen discovery';Program=$AppPath;Protocol='UDP';Port='Any'},
 @{Name='PyroScreen video';Program=$receiver;Protocol='UDP';Port='55165'}
)
foreach($spec in $specs){
 $rules=@(Get-NetFirewallRule -PolicyStore PersistentStore -DisplayName $spec.Name -ErrorAction SilentlyContinue)
 if($rules.Count){$rules | Remove-NetFirewallRule -ErrorAction Stop}
 New-NetFirewallRule -DisplayName $spec.Name -Group 'PyroScreen' -Direction Inbound -Action Allow -Enabled True -Program $spec.Program -Protocol $spec.Protocol -LocalPort $spec.Port -RemoteAddress LocalSubnet -Profile Private | Out-Null
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
Write-Output 'Verified 3 private-network firewall rules for this copy of PyroScreen.'
