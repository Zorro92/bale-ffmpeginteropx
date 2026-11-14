param([switch]$NativeOnly)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/ffmpeginteropx-wrapper-common.ps1"
# This fork restores only native FFmpeg; it never consumes a prior wrapper release.
$nativePin = Get-NativePin
$packages = @([pscustomobject]@{ Id = 'FFmpegInteropX.UWP.FFmpeg'; Pin = $nativePin })
$feed = Join-Path $PSScriptRoot '../.cache/ffmpeg-native'
$cache = if ($env:NUGET_PACKAGES) { $env:NUGET_PACKAGES } else {
    Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages'
}
foreach ($package in $packages) {
    $pin = $package.Pin
    $asset = "$($package.Id).$($pin.version).nupkg"
    $staging = Join-Path ([System.IO.Path]::GetTempPath()) "bale-native-$([guid]::NewGuid())"
    New-Item -ItemType Directory $staging | Out-Null
    try {
        $download = Join-Path $staging $asset
        Invoke-WebRequest -Uri "https://github.com/Zorro92/bale-for-silo/releases/download/$($pin.release_tag)/$asset" -OutFile $download
        $actual = (Get-FileHash $download -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $pin.sha256) { throw "$($package.Id) SHA256 mismatch: expected $($pin.sha256), got $actual" }
        New-Item -ItemType Directory -Force $feed | Out-Null
        Move-Item $download (Join-Path $feed $asset) -Force
        # Force restore from the verified feed, not a stale/tampered extracted cache.
        $cached = Join-Path $cache "$($package.Id.ToLowerInvariant())/$($pin.version)"
        if (Test-Path $cached) { Remove-Item $cached -Recurse -Force }
        Write-Output "Verified $($package.Id) $($pin.version): SHA256 $actual"
    }
    finally { Remove-Item $staging -Recurse -Force }
}
