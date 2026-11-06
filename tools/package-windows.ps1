param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$Version = '0.1.0',
    [string]$ConverterExecutable,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../dist')
)
$ErrorActionPreference = 'Stop'
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$outputRoot = [IO.Path]::GetFullPath($OutputDirectory)
$bundle = Join-Path $outputRoot "PvzDevTools-$Version-windows-x64"
if (Test-Path -LiteralPath $bundle) { throw "Package directory already exists: $bundle" }
New-Item -ItemType Directory -Path $bundle -Force | Out-Null
$programs = @{
    'pvz-resource-build.exe' = (Join-Path $buildRoot 'pvz-resource-build.exe')
    'reanimc.exe' = (Join-Path $buildRoot 'resourceCompiler/src/reanimc.exe')
    'pvz_dat64_converter.exe' = (Join-Path $buildRoot 'saveConverter/src/pvz_dat64_converter.exe')
}
foreach ($name in @($programs.Keys)) {
    if ($name -eq 'pvz_dat64_converter.exe' -and $ConverterExecutable) {
        $programs[$name] = (Resolve-Path -LiteralPath $ConverterExecutable).Path
    }
    if (-not (Test-Path -LiteralPath $programs[$name])) { throw "Missing program: $($programs[$name])" }
    Copy-Item -LiteralPath $programs[$name] -Destination (Join-Path $bundle $name)
}
foreach ($library in Get-ChildItem -LiteralPath $buildRoot -Filter '*.dll' -File) {
    Copy-Item -LiteralPath $library.FullName -Destination $bundle
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../LICENSE') -Destination $bundle
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../README.md') -Destination $bundle
if (Test-Path -LiteralPath (Join-Path $PSScriptRoot '../third-party-notices')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../third-party-notices') -Destination $bundle -Recurse
}
$rows = Get-ChildItem -LiteralPath $bundle -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{path=[IO.Path]::GetRelativePath($bundle,$_.FullName).Replace('\','/');bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
[ordered]@{format='pvz-devtools-binaries-v1';version=$Version;platform='windows-x64';files=@($rows)} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $bundle 'manifest.json') -Encoding utf8NoBOM
$zip = Join-Path $outputRoot "PvzDevTools-$Version-windows-x64.zip"
Compress-Archive -Path (Join-Path $bundle '*') -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash -LiteralPath $zip -Algorithm SHA256 | ForEach-Object {
    "$($_.Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($zip))" |
        Set-Content -LiteralPath (Join-Path $outputRoot 'SHA256SUMS.txt') -Encoding ascii
}
Write-Output $zip
