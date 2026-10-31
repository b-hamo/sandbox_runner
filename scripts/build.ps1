#Requires -Version 5.1
<# Build the Windows product with one UCRT64 toolchain; never launch a Sandbox. #>
[CmdletBinding()]
param(
    [string]$UcrtRoot = '',
    [string]$BuildDir = '',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $UcrtRoot) {
    if ($env:MSYS2_ROOT) { $UcrtRoot = Join-Path $env:MSYS2_ROOT 'ucrt64' }
    else {
        $candidate = Get-Command cmake.exe -ErrorAction SilentlyContinue
        if ($candidate -and $candidate.Source -match '[\\/]ucrt64[\\/]bin[\\/]') {
            $UcrtRoot = Split-Path (Split-Path $candidate.Source -Parent) -Parent
        } else { $UcrtRoot = 'C:\msys64\ucrt64' }
    }
}
$UcrtRoot = [IO.Path]::GetFullPath($UcrtRoot)
if ((Split-Path $UcrtRoot -Leaf) -ne 'ucrt64') { throw 'UcrtRoot must be the MSYS2 ucrt64 directory.' }
$bin = Join-Path $UcrtRoot 'bin'
foreach ($name in @('cmake.exe','ninja.exe','g++.exe','objdump.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $name) -PathType Leaf)) {
        throw "Missing UCRT64 $name. Install MSYS2 UCRT64 gcc, cmake, ninja and jsoncpp packages; pass -UcrtRoot."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $UcrtRoot 'lib/cmake/jsoncpp/jsoncppConfig.cmake'))) {
    throw 'Missing JsonCpp: install mingw-w64-ucrt-x86_64-jsoncpp.'
}
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build/solution' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if ($BuildDir -eq $repo -or -not $BuildDir.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDir must be a dedicated directory inside the Runner repository.'
}
$oldPath = $env:PATH
$env:PATH = $bin + ';' + $env:PATH
try {
    $compiler = Join-Path $bin 'g++.exe'
    $target = (& $compiler -dumpmachine) -join ''
    if ($LASTEXITCODE -ne 0 -or $target -ne 'x86_64-w64-mingw32') { throw 'Expected Windows x64 GCC target.' }
    if ($CheckOnly) { Write-Host "RUNNER_BUILD_READY: $UcrtRoot"; return }
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    $receipt = Join-Path $BuildDir 'build-info.json'
    # Invalidate the receipt before building, so a failed rebuild cannot look current.
    if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }
    & (Join-Path $bin 'cmake.exe') -S $repo -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_CXX_COMPILER=$($compiler.Replace('\','/'))" `
        "-DCMAKE_MAKE_PROGRAM=$((Join-Path $bin 'ninja.exe').Replace('\','/'))" `
        "-DCMAKE_PREFIX_PATH=$($UcrtRoot.Replace('\','/'))"
    if ($LASTEXITCODE -ne 0) { throw 'Runner CMake configure failed.' }
    & (Join-Path $bin 'cmake.exe') --build $BuildDir --target sandbox_runner
    if ($LASTEXITCODE -ne 0) { throw 'Runner build failed.' }
    $exe = Join-Path $BuildDir 'sandbox_runner.exe'
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw 'Runner executable missing after build.' }
    # binutils may decode absolute Unicode arguments using the system ANSI code page.
    Push-Location -LiteralPath $BuildDir
    try {
        $imports = (& (Join-Path $bin 'objdump.exe') -p './sandbox_runner.exe') | Select-String 'DLL Name:'
        if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Runner dependencies.' }
    } finally { Pop-Location }
    if ($imports -match '(?i)(libgcc|libstdc|libwinpthread|jsoncpp|libssl|libcrypto).*\.dll') {
        throw 'Runner has non-system runtime DLL dependencies; package them before deployment.'
    }
    $commit = (& git -C $repo rev-parse HEAD) -join ''
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record Runner source commit.' }
    $dirty = [bool]((& git -C $repo status --porcelain --untracked-files=normal) -join '')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record Runner working tree state.' }
    $info = [ordered]@{ schema_version=1; executable=$exe; sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLower();
        source=$repo; commit=$commit; dirty=$dirty; ucrt_root=$UcrtRoot; compiler=((& $compiler --version) | Select-Object -First 1);
        built_at=[DateTime]::UtcNow.ToString('o'); dll_imports=@($imports | ForEach-Object { $_.Line.Trim() }) }
    $info | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($receipt+'.tmp') -Encoding UTF8
    Move-Item -LiteralPath ($receipt+'.tmp') -Destination $receipt -Force
    Write-Host "RUNNER_BUILT: $receipt"
} finally { $env:PATH = $oldPath }
