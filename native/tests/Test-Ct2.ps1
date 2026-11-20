param(
    [string]$InstallDir = (Join-Path $PSScriptRoot '..\..\native-install'),
    [string]$Wav = (Join-Path $PSScriptRoot '..\downloads\jfk.wav'),
    [string]$StateRoot = (Join-Path $PSScriptRoot '..\test-state'),
    [int]$TimeoutSeconds = 600,
    [switch]$Live
)
$ErrorActionPreference = 'Stop'
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$Wav = (Resolve-Path -LiteralPath $Wav).Path
$StateRoot = [IO.Path]::GetFullPath($StateRoot)
$exe = Join-Path $InstallDir 'test_localstt_ct2.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build the test_localstt_ct2 target first: $exe" }
if ($TimeoutSeconds -lt 1) { throw 'TimeoutSeconds must be positive' }
New-Item -ItemType Directory -Force -Path $StateRoot | Out-Null
$stdout = Join-Path $StateRoot 'ct2-test-stdout.txt'
$stderr = Join-Path $StateRoot 'ct2-test-stderr.txt'
# These arguments are filesystem paths, not shell commands. Double quotes are
# invalid in Windows paths and are rejected rather than interpolated.
foreach ($path in @($Wav, $StateRoot, $InstallDir)) {
    if ($path.Contains('"')) { throw 'Invalid quote in a filesystem path' }
}
$arguments = '"' + $Wav + '" "' + $StateRoot.TrimEnd('\') + '" "' + $InstallDir.TrimEnd('\') + '"'
if ($Live) { $arguments += ' --live' }
$process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $InstallDir -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
try {
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        $process.Kill()
        $process.WaitForExit()
        throw "CT2 integration test timed out after $TimeoutSeconds seconds"
    }
    # Refresh the process handle to make the exit code available consistently.
    $process.Refresh()
    Get-Content -LiteralPath $stdout -Encoding UTF8
    if ($process.ExitCode -ne 0) {
        Get-Content -LiteralPath $stderr -Encoding UTF8
        throw "CT2 integration tests failed with exit code $($process.ExitCode)"
    }
} finally {
    $process.Dispose()
}
