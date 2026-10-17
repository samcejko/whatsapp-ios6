param(
    [string]$Url = '',
    [string]$Screenshot = '',
    [string]$Cmd = ''
)

$ErrorActionPreference = 'Stop'
$localCfg = Join-Path $PSScriptRoot 'local.json'
$cfg = Get-Content $localCfg -Raw | ConvertFrom-Json
$ipad = $cfg.ipad
$sshKey = Join-Path $env:USERPROFILE '.ssh\ipad_ios6'
$sshOpts = @('-i', $sshKey, '-o', 'StrictHostKeyChecking=no', '-o', 'HostKeyAlgorithms=+ssh-rsa', '-o', 'PubkeyAcceptedKeyTypes=+ssh-rsa')

if ($Url -and $Url.Length -gt 0) {
    Write-Host "Opening URL on iPad: $Url"
    & ssh @sshOpts root@$ipad "uiopen '$Url'"
}

if ($Cmd -and $Cmd.Length -gt 0) {
    Write-Host "Running command on iPad: $Cmd"
    & ssh @sshOpts root@$ipad $Cmd
}

if ($Screenshot -and $Screenshot.Length -gt 0) {
    Start-Sleep -Seconds 2
    & ssh @sshOpts root@$ipad "uiopen 'wazzup://shot'"
    Start-Sleep -Seconds 2
    & scp @sshOpts "root@${ipad}:/var/mobile/Documents/wa6-shot.png" $Screenshot
    Write-Host "Screenshot saved to $Screenshot"
}
