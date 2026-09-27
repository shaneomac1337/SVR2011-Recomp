$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Sync-VulkanRuntime.ps1"
$root = Join-Path ([IO.Path]::GetTempPath()) ([IO.Path]::GetRandomFileName())
$source = Join-Path $root 'source'
$destination = Join-Path $root 'destination'
[IO.Directory]::CreateDirectory($source) | Out-Null
[IO.Directory]::CreateDirectory($destination) | Out-Null
$names = @('rexruntimerd.dll','rexgpu-xenosrd.dll','TracyClientrd.dll')
try {
    foreach ($name in $names) {
        [IO.File]::WriteAllText((Join-Path $source $name), "new-$name")
        [IO.File]::WriteAllText((Join-Path $destination $name), "stale-$name")
    }
    Sync-VulkanRuntime $source $destination
    foreach ($name in $names) {
        if ((Get-FileHash (Join-Path $source $name)).Hash -ne (Get-FileHash (Join-Path $destination $name)).Hash) { throw 'Stale DLL survived runtime synchronization.' }
    }
    $time = (Get-Item (Join-Path $destination $names[0])).LastWriteTimeUtc
    Sync-VulkanRuntime $source $destination
    if ((Get-Item (Join-Path $destination $names[0])).LastWriteTimeUtc -ne $time) { throw 'Unchanged DLL was unnecessarily rewritten.' }
    Remove-Item -LiteralPath (Join-Path $source $names[1])
    $rejected = $false
    try { Sync-VulkanRuntime $source $destination } catch { $rejected = $true }
    if (!$rejected) { throw 'Incomplete runtime was accepted.' }
    Write-Output 'Runtime deployment checks passed.'
} finally {
    foreach ($directory in @($source,$destination)) {
        foreach ($name in $names) {
            $path = Join-Path $directory $name
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        }
        [IO.Directory]::Delete($directory)
    }
    [IO.Directory]::Delete($root)
}
