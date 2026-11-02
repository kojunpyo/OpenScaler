param([string]$Sha, [string]$Branch)
# 지정한 커밋의 GitHub Actions 실행이 끝날 때까지 기다린 뒤 결과를 출력한다.
$api = "https://api.github.com/repos/kojunpyo/OpenScaler/actions/runs?head_sha=$Sha&branch=$Branch"
for ($i = 0; $i -lt 60; $i++) {
  try { $r = Invoke-RestMethod $api -Headers @{ "User-Agent" = "OpenScaler-ci-check" } } catch { "API error: $_"; Start-Sleep 20; continue }
  $run = $r.workflow_runs | Select-Object -First 1
  if ($run -and $run.status -eq "completed") {
    "status=$($run.status) conclusion=$($run.conclusion) url=$($run.html_url)"
    $jobs = Invoke-RestMethod $run.jobs_url -Headers @{ "User-Agent" = "OpenScaler-ci-check" }
    foreach ($j in $jobs.jobs) { "job $($j.name): $($j.conclusion)"; foreach ($s in $j.steps) { "  $($s.name): $($s.conclusion)" } }
    exit 0
  }
  Start-Sleep 20
}
"timed out waiting for run"

