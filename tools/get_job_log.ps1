param([string]$RunId = "39026255771")
$cfg = Get-Content (Join-Path $PSScriptRoot 'local.json') -Raw | ConvertFrom-Json
$headers = @{
    Authorization = "Bearer $($cfg.token)"
    Accept = 'application/vnd.github+json'
    'User-Agent' = 'job-checker'
}
$jobs = Invoke-RestMethod -Uri "https://api.github.com/repos/$($cfg.repo)/actions/runs/$RunId/jobs" -Headers $headers
foreach ($j in $jobs.jobs) {
    Write-Host "Job: $($j.name), conclusion: $($j.conclusion)"
    foreach ($s in $j.steps) {
        Write-Host "  Step: $($s.name), conclusion: $($s.conclusion)"
    }
    if ($j.conclusion -eq 'failure') {
        # Fetch log
        try {
            $log = Invoke-RestMethod -Uri "https://api.github.com/repos/$($cfg.repo)/actions/jobs/$($j.id)/logs" -Headers $headers
            Write-Host "=== LOG START ==="
            $lines = $log -split "`n"
            $lines | Select-Object -Last 100 | ForEach-Object { Write-Host $_ }
            Write-Host "=== LOG END ==="
        } catch {
            Write-Warning "Could not fetch log directly: $_"
        }
    }
}
