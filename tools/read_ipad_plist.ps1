$localCfg = Join-Path $PSScriptRoot 'local.json'
$cfg = Get-Content $localCfg -Raw | ConvertFrom-Json
$ipad = $cfg.ipad
$sshKey = Join-Path $env:USERPROFILE '.ssh\ipad_ios6'
$sshOpts = @('-i', $sshKey, '-o', 'StrictHostKeyChecking=no', '-o', 'HostKeyAlgorithms=+ssh-rsa', '-o', 'PubkeyAcceptedKeyTypes=+ssh-rsa')

& scp @sshOpts "root@${ipad}:/var/mobile/Documents/wa6.chats.plist" "tools/wa6.chats.plist"
Write-Host "Downloaded plist to tools/wa6.chats.plist"
