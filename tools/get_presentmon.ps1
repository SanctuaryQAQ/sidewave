$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$output=Join-Path $root 'build\bin\PresentMon.exe';$expected='B2A706BC6AD475749E3B7E3409263AA1E6906D45BDCF993F6DBC0F660188F1AF'
if(Test-Path -LiteralPath $output){if((Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash -ne $expected){throw 'PresentMon integrity check failed'};return}
New-Item -ItemType Directory -Path (Split-Path $output) -Force|Out-Null
$cached=Join-Path $root 'tests\private-bin\PresentMon-2.6.0-x64.exe'
if(Test-Path -LiteralPath $cached){if((Get-FileHash -LiteralPath $cached).Hash -ne $expected){throw 'Cached PresentMon integrity check failed'};Copy-Item -LiteralPath $cached -Destination $output;return}
$temporary=$output+'.download'
try{
 [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12
 Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/GameTechDev/PresentMon/releases/download/v2.6.0/PresentMon-2.6.0-x64.exe' -OutFile $temporary
 if((Get-FileHash -LiteralPath $temporary).Hash -ne $expected){throw 'Downloaded PresentMon integrity check failed'}
 Move-Item -LiteralPath $temporary -Destination $output
}finally{Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue}
