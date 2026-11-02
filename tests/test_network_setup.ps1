$ErrorActionPreference='Stop'
$installer=Join-Path $PSScriptRoot '..\tools\Install-Network.ps1'
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('SideWave-network-test-'+[Guid]::NewGuid().ToString('N'))
$app=Join-Path $fixture 'SideWave.exe'
$receiver=Join-Path $fixture 'SideWave-Receiver.exe'
$stateName='SideWaveNetworkRegressionState'
if(Get-Variable -Name $stateName -Scope Global -ErrorAction SilentlyContinue){throw 'Network regression state already exists.'}

function Assert([bool]$condition,[string]$message){if(-not $condition){throw ('Assertion failed: '+$message)}}
function Reset-Mock {
 $global:SideWaveNetworkRegressionState=@{Rules=@{};NewCount=0;RemoveCount=0;ActiveReads=0;FailWrite=$false;Mismatch=''}
}
# These functions shadow every firewall command used by the installer. They never
# import NetSecurity or delegate to its cmdlets, so this test cannot change rules.
function Get-NetFirewallRule {
 [CmdletBinding()]param([string]$PolicyStore,[string]$DisplayName)
 $state=$global:SideWaveNetworkRegressionState
 Assert ($PolicyStore -in @('PersistentStore','ActiveStore')) 'explicit firewall policy store'
 if($PolicyStore -eq 'ActiveStore'){$state.ActiveReads++}
 if(-not $state.Rules.ContainsKey($DisplayName)){return}
 $rule=$state.Rules[$DisplayName]
 if($PolicyStore -eq 'ActiveStore' -and $state.Mismatch){
  $copy=@{};foreach($property in $rule.PSObject.Properties){$copy[$property.Name]=$property.Value}
  $copy[$state.Mismatch]=switch($state.Mismatch){
   'Enabled' {'False'}
   'Direction' {'Outbound'}
   'Action' {'Block'}
   'Profile' {'Public'}
   'Program' {'C:\unexpected\wrong.exe'}
   'Protocol' {'ICMPv4'}
   'LocalPort' {'1'}
   'RemoteAddress' {'Any'}
   default {throw 'Unknown verification mismatch'}
  }
  return [pscustomobject]$copy
 }
 return $rule
}
function New-NetFirewallRule {
 [CmdletBinding()]param([string]$DisplayName,[string]$Group,[string]$Direction,[string]$Action,[string]$Enabled,[string]$Program,[string]$Protocol,[string]$LocalPort,[string]$RemoteAddress,[string]$Profile)
 $state=$global:SideWaveNetworkRegressionState
 if($state.FailWrite){throw 'Simulated firewall write denied'}
 Assert (-not $state.Rules.ContainsKey($DisplayName)) 'no duplicate rule creation'
 $rule=[pscustomobject]@{DisplayName=$DisplayName;Group=$Group;Direction=$Direction;Action=$Action;Enabled=$Enabled;Program=$Program;Protocol=$Protocol;LocalPort=$LocalPort;RemoteAddress=$RemoteAddress;Profile=$Profile}
 $state.Rules[$DisplayName]=$rule;$state.NewCount++
 return $rule
}
function Remove-NetFirewallRule {
 [CmdletBinding()]param([Parameter(ValueFromPipeline=$true)]$InputObject)
 process{
  $state=$global:SideWaveNetworkRegressionState
  Assert ($null -ne $InputObject -and $state.Rules.ContainsKey($InputObject.DisplayName)) 'remove only an existing rule'
  $state.Rules.Remove($InputObject.DisplayName);$state.RemoveCount++
 }
}
function Get-NetFirewallApplicationFilter {
 [CmdletBinding()]param([Parameter(ValueFromPipeline=$true)]$InputObject)
 process{[pscustomobject]@{Program=$InputObject.Program}}
}
function Get-NetFirewallPortFilter {
 [CmdletBinding()]param([Parameter(ValueFromPipeline=$true)]$InputObject)
 process{[pscustomobject]@{Protocol=$InputObject.Protocol;LocalPort=$InputObject.LocalPort}}
}
function Get-NetFirewallAddressFilter {
 [CmdletBinding()]param([Parameter(ValueFromPipeline=$true)]$InputObject)
 process{[pscustomobject]@{RemoteAddress=$InputObject.RemoteAddress}}
}
function Assert-Scopes {
 $state=$global:SideWaveNetworkRegressionState
 Assert ($state.Rules.Count -eq 3) 'exactly three rules'
 $expected=@(
  @{Name='SideWave control';Program=$app;Protocol='TCP';Port='55166'},
  @{Name='SideWave discovery';Program=$app;Protocol='UDP';Port='Any'},
  @{Name='SideWave video';Program=$receiver;Protocol='UDP';Port='55165'}
 )
 foreach($spec in $expected){
  $rule=$state.Rules[$spec.Name]
  Assert ($null -ne $rule) ('rule exists: '+$spec.Name)
  Assert ($rule.Group -eq 'SideWave' -and $rule.Enabled -eq 'True' -and $rule.Direction -eq 'Inbound' -and $rule.Action -eq 'Allow') ('rule attributes: '+$spec.Name)
  Assert ($rule.Profile -eq 'Private' -and $rule.RemoteAddress -eq 'LocalSubnet') ('bounded network scope: '+$spec.Name)
  Assert ($rule.Program -eq $spec.Program -and $rule.Protocol -eq $spec.Protocol -and $rule.LocalPort -eq $spec.Port) ('exact program/protocol/port: '+$spec.Name)
 }
}
function Expect-Failure([string]$message){
 $caught=$null
 try{& $installer -AppPath $app | Out-Null}catch{$caught=$_}
 Assert ($null -ne $caught) 'installer must fail'
 Assert ($caught.Exception.Message -like ('*'+$message+'*')) ('expected failure: '+$message+'; actual: '+$caught.Exception.Message)
}

