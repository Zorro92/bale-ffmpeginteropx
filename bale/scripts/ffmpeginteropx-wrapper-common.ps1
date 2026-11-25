$ErrorActionPreference = 'Stop'

function Get-NativePin {
    $pin = Get-Content (Join-Path $PSScriptRoot '../ffmpeg-native.json') -Raw | ConvertFrom-Json
    # patch_revision describes the native package's reviewed FFmpeg patches, not wrapper patches.
    $patchRevision = "$($pin.patch_revision)"
    if ($pin.sha256 -notmatch '^[0-9a-f]{64}$' -or $pin.dav1d_version -notmatch '^\d+\.\d+\.\d+$' -or
        $pin.upstream_repository -notmatch '^[\w.-]+/[\w.-]+$' -or
        $pin.upstream_commit -notmatch '^[0-9a-f]{40}$' -or
        $patchRevision -cnotmatch '^(p[1-9][0-9]*)?$' -or
        "$($pin.version)" -cnotmatch '^\d+\.\d+(\.\d+)?-bale\.') {
        throw 'Invalid native FFmpeg package pin'
    }
    # The FFmpeg release (8.1.2, 9.1, ...) is the version's first part; version and tag must agree on it.
    $ffmpegVersion = $pin.version.Substring(0, $pin.version.IndexOf('-bale.'))
    if ($pin.version -ne "$ffmpegVersion-bale.dav1d.$($pin.dav1d_version)$(if ($patchRevision) { ".$patchRevision" })") {
        throw 'Invalid native FFmpeg package pin'
    }
    # NuGet normalizes a two-part release (9.1) to 9.1.0 in the package version; the tag keeps FFmpeg's name.
    $tagVersion = $ffmpegVersion -creplace '^(\d+\.\d+)\.0$', '$1'
    if ($pin.release_tag -ne "native-ffmpeg-$ffmpegVersion-dav1d-$($pin.dav1d_version)$(if ($patchRevision) { "-$patchRevision" })" -and
        $pin.release_tag -ne "native-ffmpeg-$tagVersion-dav1d-$($pin.dav1d_version)$(if ($patchRevision) { "-$patchRevision" })") {
        throw 'Inconsistent native release tag'
    }
    return $pin
}

function Get-WrapperPin {
    $pin = Get-Content (Join-Path $PSScriptRoot '../ffmpeginteropx-wrapper.json') -Raw | ConvertFrom-Json
    $nativePin = Get-NativePin
    if ($pin.upstream_repository -notmatch '^[\w.-]+/[\w.-]+$' -or
        $pin.upstream_base -notmatch '^[0-9a-f]{40}$' -or
        $pin.upstream_repository -ne $nativePin.upstream_repository -or
        $pin.original_package_id -ne 'FFmpegInteropX.UWP.Lib' -or
        $pin.original_package_version -ne '2.1.0' -or
        $pin.original_package_sha256 -notmatch '^[0-9a-f]{64}$' -or
        $pin.windows_sdk -notmatch '^10\.0\.\d+\.0$' -or
        $pin.windows_min_version -notmatch '^10\.0\.\d+\.0$' -or
        $pin.platform_toolset -notmatch '^v\d{3}$' -or $pin.library_version -ne '2.1.0.0') {
        throw 'Invalid or inconsistent FFmpegInteropX wrapper build pin'
    }
    return $pin
}

function Invoke-WrapperGit {
    param([string]$Directory, [string[]]$GitArguments)
    $result = & git -C $Directory @GitArguments
    if ($LASTEXITCODE -ne 0) { throw "git failed: $($GitArguments[0])" }
    return $result
}

function Assert-WrapperReleaseAvailable {
    param([string]$Repository, [string]$Tag)
    # Older immutable wrapper releases live in Bale; never reuse one of those versions here.
    foreach ($repo in (@($Repository, 'Zorro92/bale-for-silo') | Select-Object -Unique)) {
        & git ls-remote --exit-code --tags "https://github.com/$repo.git" "refs/tags/$Tag" | Out-Null
        if ($LASTEXITCODE -eq 0) { throw "Wrapper release tag already exists in $repo; do not replace a hash-pinned package" }
        if ($LASTEXITCODE -ne 2) { throw "Could not establish whether the wrapper release tag exists in $repo" }
    }
    $global:LASTEXITCODE = 0
}

