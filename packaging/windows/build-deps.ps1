<#
.SYNOPSIS
    Builds the libraries AIEdit uses on Windows when it finds them: KSyntaxHighlighting (syntax
    highlighting and colour schemes), uchardet (charset detection) and Capstone (disassembly).

.DESCRIPTION
    Each comes from its project's git repository at a fixed commit, is built with MSVC and Ninja,
    and is installed under -Prefix. Run it in a Visual Studio developer PowerShell or prompt for
    x64, with git, CMake, Ninja and Perl on PATH (KSyntaxHighlighting writes some of its syntax
    files with Perl). Then build AIEdit with both folders on CMAKE_PREFIX_PATH:

        cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=<Qt>;<Prefix>"

    Build AIEdit in Release or RelWithDebInfo: these libraries use Qt's release DLLs.

.EXAMPLE
    packaging\windows\build-deps.ps1 -Qt C:\Qt\6.10.3\msvc2022_64 -Prefix C:\aiedit-deps
#>
param(
    # Qt for MSVC, the folder with bin\, lib\ and plugins\.
    [Parameter(Mandatory = $true)] [string] $Qt,
    # Where the libraries are installed.
    [Parameter(Mandatory = $true)] [string] $Prefix,
    # Where the sources are checked out and built.
    [string] $WorkDir = (Join-Path ([System.IO.Path]::GetTempPath()) 'aiedit-deps')
)

$ErrorActionPreference = 'Stop'

# Runs a command and stops if it fails, which Windows PowerShell does not do by itself.
function Invoke-Checked([scriptblock] $Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "Failed with exit code ${LASTEXITCODE}: $Command"
    }
}

$libraries = @(
    @{
        # KDE's CMake modules, which KSyntaxHighlighting is built with.
        Name    = 'extra-cmake-modules'
        Url     = 'https://github.com/KDE/extra-cmake-modules.git'
        Tag     = 'v6.24.0'
        Commit  = '862b5b0ed8b2413a6c0e524a13640495dc5bb0d3'
        Options = @('-DBUILD_TESTING=OFF', '-DBUILD_DOC=OFF')
    },
    @{
        # The library alone: no tests, examples or QML bindings.
        Name    = 'syntax-highlighting'
        Url     = 'https://github.com/KDE/syntax-highlighting.git'
        Tag     = 'v6.24.0'
        Commit  = 'bbea2326fbd4fc720518efc43f1b018f8a7d0670'
        Options = @('-DBUILD_TESTING=OFF', '-DBUILD_QCH=OFF', '-DDISABLE_ALL_OPTIONAL_SUBDIRECTORIES=ON',
                    '-DCMAKE_DISABLE_FIND_PACKAGE_Qt6Quick=ON', '-DCMAKE_DISABLE_FIND_PACKAGE_XercesC=ON')
    },
    @{
        # Linked into aiedit.exe. It asks for CMake 3.1, which CMake 4 no longer emulates.
        Name    = 'uchardet'
        Url     = 'https://gitlab.freedesktop.org/uchardet/uchardet.git'
        Tag     = 'v0.0.8'
        Commit  = 'ae6302a016088ad07177f86d417b20010053632b'
        Options = @('-DBUILD_SHARED_LIBS=OFF', '-DBUILD_BINARY=OFF', '-DCHECK_SSE2=OFF',
                    '-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
    },
    @{
        # Linked into aiedit.exe, with the architectures AIEdit disassembles.
        Name    = 'capstone'
        Url     = 'https://github.com/capstone-engine/capstone.git'
        Tag     = '5.0.7'
        Commit  = '52c66920fc7bfa15fd9626dfd9f646c698aaa99b'
        Options = @('-DBUILD_SHARED_LIBS=OFF', '-DCAPSTONE_BUILD_TESTS=OFF', '-DCAPSTONE_BUILD_CSTOOL=OFF',
                    '-DCAPSTONE_ARCHITECTURE_DEFAULT=OFF', '-DCAPSTONE_X86_SUPPORT=ON', '-DCAPSTONE_ARM_SUPPORT=ON',
                    '-DCAPSTONE_ARM64_SUPPORT=ON', '-DCAPSTONE_RISCV_SUPPORT=ON')
    }
)

# KSyntaxHighlighting runs a tool of its own while it builds, which needs Qt's DLLs.
$env:PATH = "$(Join-Path $Qt 'bin');$env:PATH"
New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

foreach ($library in $libraries) {
    Write-Host "== $($library.Name) $($library.Tag)"
    $source = Join-Path $WorkDir $library.Name
    if (-not (Test-Path $source)) {
        $clone = @('-c', 'advice.detachedHead=false', 'clone', '--quiet', '--depth', '1', '--branch', $library.Tag,
                   $library.Url, $source)
        Invoke-Checked { git @clone }
    }
    $commit = (git -C $source rev-parse HEAD).Trim()
    if ($commit -ne $library.Commit) {
        throw "$($library.Name) $($library.Tag) is at commit $commit, not $($library.Commit)"
    }

    $build = "$source-build"
    $configure = @('-S', $source, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                   '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', "-DCMAKE_INSTALL_PREFIX=$Prefix",
                   "-DCMAKE_PREFIX_PATH=$Qt;$Prefix") + $library.Options
    Invoke-Checked { cmake @configure }
    Invoke-Checked { cmake --build $build }
    Invoke-Checked { cmake --install $build }
}
