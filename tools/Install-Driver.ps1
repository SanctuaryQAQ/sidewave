$ErrorActionPreference='Stop'
[Console]::OutputEncoding=New-Object System.Text.UTF8Encoding
$driver=Join-Path $PSScriptRoot 'driver'
$catalog=Join-Path $driver 'mttvdd.cat'
$signature=Get-AuthenticodeSignature -LiteralPath $catalog
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'SignPath Foundation'){
 throw 'The packaged Virtual Display Driver catalog signature is not valid.'
}
$configuration=Join-Path $env:SystemDrive 'VirtualDisplayDriver'
if(-not(Test-Path $configuration)){New-Item -ItemType Directory -Path $configuration|Out-Null}
$xml=Join-Path $configuration 'vdd_settings.xml'
if(-not(Test-Path $xml)){
 Copy-Item -LiteralPath (Join-Path $driver 'pyroscreen-vdd-settings.xml') -Destination $xml
}else{
 Write-Output 'Existing VDD settings preserved; available display modes come from that configuration.'
}
& (Join-Path $env:SystemRoot 'System32\pnputil.exe') /add-driver (Join-Path $driver 'MttVDD.inf') /install
if($LASTEXITCODE -notin @(0,3010)){throw "PnPUtil failed with exit code $LASTEXITCODE"}
$reboot=$LASTEXITCODE -eq 3010
$packages=@(Get-ChildItem -LiteralPath (Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository') -Directory -Filter 'mttvdd.inf_*')
if(-not $packages.Count){throw 'Virtual display driver package was not found in the Windows driver store after installation.'}
if($reboot){Write-Output 'PYROSCREEN_REBOOT_REQUIRED'}
Write-Output 'Verified Virtual Display Driver package in Windows driver store. The display device is created on connection.'
