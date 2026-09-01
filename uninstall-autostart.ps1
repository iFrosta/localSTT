$startup = [Environment]::GetFolderPath("Startup")
$shortcut = Join-Path $startup "LocalSTT.lnk"
if (Test-Path $shortcut) {
    Remove-Item $shortcut
    "Removed per-user autostart shortcut: $shortcut"
} else {
    "Autostart shortcut was not present: $shortcut"
}
