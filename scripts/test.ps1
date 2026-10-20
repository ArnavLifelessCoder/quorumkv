param(
    [ValidateSet("debug", "asan", "tsan")]
    [string]$Preset = "debug",
    [string]$VsRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$logDirectory = Join-Path $projectRoot "results/m1"
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

function Invoke-Logged {
    param([string]$Program, [string[]]$Arguments, [string]$LogName)
    & $Program @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $logDirectory $LogName)
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE. See $LogName."
    }
}

Push-Location $projectRoot
try {
    if ($env:OS -eq "Windows_NT") {
        if ($Preset -eq "tsan") {
            throw "Run the tsan preset on Linux."
        }
        & (Join-Path $VsRoot "Common7/Tools/Launch-VsDevShell.ps1") -Arch amd64 -SkipAutomaticLocation
        if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
            throw "MSVC is not available in the developer environment."
        }
    }
    $toolInfo = [ordered]@{
        preset = $Preset
        utc = [DateTime]::UtcNow.ToString("o")
        cmake = ((& cmake --version) -join "`n")
        ninja = ((& ninja --version) -join "`n")
        python = ((& python --version) -join "`n")
        compiler = if ($env:OS -eq "Windows_NT") { (Get-Command cl).Source } else { $env:CXX }
        source_revision = (& git rev-parse HEAD)
        source_status = ((& git status --short) -join "`n")
    }
    $toolInfo | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $logDirectory "$Preset-tools.json")
    Invoke-Logged -Program "cmake" -Arguments @("--preset", $Preset) -LogName "$Preset-configure.txt"
    Invoke-Logged -Program "cmake" -Arguments @("--build", "--preset", $Preset, "--verbose") -LogName "$Preset-build.txt"
    Invoke-Logged -Program "ctest" -Arguments @("--preset", $Preset, "--verbose") -LogName "$Preset-tests.txt"
}
finally {
    Pop-Location
}
