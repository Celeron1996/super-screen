# Register/uninstall the super_screen_win logon autostart task.
# No Windows service (session 0 has no interactive desktop / display topology).
# See docs/plan-windows.md section 6 (M2 autostart).
param(
    [string]$ExePath = '',
    [string]$TaskName = 'super_screen_win',
    [int]$DelaySeconds = 30,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

if ($Uninstall) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Output "removed scheduled task '$TaskName'"
    } else {
        Write-Output "scheduled task '$TaskName' not found"
    }
    exit 0
}

if ($ExePath -eq '') {
    $ExePath = Join-Path $PSScriptRoot '..\build\x64\Release\super_screen_win.exe'
}
$ExePath = [IO.Path]::GetFullPath($ExePath)
if (-not (Test-Path -LiteralPath $ExePath)) {
    throw "executable not found: $ExePath"
}
$workDir = Split-Path -Parent $ExePath

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}

$action = New-ScheduledTaskAction -Execute $ExePath -WorkingDirectory $workDir
$trigger = New-ScheduledTaskTrigger -AtLogOn
$trigger.Delay = ('PT{0}S' -f $DelaySeconds)
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Seconds 0) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId ('{0}\{1}' -f $env:USERDOMAIN, $env:USERNAME) `
    -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -Principal $principal -Description 'super_screen_win: virtual secondary display sender' | Out-Null

Write-Output "registered scheduled task '$TaskName'"
Write-Output "  exe:     $ExePath"
Write-Output "  workdir: $workDir"
Write-Output "  trigger: at logon, delay ${DelaySeconds}s"
