param([string]$CommitSha = '0dc4a16')
$cfg = Get-Content (Join-Path $PSScriptRoot 'local.json') -Raw | ConvertFrom-Json
$headers = @{
    Authorization = "Bearer $($cfg.token)"
    Accept = 'application/vnd.github+json'
    'User-Agent' = 'ci-waiter'
}

Write-Host "Waiting for CI build for commit $CommitSha to complete..."
while ($true) {
    Start-Sleep -Seconds 15
    $runs = Invoke-RestMethod -Uri "https://api.github.com/repos/$($cfg.repo)/actions/runs?per_page=5" -Headers $headers
    $targetRuns = @($runs.workflow_runs | Where-Object { $_.head_sha -like "$CommitSha*" })
    if ($targetRuns.Count -eq 0) {
        Write-Host "No runs found yet..."
        continue
    }
    $allDone = $true
    $anyFailed = $false
    foreach ($r in $targetRuns) {
        Write-Host "Run $($r.id): status=$($r.status), conclusion=$($r.conclusion)"
        if ($r.status -ne 'completed') {
            $allDone = $false
        } elseif ($r.conclusion -ne 'success') {
            $anyFailed = $true
        }
    }
    if ($allDone) {
        if ($anyFailed) {
            throw "CI build failed!"
        }
        Write-Host "CI builds succeeded!"
        break
    }
}
