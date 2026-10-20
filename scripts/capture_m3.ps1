param(
    [string]$VsRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$python = Join-Path $projectRoot ".venv/Scripts/python.exe"

function Get-CleanRevision {
    $status = (& git status --porcelain=v1 --untracked-files=normal) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $status) {
        throw "Evidence capture requires a clean tree, including untracked files. Commit fixes first."
    }
    $revision = & git rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw "Cannot identify HEAD." }
    return $revision
}

function Invoke-Check {
    param([string[]]$Arguments, [string]$LogName)
    & $python @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $stage $LogName)
    if ($LASTEXITCODE -ne 0) {
        throw "Check failed: $Arguments. See $LogName."
    }
}

Push-Location $projectRoot
try {
    $revision = Get-CleanRevision
    $stage = Join-Path $projectRoot ("build/m3-capture-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $stage | Out-Null
    $env:PATH = (Join-Path $projectRoot ".venv/Scripts") + [IO.Path]::PathSeparator + $env:PATH
    & (Join-Path $PSScriptRoot "test.ps1") -Preset debug -VsRoot $VsRoot -OutputDirectory $stage
    & (Join-Path $PSScriptRoot "test.ps1") -Preset asan -VsRoot $VsRoot -OutputDirectory $stage
    foreach ($preset in @("debug", "asan")) {
        $mutationArgs = @("scripts/verify_m3_mutations.py", "build/$preset", "--output", (Join-Path $stage "$preset-mutations.jsonl"))
        if ($preset -eq "debug") { $mutationArgs += @("--survey", "--jobs", "3") }
        Invoke-Check -Arguments $mutationArgs -LogName "$preset-mutations-driver.txt"
    }
    Invoke-Check -Arguments @("scripts/check_tidy.py", "--output", (Join-Path $stage "clang-tidy.txt")) -LogName "tidy-driver.txt"
    Invoke-Check -Arguments @("scripts/verify_replay.py", "build/debug/quorumkv_tests.exe", "--output", (Join-Path $stage "replay.txt")) -LogName "replay-driver.txt"
    Invoke-Check -Arguments @("scripts/verify_sim_replay.py", "build/debug/quorumkv_tests.exe", "--compare-executable", "build/asan/quorumkv_tests.exe", "--output", (Join-Path $stage "sim-replay.txt")) -LogName "sim-replay-driver.txt"
    Invoke-Check -Arguments @("scripts/verify_replication_replay.py", "build/debug/quorumkv_tests.exe", "--compare-executable", "build/asan/quorumkv_tests.exe", "--output", (Join-Path $stage "replication-replay.txt")) -LogName "replication-replay-driver.txt"
    Invoke-Check -Arguments @("-m", "pytest", "tests/python", "-q", "--basetemp", (Join-Path $stage "pytest-tmp")) -LogName "python-tests.txt"
    Invoke-Check -Arguments @("scripts/check_format.py") -LogName "format.txt"
    Invoke-Check -Arguments @("-m", "ruff", "check", ".") -LogName "ruff-check.txt"
    Invoke-Check -Arguments @("-m", "ruff", "format", "--check", ".") -LogName "ruff-format.txt"
    @(
        "clang-format --dry-run --Werror: PASS"
        (Get-Content -LiteralPath (Join-Path $stage "ruff-check.txt") -Raw)
        (Get-Content -LiteralPath (Join-Path $stage "ruff-format.txt") -Raw)
    ) | Set-Content -Encoding utf8 (Join-Path $stage "lint.txt")
    Invoke-Check -Arguments @("scripts/summarize_m3.py", "--results-dir", $stage) -LogName "summary-driver.txt"
    Invoke-Check -Arguments @("scripts/fetch_ci.py", "--revision", $revision, "--output-dir", $stage, "--wait-seconds", "3600") -LogName "ci-driver.txt"
    if ((Get-CleanRevision) -ne $revision) {
        throw "Source HEAD changed during evidence capture."
    }
    $destination = Join-Path $projectRoot "results/m3"
    New-Item -ItemType Directory -Force -Path $destination | Out-Null
    # Publish only after all gates pass and the tested tree is still clean.
    $files = @(
        "debug-configure.txt", "debug-build.txt", "debug-tests.txt", "debug-tools.json",
        "asan-configure.txt", "asan-build.txt", "asan-tests.txt", "asan-tools.json",
        "clang-tidy.txt", "python-tests.txt", "replay.txt", "sim-replay.txt", "lint.txt", "summary.json",
        "debug-mutations.jsonl", "asan-mutations.jsonl", "replication-replay.txt", "ci.json", "ci-tsan.txt"
    )
    foreach ($file in $files) {
        Copy-Item -LiteralPath (Join-Path $stage $file) -Destination (Join-Path $destination $file) -Force
    }
    Write-Output "Published evidence from clean source HEAD $revision. Commit only results/m3 next."
}
finally {
    Pop-Location
}
