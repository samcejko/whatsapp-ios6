param([string]$CommitSha)
$cfg = Get-Content (Join-Path $PSScriptRoot 'local.json') -Raw | ConvertFrom-Json
$headers = @{
    Authorization = "Bearer $($cfg.token)"
    Accept = 'application/vnd.github+json'
    'User-Agent' = 'ci-checker'
}
$runs = Invoke-RestMethod -Uri "https://api.github.com/repos/$($cfg.repo)/actions/runs?per_page=5" -Headers $headers
$runs.workflow_runs | Select-Object id, head_sha, status, conclusion, created_at | Format-Table -AutoSize