try{
 New-Item -ItemType Directory -Path $fixture | Out-Null
 [IO.File]::WriteAllText($app,'test fixture; never executed')
 [IO.File]::WriteAllText($receiver,'test fixture; never executed')
 foreach($command in @('Get-NetFirewallRule','New-NetFirewallRule','Remove-NetFirewallRule','Get-NetFirewallApplicationFilter','Get-NetFirewallPortFilter','Get-NetFirewallAddressFilter')){
  Assert ((Get-Command $command).CommandType -eq 'Function') ('firewall command is mocked: '+$command)
 }
 Reset-Mock
 & $installer -AppPath $app | Out-Null
 Assert-Scopes
 Assert ($global:SideWaveNetworkRegressionState.NewCount -eq 3 -and $global:SideWaveNetworkRegressionState.RemoveCount -eq 0 -and $global:SideWaveNetworkRegressionState.ActiveReads -eq 3) 'first install creates and verifies without deleting absent rules'
 Write-Output 'PASS first install with no rules and exact scopes'

 & $installer -AppPath $app | Out-Null
 Assert-Scopes
 Assert ($global:SideWaveNetworkRegressionState.NewCount -eq 6 -and $global:SideWaveNetworkRegressionState.RemoveCount -eq 3 -and $global:SideWaveNetworkRegressionState.ActiveReads -eq 6) 'second install replaces existing rules without duplicates'
 Write-Output 'PASS second install is idempotent and preserves exact scopes'

 Reset-Mock;$global:SideWaveNetworkRegressionState.FailWrite=$true
 Expect-Failure 'Simulated firewall write denied'
 Assert ($global:SideWaveNetworkRegressionState.ActiveReads -eq 0) 'write failure cannot report verified success'
 Write-Output 'PASS firewall write failure propagates'

 foreach($field in @('Enabled','Direction','Action','Profile','Program','Protocol','LocalPort','RemoteAddress')){
  Reset-Mock;$global:SideWaveNetworkRegressionState.Mismatch=$field
  Expect-Failure 'Firewall rule verification failed: SideWave control'
  Write-Output ('PASS ActiveStore mismatch rejected: '+$field)
 }
 Write-Output 'PASS 11 mocked network setup regression scenarios; no real firewall calls'
}finally{
 Remove-Variable -Name $stateName -Scope Global -ErrorAction SilentlyContinue
 $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
 $resolved=[IO.Path]::GetFullPath($fixture)
 if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Refusing to remove fixture outside the temporary directory.'}
 if(Test-Path -LiteralPath $resolved){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
