<#
.SYNOPSIS
    Packs a Windows build of AIEdit: an installer, AIEdit-<version>-windows-x64-setup.exe, and the
    same files in AIEdit-<version>-windows-x64.zip for running without installing.

.DESCRIPTION
    Installs the build into <Output>\AIEdit with everything it needs to run (Qt, the libraries
    build-deps.ps1 built, the compiler's runtime), then packs that folder with Inno Setup 6 and as
    a zip. The installer speaks Chinese too, with Inno Setup's unofficial ChineseSimplified.isl,
    which is fetched once from the Inno Setup repository and checked against its git hash.

.EXAMPLE
    packaging\windows\package.ps1 -Build build -Output dist
#>
param(
    [string] $Build = 'build',
    [string] $Output = 'dist'
)

$ErrorActionPreference = 'Stop'

# Runs a command and stops if it fails, which Windows PowerShell does not do by itself.
function Invoke-Checked([scriptblock] $Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Failed with exit code ${LASTEXITCODE}: $Command"
    }
}

$cache = Get-Content (Join-Path $Build 'CMakeCache.txt')
$version = ($cache | Select-String -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$').Matches[0].Groups[1].Value
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$Output = (Resolve-Path $Output).Path

$stage = Join-Path $Output 'AIEdit'
if (Test-Path $stage) {
    Remove-Item -Recurse -Force $stage
}
Invoke-Checked { cmake --install $Build --prefix $stage }
foreach ($file in 'bin\aiedit.exe', 'bin\Qt6Core.dll', 'bin\vcruntime140.dll') {
    if (-not (Test-Path (Join-Path $stage $file))) {
        throw "$file is missing from $stage"
    }
}
if (-not (Get-ChildItem -Path $stage -Recurse -Filter 'qwindows.dll')) {
    throw "Qt's platform plugin (qwindows.dll) is missing from $stage"
}

$isl = Join-Path $Output 'ChineseSimplified.isl'
if (-not (Test-Path $isl)) {
    Invoke-WebRequest -UseBasicParsing -OutFile $isl `
        -Uri 'https://raw.githubusercontent.com/jrsoftware/issrc/is-6_7_1/Files/Languages/Unofficial/ChineseSimplified.isl'
}
$islHash = (git hash-object $isl).Trim()
if ($islHash -ne 'd6a11c4490de07dad443ade668289fc954dfa1ed') {
    throw "$isl is not the file expected (git hash $islHash)"
}

$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) {
    throw "Inno Setup 6 is not installed (no $iscc)"
}
$script = Join-Path $PSScriptRoot 'aiedit.iss'
Invoke-Checked {
    & $iscc /Q "/DAppVersion=$version" "/DSourceDir=$stage" "/DOutputDir=$Output" "/DChineseIsl=$isl" $script
}

$zip = Join-Path $Output "AIEdit-$version-windows-x64.zip"
Compress-Archive -Path $stage -DestinationPath $zip -Force
Get-ChildItem -Path $Output -File | Format-Table Name, Length
