# Remove the legacy super_screen_win logon autostart task.
# The app now creates the virtual display only after the user launches it.
# Run from an elevated PowerShell if the task was registered by an administrator.
param(
    [string]$TaskName = 'super_screen_win'
)

$ErrorActionPreference = 'Stop'

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Output "removed scheduled task '$TaskName'"
} else {
    Write-Output "scheduled task '$TaskName' not found"
}
