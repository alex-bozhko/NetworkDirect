<#
.SYNOPSIS
    Packages the build outputs into a binaries zip, a symbols zip and SHA256SUMS.txt.

.DESCRIPTION
    Run after building src\netdirect.sln. The file list comes from the projects in
    the source tree, not from the contents of out\, so stale outputs are never
    packaged. Package layout:

        <name>\ndprov.dll, ndprov_install.exe, README.md, LICENSE.txt
        <name>\tools\<example>.exe
        <name>\tests\<unittest>.exe

    The symbols zip mirrors this layout with .pdb files.

.EXAMPLE
    .github\scripts\package.ps1 -Version 1.2.3 -FileVersion 1.2.3.0
#>
[CmdletBinding()]
param(
    # Version label used in archive names, e.g. 1.2.3 or 1.2.3-rc.1.
    [Parameter(Mandatory)]
    [string] $Version,

    # If set, every binary that has a version resource must report this FileVersion.
    [string] $FileVersion,

    [string] $Configuration = 'Release',
    [string] $Platform = 'x64',
    [string] $OutputDirectory = 'artifacts'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildDir = Join-Path $root "out\$Configuration-$Platform"
$name = "NetworkDirect-USB4-$Version-$Platform"
$stage = Join-Path $root 'out\package'
$binRoot = Join-Path $stage $name
$symRoot = Join-Path $stage "$name-symbols"
$outDir = Join-Path $root $OutputDirectory

function Get-ApplicationProjects([string] $dir) {
    Get-ChildItem (Join-Path $root $dir) -Filter *.vcxproj -Recurse |
        Where-Object { Select-String -Path $_.FullName -Pattern '<ConfigurationType>Application<' -Quiet } |
        ForEach-Object BaseName |
        Sort-Object
}

$layout = [ordered]@{
    '.'     = @('ndprov', 'ndprov_install')
    'tools' = @(Get-ApplicationProjects 'src\examples')
    'tests' = @(Get-ApplicationProjects 'src\unittests')
}

foreach ($dir in $stage, $outDir) {
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    New-Item $dir -ItemType Directory | Out-Null
}

foreach ($sub in $layout.Keys) {
    $binDest = Join-Path $binRoot $sub
    $symDest = Join-Path $symRoot $sub
    New-Item $binDest, $symDest -ItemType Directory -Force | Out-Null

    foreach ($project in $layout[$sub]) {
        $projectDir = Join-Path $buildDir $project
        $binary = Get-ChildItem $projectDir -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -in "$project.exe", "$project.dll" }
        $pdb = Join-Path $projectDir "$project.pdb"
        if (-not $binary) { throw "Missing build output for '$project' in $projectDir" }
        if (-not (Test-Path $pdb)) { throw "Missing symbols for '$project': $pdb" }

        if ($FileVersion) {
            $actual = $binary.VersionInfo.FileVersion
            if ($actual -and $actual -ne $FileVersion) {
                throw "$($binary.Name) has FileVersion '$actual', expected '$FileVersion'"
            }
            if (-not $actual -and $project -eq 'ndprov') {
                throw "$($binary.Name) has no version resource"
            }
        }

        Copy-Item $binary.FullName $binDest
        Copy-Item $pdb $symDest
        Write-Host "  $sub\$($binary.Name)"
    }
}

Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE.txt') $binRoot
Copy-Item (Join-Path $root 'LICENSE.txt') $symRoot

Compress-Archive -Path $binRoot -DestinationPath (Join-Path $outDir "$name.zip")
Compress-Archive -Path $symRoot -DestinationPath (Join-Path $outDir "$name-symbols.zip")

$sums = Get-ChildItem $outDir -Filter *.zip | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
}
Set-Content (Join-Path $outDir 'SHA256SUMS.txt') $sums -Encoding ascii

Remove-Item $stage -Recurse -Force
Get-ChildItem $outDir | Format-Table Name, Length -AutoSize | Out-String | Write-Host
