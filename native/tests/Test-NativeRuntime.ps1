[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $InstallDir,
    [Parameter(Mandatory)] [string] $Wav,
    [ValidateRange(1, 65535)] [int] $Port = 7777,
    [string] $Language = 'ru'
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $InstallDir).ProviderPath.TrimEnd('\') + '\'
$sample = Get-Item -LiteralPath $Wav
if ($sample.PSIsContainer) { throw 'A speech recording is required.' }
$address = "http://127.0.0.1:$Port"
function Get-ListenerId {
    $ids = @(Get-NetTCPConnection -LocalPort $Port -State Listen |
        Where-Object LocalAddress -eq '127.0.0.1' |
        Select-Object -ExpandProperty OwningProcess -Unique)
    if ($ids.Count -ne 1) { throw "Expected one LocalSTT listener on $address" }
    return $ids[0]
}
function Test-ProcessTree([int] $RootId) {
    $tree = @(Get-CimInstance Win32_Process)
    $queue = New-Object 'System.Collections.Generic.Queue[int]'
    $queue.Enqueue($RootId)
    $seen = New-Object 'System.Collections.Generic.HashSet[int]'
    while ($queue.Count -gt 0) {
        $currentId = $queue.Dequeue()
        if (-not $seen.Add($currentId)) { continue }
        $proc = Get-Process -Id $currentId -ErrorAction Stop
        if ($proc.ProcessName -match '^(python(w|[0-9.]*)?|py)$') {
            throw "Python process in service tree: $($proc.ProcessName)"
        }
        foreach ($module in $proc.Modules) {
            if ($module.ModuleName -match '^python[0-9._a-z]*\.dll$|\.pyd$' -or
                $module.FileName -match '[\\/]\.venv[\\/]') {
                throw "Python runtime dependency: $($module.ModuleName)"
            }
        }
        foreach ($child in $tree | Where-Object ParentProcessId -eq $currentId) {
            $queue.Enqueue([int]$child.ProcessId)
        }
    }
}
$listener = Get-ListenerId
$service = Get-Process -Id $listener
if (-not $service.Path -or -not $service.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
    throw "The listener does not belong to the native installation: $($service.Path)"
}
Test-ProcessTree $listener
$health = Invoke-RestMethod "$address/health"
if ($health.status -ne 'ok') { throw 'Health check failed.' }
# Use a simple local filename so curl's multipart parser never interprets characters in the source path.
$temp = Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString('N') + '.wav')
try {
    Copy-Item -LiteralPath $sample.FullName -Destination $temp
    Push-Location ([IO.Path]::GetDirectoryName($temp))
    try {
        $form = 'file=@' + [IO.Path]::GetFileName($temp)
        $raw = & curl.exe --silent --show-error --fail --max-time 600 -F $form -F "language=$Language" -F 'response_format=json' "$address/v1/audio/transcriptions"
        if ($LASTEXITCODE -ne 0) { throw 'Transcription request failed.' }
        $reply = $raw | ConvertFrom-Json
        foreach ($field in 'text','language','duration','processing_time','backend','device','model','compute_type') {
            if ($reply.PSObject.Properties.Name -notcontains $field) { throw "Missing response field: $field" }
        }
        if ([string]::IsNullOrWhiteSpace($reply.text) -or $reply.duration -le 0 -or $reply.device -ne 'cuda') {
            throw 'Expected nonempty CUDA transcription of the supplied speech sample.'
        }
    } finally { Pop-Location }
} finally { Remove-Item -LiteralPath $temp -ErrorAction SilentlyContinue }
if ((Get-ListenerId) -ne $listener) { throw 'Service process changed during transcription.' }
Test-ProcessTree $listener
[PSCustomObject]@{ Process = $service.ProcessName; Model = $reply.model; AudioSeconds = $reply.duration; ProcessingSeconds = $reply.processing_time }
