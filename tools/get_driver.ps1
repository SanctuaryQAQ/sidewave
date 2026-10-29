$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$archive=Join-Path $root 'third_party\vdd-25.7.23.zip'
$expected='E24210692B442B39AF763536330CE78B423F19342B7A7792C26DE3944E418B3A'
if(-not(Test-Path $archive)){
 Invoke-WebRequest 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip' -OutFile $archive -UseBasicParsing
}
if((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected){throw 'Official driver ZIP hash differs from the pinned release.'}
$unpack=Join-Path $root 'third_party\vdd-25.7.23'
Expand-Archive -LiteralPath $archive -DestinationPath $unpack -Force
$driver=Join-Path $root 'build\bin\driver'
New-Item -ItemType Directory -Path $driver -Force|Out-Null
foreach($name in @('MttVDD.inf','MttVDD.dll','mttvdd.cat')){Copy-Item -LiteralPath (Join-Path $unpack "VirtualDisplayDriver\$name") -Destination $driver -Force}
$signature=Get-AuthenticodeSignature -LiteralPath (Join-Path $driver 'mttvdd.cat')
if($signature.Status -ne 'Valid'){throw 'Driver catalog signature is invalid.'}
Copy-Item (Join-Path $root 'assets\pyroscreen-vdd-settings.xml') $driver -Force
