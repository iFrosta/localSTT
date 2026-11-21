<#
.SYNOPSIS
    Renders the settings window of the native application and saves the images the
    README uses.

.DESCRIPTION
    Starts its own LocalSTT with an empty state folder and a spare API port, so the
    images show the default configuration rather than this machine's, then closes it.
    Only one LocalSTT runs at a time: stop the running one first (stop-localstt.ps1).

    Health, Voice commands and AI cleanup are left out: each reports the machine it ran
    on, so a committed screenshot of one machine would document nothing.

.EXAMPLE
    .\stop-localstt.ps1; .\docs\capture-screenshots.ps1; .\start-localstt.vbs
#>
[CmdletBinding()]
param([string]$Out)

$ErrorActionPreference = 'Stop'
# Windows PowerShell leaves $PSScriptRoot empty in parameter defaults.
if (-not $Out) { $Out = Join-Path $PSScriptRoot 'images' }
. (Join-Path $PSScriptRoot '..\_env.ps1')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Screens {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string c, string t);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
}
'@
# Per-monitor aware, so the image has the window's real pixels rather than a scaled copy.
[void][Screens]::SetThreadDpiAwarenessContext([IntPtr](-4))

# Section index in the settings navigation -> file name.
$pages = [ordered]@{ 0 = 'settings-general.png'; 2 = 'settings-hotkeys.png'; 3 = 'settings-delivery.png' }
$WM_COMMAND = 0x111; $WM_CLOSE = 0x10; $settingsCommand = 216; $firstSection = 400

if (Get-Process LocalSTT -ErrorAction SilentlyContinue) {
    throw 'LocalSTT is running. Stop it first with stop-localstt.ps1: only one instance can run.'
}

function Find-Window([string]$Class, [int]$ProcessId) {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $handle = [Screens]::FindWindow($Class, [NullString]::Value)
        if ($handle -ne [IntPtr]::Zero) {
            [uint32]$owner = 0
            [void][Screens]::GetWindowThreadProcessId($handle, [ref]$owner)
            if ($owner -eq $ProcessId) { return $handle }
        }
        Start-Sleep -Milliseconds 100
    }
    throw "No $Class window from process $ProcessId"
}

function Save-Client([IntPtr]$Window, [string]$Path) {
    $rect = New-Object Screens+Rect
    [void][Screens]::GetClientRect($Window, [ref]$rect)
    $bitmap = New-Object Drawing.Bitmap $rect.Right, $rect.Bottom
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        # PW_CLIENTONLY | PW_RENDERFULLCONTENT: the client area as DWM composes it.
        if (-not [Screens]::PrintWindow($Window, $dc, 3)) { throw 'PrintWindow failed' }
    } finally { $graphics.ReleaseHdc($dc); $graphics.Dispose() }
    try { $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
}

$root = Get-LocalSttRoot
$state = Join-Path ([IO.Path]::GetTempPath()) "localstt-screenshots-$PID"
New-Item -ItemType Directory -Force -Path $state, $Out | Out-Null
$process = Start-Process -FilePath (Get-LocalSttNative) -PassThru -WorkingDirectory $root `
    -ArgumentList '--root', "`"$root`"", '--state-dir', "`"$state`"", '--port', '17781'
$hostWindow = [IntPtr]::Zero
try {
    $hostWindow = Find-Window 'LocalSTT.Native.Host' $process.Id
    [void][Screens]::PostMessage($hostWindow, $WM_COMMAND, [IntPtr]$settingsCommand, [IntPtr]::Zero)
    $settings = Find-Window 'LocalSTT.Native.Settings' $process.Id
    foreach ($page in $pages.GetEnumerator()) {
        [void][Screens]::SendMessage($settings, $WM_COMMAND, [IntPtr]($firstSection + $page.Key), [IntPtr]::Zero)
        Start-Sleep -Milliseconds 400
        Save-Client $settings (Join-Path $Out $page.Value)
        "Saved $(Join-Path $Out $page.Value)"
    }
} finally {
    if ($hostWindow -ne [IntPtr]::Zero) { [void][Screens]::PostMessage($hostWindow, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (-not $process.WaitForExit(20000)) { Stop-Process -Id $process.Id -Force }
    Remove-Item $state -Recurse -Force -ErrorAction SilentlyContinue
}
