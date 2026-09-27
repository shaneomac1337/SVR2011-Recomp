param([ValidateRange(1, 300)][int]$Seconds = 20)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$executable = "$projectRoot/out/build/win-amd64-relwithdebinfo/svr2011.exe"
$run = Join-Path $projectRoot ("analysis/runtime-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $run | Out-Null
$info = [System.Diagnostics.ProcessStartInfo]::new()
$info.FileName = $executable
$info.WorkingDirectory = Split-Path $executable -Parent
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
foreach ($argument in @('--headless', '--gpu_plugin=xenos', "--game_data_root=$projectRoot/assets",
    "--user_data_root=$projectRoot/userdata", "--cache_root=$projectRoot/cache",
    "--log_file=$run/runtime.log", '--log_level=debug', '--log_flush_interval=1')) {
    $info.ArgumentList.Add($argument)
}
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $info
try {
    $null = $process.Start()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $timedOut = !$process.WaitForExit($Seconds * 1000)
    if ($timedOut) { $process.Kill($true); $process.WaitForExit() }
    $stdout.GetAwaiter().GetResult() | Set-Content "$run/stdout.txt"
    $stderr.GetAwaiter().GetResult() | Set-Content "$run/stderr.txt"
    $result = [ordered]@{ executable = $executable; seconds = $Seconds; timed_out = $timedOut; exit_code = $process.ExitCode; logs = $run }
    $result | ConvertTo-Json | Set-Content "$run/result.json"
    $result | ConvertTo-Json
} finally {
    if (!$process.HasExited) { $process.Kill($true); $process.WaitForExit() }
    $process.Dispose()
}