function Assert-WrapperSource {
    param([string]$SourcePath, [string]$UpstreamBase)
    if (Invoke-WrapperGit $SourcePath @('status', '--porcelain', '--untracked-files=all')) {
        throw 'Wrapper source checkout must be clean before building'
    }
    & git -C $SourcePath merge-base --is-ancestor $UpstreamBase HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Wrapper upstream base must be an ancestor of HEAD' }
    # Disable rename detection so BOTH paths are checked, including deleted upstream files.
    $changes = @(Invoke-WrapperGit $SourcePath @('diff', '--name-only', '--no-renames', $UpstreamBase, 'HEAD', '--'))
    foreach ($file in $changes) {
        if ($file -cnotmatch '^Source/(?:[A-Za-z0-9_-]+/)*[A-Za-z0-9_.-]+\.(cpp|h)$' -and
            $file -cnotmatch '^bale/.+$' -and $file -cnotmatch '^\.github/workflows/bale-[^/]+\.yml$' -and
            $file -cnotmatch '^(AGENTS\.md|NOTICE|README\.md)$') {
            throw "Wrapper source guard forbids changes outside Source/**/*.cpp|h and fork-owned paths: $file"
        }
    }
}

function Compare-WrapperWinmd {
    param([string]$OriginalPath, [string]$BuiltPath)
    # Read raw WinRT metadata, not CLR projections or ReflectionOnlyLoad (unavailable in pwsh).
    if (-not ('Bale.WrapperMetadata' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
namespace Bale
{
    public static class WrapperMetadata
    {
        private static string Name(MetadataReader reader, TypeDefinitionHandle handle)
        {
            var type = reader.GetTypeDefinition(handle);
            var parent = type.GetDeclaringType();
            return parent.IsNil
                ? reader.GetString(type.Namespace) + "." + reader.GetString(type.Name)
                : Name(reader, parent) + "+" + reader.GetString(type.Name);
        }
        public static string[] PublicTypes(string path)
        {
            using var stream = File.OpenRead(path);
            using var pe = new PEReader(stream);
            var reader = pe.GetMetadataReader(MetadataReaderOptions.None);
            var names = new SortedSet<string>(StringComparer.Ordinal);
            foreach (var handle in reader.TypeDefinitions)
            {
                var type = reader.GetTypeDefinition(handle);
                var visibility = type.Attributes & TypeAttributes.VisibilityMask;
                if (visibility == TypeAttributes.Public || visibility == TypeAttributes.NestedPublic)
                    names.Add(Name(reader, handle));
            }
            if (names.Count == 0) throw new InvalidDataException("WinMD has no public types");
            var result = new string[names.Count];
            names.CopyTo(result);
            return result;
        }
    }
}
'@
    }
    $original = [Bale.WrapperMetadata]::PublicTypes($OriginalPath)
    $built = [Bale.WrapperMetadata]::PublicTypes($BuiltPath)
    $difference = @(Compare-Object $original $built -CaseSensitive)
    if ($difference.Count) {
        $details = ($difference | ForEach-Object { "$($_.SideIndicator) $($_.InputObject)" }) -join '; '
        throw "WinMD public type mismatch (<= original only; => built only): $details"
    }
    return "WinMD public types: identical ($($original.Count)); original package winmd retained."
}

function Export-WrapperZipEntry {
    param([System.IO.Compression.ZipArchive]$Zip, [string]$Name, [string]$Path)
    $entry = $Zip.GetEntry($Name)
    if (-not $entry) { throw "Original wrapper package is missing $Name" }
    $inputStream = $entry.Open()
    $outputStream = [System.IO.File]::Create($Path)
    try { $inputStream.CopyTo($outputStream) }
    finally { $outputStream.Dispose(); $inputStream.Dispose() }
}

function New-WrapperPackage {
    param(
        [string]$OriginalPackagePath,
        [ValidatePattern('^[0-9a-f]{64}$')][string]$OriginalSha256,
        [string]$NativeOutputPath,
        [ValidatePattern('^2\.1\.0-bale\.(0|[1-9]\d*)$')][string]$Version,
        [string]$SourcesPath,
        [string]$OutputPath
    )
    $actual = (Get-FileHash $OriginalPackagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $OriginalSha256) { throw "Original wrapper SHA256 mismatch: expected $OriginalSha256, got $actual" }
    foreach ($name in @('FFmpegInteropX.dll', 'FFmpegInteropX.pdb', 'FFmpegInteropX.winmd')) {
        if (-not (Test-Path (Join-Path $NativeOutputPath $name) -PathType Leaf)) { throw "Missing wrapper output: $name" }
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $staging = Join-Path ([System.IO.Path]::GetTempPath()) "bale-wrapper-repack-$([guid]::NewGuid())"
    New-Item -ItemType Directory $staging | Out-Null
    $original = [System.IO.Compression.ZipFile]::OpenRead($OriginalPackagePath)
    try {
        $winmd = Join-Path $staging 'original.winmd'
        Export-WrapperZipEntry $original 'lib/uap10/FFmpegInteropX.winmd' $winmd
        $comparison = Compare-WrapperWinmd $winmd (Join-Path $NativeOutputPath 'FFmpegInteropX.winmd')
        New-Item -ItemType Directory -Force $OutputPath | Out-Null
        $comparison | Set-Content (Join-Path $OutputPath 'winmd-public-types.txt') -Encoding utf8
        # Require the original managed projection, targets and all three architecture payloads.
        foreach ($name in @('lib/net6.0-windows10.0.17763.0/FFmpegInteropX.DotNet.dll',
            'build/native/FFmpegInteropX.UWP.Lib.targets', 'runtimes/win-x86/native/FFmpegInteropX.dll',
            'runtimes/win-x64/native/FFmpegInteropX.dll', 'runtimes/win-arm64/native/FFmpegInteropX.dll')) {
            if (-not $original.GetEntry($name)) { throw "Original wrapper package is missing $name" }
        }
        $specPath = Join-Path $staging 'FFmpegInteropX.UWP.Lib.nuspec'
        Export-WrapperZipEntry $original 'FFmpegInteropX.UWP.Lib.nuspec' $specPath
        [xml]$spec = Get-Content $specPath -Raw
        if ($spec.package.metadata.id -ne 'FFmpegInteropX.UWP.Lib' -or $spec.package.metadata.version -ne '2.1.0') {
            throw 'Unexpected original wrapper package identity'
        }
        $spec.package.metadata.version = $Version
        $spec.Save($specPath)
        $contentPath = Join-Path $staging 'content-types.xml'
        Export-WrapperZipEntry $original '[Content_Types].xml' $contentPath
        [xml]$contentTypes = Get-Content $contentPath -Raw
        foreach ($extension in @('pdb', 'txt')) {
            if ($extension -notin @($contentTypes.Types.Default.Extension)) {
                $node = $contentTypes.CreateElement('Default', $contentTypes.DocumentElement.NamespaceURI)
                $node.SetAttribute('Extension', $extension)
                $node.SetAttribute('ContentType', 'application/octet')
                [void]$contentTypes.DocumentElement.AppendChild($node)
            }
        }
        $contentTypes.Save($contentPath)
        $replacements = @{
            'FFmpegInteropX.UWP.Lib.nuspec' = $specPath
            '[Content_Types].xml' = $contentPath
            'runtimes/win-x64/native/FFmpegInteropX.dll' = Join-Path $NativeOutputPath 'FFmpegInteropX.dll'
            'runtimes/win-x64/native/FFmpegInteropX.pdb' = Join-Path $NativeOutputPath 'FFmpegInteropX.pdb'
            'SOURCES.txt' = $SourcesPath
        }
        foreach ($entry in $original.Entries) {
            if ($entry.FullName -like 'package/services/metadata/core-properties/*.psmdcp') {
                $propertiesPath = Join-Path $staging 'core-properties.xml'
                Export-WrapperZipEntry $original $entry.FullName $propertiesPath
                [xml]$properties = Get-Content $propertiesPath -Raw
                $properties.coreProperties.version = $Version
                $properties.Save($propertiesPath)
                $replacements[$entry.FullName] = $propertiesPath
            }
        }
        $package = Join-Path $OutputPath "FFmpegInteropX.UWP.Lib.$Version.nupkg"
        $zip = [System.IO.Compression.ZipFile]::Open($package, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($entry in $original.Entries) {
                # Any modification invalidates NuGet's repository signature; never ship a stale one.
                if ($entry.FullName -eq '.signature.p7s' -or $replacements.ContainsKey($entry.FullName)) { continue }
                $target = $zip.CreateEntry($entry.FullName)
                $target.LastWriteTime = $entry.LastWriteTime
                $inputStream = $entry.Open()
                $outputStream = $target.Open()
                try { $inputStream.CopyTo($outputStream) }
                finally { $outputStream.Dispose(); $inputStream.Dispose() }
            }
            foreach ($name in ($replacements.Keys | Sort-Object)) {
                $target = $zip.CreateEntry($name)
                $target.LastWriteTime = [DateTimeOffset]'2000-01-01T00:00:00Z'
                $inputStream = [System.IO.File]::OpenRead($replacements[$name])
                $outputStream = $target.Open()
                try { $inputStream.CopyTo($outputStream) }
                finally { $outputStream.Dispose(); $inputStream.Dispose() }
            }
        }
        finally { $zip.Dispose() }
        $hash = (Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $([System.IO.Path]::GetFileName($package))" | Set-Content "$package.sha256" -Encoding ascii
        Write-Host $comparison
        return $hash
    }
    finally { $original.Dispose(); Remove-Item $staging -Recurse -Force }
}
