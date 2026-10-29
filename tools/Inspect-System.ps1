param([Parameter(Mandatory=$true)][string]$AppPath)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
$issues=New-Object 'System.Collections.Generic.List[string]'
$profiles=@();$rules=@();$devices=@();$packages=@();$legacyPackages=@();$listeners=@();$sampler=$false
try{$profiles=@(Get-NetConnectionProfile | ForEach-Object {
 $idx=$_.InterfaceIndex
 $addresses=@(Get-NetIPAddress -InterfaceIndex $idx -AddressFamily IPv4 -ErrorAction SilentlyContinue | Select-Object -ExpandProperty IPAddress)
 [pscustomobject]@{name=$_.InterfaceAlias;category=[string]$_.NetworkCategory;addresses=($addresses -join ', ')}
})}catch{$issues.Add('Network profiles: '+$_.Exception.Message)}
$receiver=Join-Path (Split-Path -LiteralPath $AppPath) 'PyroScreen-Receiver.exe'
foreach($spec in @(@{Name='PyroScreen control';Program=$AppPath;Protocol='TCP';Port='55166'},@{Name='PyroScreen discovery';Program=$AppPath;Protocol='UDP';Port='Any'},@{Name='PyroScreen video';Program=$receiver;Protocol='UDP';Port='55165'})){
 try{
  $found=@(Get-NetFirewallRule -PolicyStore ActiveStore -DisplayName $spec.Name -ErrorAction SilentlyContinue)
  $valid=@($found | Where-Object {
   $app=$_ | Get-NetFirewallApplicationFilter; $port=$_ | Get-NetFirewallPortFilter; $addr=$_ | Get-NetFirewallAddressFilter
   $_.Enabled -eq 'True' -and $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' -and $_.Profile -eq 'Private' -and $app.Program -eq $spec.Program -and $port.Protocol -eq $spec.Protocol -and $port.LocalPort -eq $spec.Port -and $addr.RemoteAddress -eq 'LocalSubnet'
  })
  $rules+=[pscustomobject]@{name=$spec.Name;valid=($valid.Count -gt 0);protocol=$spec.Protocol;port=$spec.Port}
 }catch{$issues.Add($spec.Name+': '+$_.Exception.Message)}
}
try{$packages=@(Get-ChildItem -LiteralPath (Join-Path $env:windir 'System32\DriverStore\FileRepository') -Directory -Filter 'mttvdd.inf_*' | Select-Object -ExpandProperty Name)}catch{$issues.Add('Driver store: '+$_.Exception.Message)}
try{$legacyPackages=@(Get-ChildItem -LiteralPath (Join-Path $env:windir 'System32\DriverStore\FileRepository') -Directory -Filter 'iddsampledriver.inf_*' | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'LanScreenDisplay.dll')} | Select-Object -ExpandProperty Name)}catch{$issues.Add('LanScreen driver store: '+$_.Exception.Message)}
try{$devices=@(Get-PnpDevice -Class Display -PresentOnly | Where-Object {$_.InstanceId -match 'MttVDD|PyroScreen|LanScreen'} | ForEach-Object {[pscustomobject]@{name=$_.FriendlyName;status=[string]$_.Status;id=$_.InstanceId}})}catch{$issues.Add('Display devices: '+$_.Exception.Message)}
try{$listeners=@(Get-NetTCPConnection -State Listen -LocalPort 55166 -ErrorAction SilentlyContinue | ForEach-Object {[pscustomobject]@{address=$_.LocalAddress;pid=$_.OwningProcess}})}catch{$issues.Add('Control listener: '+$_.Exception.Message)}
try{$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value;$sampler=$null -ne (Get-ScheduledTask -TaskName ('PyroScreen-DisplayLatency-'+$sid) -ErrorAction SilentlyContinue)}catch{$issues.Add('Display sampler: '+$_.Exception.Message)}
[pscustomobject]@{profiles=$profiles;rules=$rules;driverPackages=$packages;legacyPackages=$legacyPackages;devices=$devices;listeners=$listeners;sampler=$sampler;issues=@($issues.ToArray())} | ConvertTo-Json -Depth 5 -Compress
