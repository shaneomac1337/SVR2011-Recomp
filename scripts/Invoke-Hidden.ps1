function Invoke-Hidden {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$WorkingDirectory = (Get-Location).Path,
        [string]$LogPath
    )
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $FilePath
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $info
    $stdoutFile = $null
    $stderrFile = $null
    try {
        $null = $process.Start()
        if ($LogPath) {
            $stdoutFile = [System.IO.FileStream]::new($LogPath, 'Create', 'Write', 'ReadWrite')
            $stderrFile = [System.IO.FileStream]::new("$LogPath.stderr", 'Create', 'Write', 'ReadWrite')
            $stdout = $process.StandardOutput.BaseStream.CopyToAsync($stdoutFile)
            $stderr = $process.StandardError.BaseStream.CopyToAsync($stderrFile)
        } else {
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
        }
        $process.WaitForExit()
        if ($LogPath) {
            $null = $stdout.GetAwaiter().GetResult()
            $null = $stderr.GetAwaiter().GetResult()
        } else {
            $stdout.GetAwaiter().GetResult()
            $stderr.GetAwaiter().GetResult()
        }
        if ($process.ExitCode -ne 0) {
            throw "$FilePath exited with code $($process.ExitCode)"
        }
    } finally {
        if ($stdoutFile) { $stdoutFile.Dispose() }
        if ($stderrFile) { $stderrFile.Dispose() }
        $process.Dispose()
    }
}
