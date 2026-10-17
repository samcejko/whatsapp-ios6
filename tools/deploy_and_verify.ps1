$ErrorActionPreference = 'Stop'
$localCfg = 'c:\Users\samcejko\Documents\whatsapp-ios6\tools\local.json'
$cfg = Get-Content $localCfg -Raw | ConvertFrom-Json
$token = $cfg.token
$repo = $cfg.repo
$ipad = $cfg.ipad
$sshKey = Join-Path $env:USERPROFILE '.ssh\ipad_ios6'

[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$headers = @{
    Authorization          = "Bearer $token"
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
    'User-Agent'           = 'whatsapp-ios6-deploy'
}

Write-Host "Fetching release info for ci-ios6-armv7..."
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/tags/ci-ios6-armv7" -Headers $headers
$asset = $release.assets | Where-Object { $_.name -eq 'Wazzup-0.1.0.ipa' } | Select-Object -First 1

if (-not $asset) {
    throw "Wazzup-0.1.0.ipa not found in release assets!"
}

Write-Host "Found asset: $($asset.name), size: $($asset.size), updated: $($asset.updated_at)"
$outDir = 'c:\Users\samcejko\Documents\whatsapp-ios6\packages'
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir -Force | Out-Null }
$ipaPath = Join-Path $outDir 'Wazzup-0.1.0.ipa'

Write-Host "Downloading IPA from $($asset.url)..."
& curl.exe -sSL -f -H "Authorization: Bearer $token" -H "Accept: application/octet-stream" -o $ipaPath $asset.url
if ($LASTEXITCODE -ne 0) { throw "Failed to download IPA" }

Write-Host "Downloaded IPA ($([math]::Round((Get-Item $ipaPath).Length / 1KB)) KB). Extracting Payload..."
$extractDir = Join-Path $outDir 'extracted'
if (Test-Path $extractDir) { Remove-Item -Recurse -Force $extractDir }
$zipPath = Join-Path $outDir 'Wazzup.zip'
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Copy-Item $ipaPath $zipPath
Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

$binPath = Join-Path $extractDir 'Payload\Wazzup.app\Wazzup'
if (-not (Test-Path $binPath)) { throw "Binary not found at $binPath" }

Write-Host "Binary extracted successfully: $(Get-Item $binPath).Length bytes"

$sshOpts = @(
    '-i', $sshKey,
    '-o', 'StrictHostKeyChecking=no',
    '-o', 'HostKeyAlgorithms=+ssh-rsa',
    '-o', 'PubkeyAcceptedKeyTypes=+ssh-rsa'
)

Write-Host "Deploying binary and resources to iPad ($ipad)..."
& scp @sshOpts $binPath "root@${ipad}:/Applications/Wazzup.app/Wazzup"
if ($LASTEXITCODE -ne 0) { throw "Failed to scp binary" }

& scp @sshOpts "c:\Users\samcejko\Documents\whatsapp-ios6\Resources\cs.lproj\Localizable.strings" "root@${ipad}:/Applications/Wazzup.app/cs.lproj/Localizable.strings"
& scp @sshOpts "c:\Users\samcejko\Documents\whatsapp-ios6\Resources\en.lproj\Localizable.strings" "root@${ipad}:/Applications/Wazzup.app/en.lproj/Localizable.strings"

Write-Host "Chmod and relaunching Wazzup..."
& ssh @sshOpts root@$ipad "chmod 755 /Applications/Wazzup.app/Wazzup; killall -9 Wazzup 2>/dev/null || true; sleep 1; sblaunch com.samcejko.wazzup"

Write-Host "Wazzup relaunched. Waiting for app to initialize..."
Start-Sleep -Seconds 3

Write-Host "Triggering refresh and screenshot on iPad..."
& ssh @sshOpts root@$ipad "uiopen 'wazzup://refresh'; sleep 2; uiopen 'wazzup://shot'"

Start-Sleep -Seconds 2

$destPng = "C:\Users\samcejko\.gemini\antigravity\brain\5e073f8c-194b-4481-a305-d824a4e7220d\shot-ipad-chats-verified.png"
Write-Host "Fetching screenshot to $destPng..."
& scp @sshOpts "root@${ipad}:/var/mobile/Documents/wa6-shot.png" $destPng

Write-Host "Done! Screenshot saved."
