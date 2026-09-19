param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Configuration = 'Release',
    [Parameter(Mandatory)][string]$Destination,
    [string]$SourceRevision = ''
)
$ErrorActionPreference = 'Stop'
$source = Split-Path $PSScriptRoot -Parent
$output = Join-Path $BuildDirectory $Configuration
$dll = Join-Path $output 'AddIn/ttp_i18n.dll'
$report = Join-Path $output 'i18n-legacy-imports.json'
foreach ($file in @($dll, $report)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Build and audit ttp_i18n.dll before packaging: missing $file"
    }
}
$audit = Get-Content -LiteralPath $report -Raw -Encoding UTF8 | ConvertFrom-Json
$hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
if ($audit.sha256 -cne $hash -or $audit.architecture -ne 'x86' -or
    $audit.minimum_subsystem -ne '5.01' -or $audit.executable -ne 'ttp_i18n.dll' -or
    $audit.inventories -notcontains '5.1.2600.txt' -or $audit.inventories -notcontains '6.1.7600.txt') {
    throw 'The DLL does not have a matching XP / Win7 import audit. Rebuild before packaging.'
}
foreach ($license in @('YY-Thunks-LICENSE.txt', 'VC-LTL-LICENSE.txt', 'legacy-third-party.md')) {
    if (-not (Test-Path -LiteralPath (Join-Path $output "licenses/$license") -PathType Leaf)) {
        throw "Missing dependency license: $license"
    }
}
$translations = Join-Path $output 'i18n'
foreach ($language in @('chs', 'cht', 'en_US')) {
    $catalog = Join-Path $translations "$language/ttplayer.po"
    if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) { throw "Missing catalog: $catalog" }
}

# Use fresh staging so a previous build's files cannot leak into the ZIP.
$package = Join-Path $BuildDirectory ('i18n-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $package | Out-Null
New-Item -ItemType Directory -Path (Join-Path $package 'AddIn') | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $package 'AddIn')
Copy-Item -LiteralPath $report -Destination $package
Copy-Item -LiteralPath (Join-Path $output 'licenses') -Destination $package -Recurse
$packageTranslations = Join-Path $package 'i18n'
New-Item -ItemType Directory -Path $packageTranslations | Out-Null
Get-ChildItem -LiteralPath $translations -File | Copy-Item -Destination $packageTranslations
# Package only the active flat catalog layout, including after an incremental build.
foreach ($locale in Get-ChildItem -LiteralPath $translations -Directory) {
    $catalogs = @(Get-ChildItem -LiteralPath $locale.FullName -File |
        Where-Object { $_.Name -in @('ttplayer.po', 'ttplayer.mo') })
    if ($catalogs.Count -eq 0) { continue }
    $localeDestination = Join-Path $packageTranslations $locale.Name
    New-Item -ItemType Directory -Path $localeDestination | Out-Null
    $catalogs | Copy-Item -Destination $localeDestination
}
Copy-Item -LiteralPath (Join-Path $source 'LICENSE'), (Join-Path $source 'README.md') -Destination $package
New-Item -ItemType Directory -Path (Join-Path $package 'docs') | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'docs/I18N.md') -Destination (Join-Path $package 'docs')
[ordered]@{
    commit = $SourceRevision
    configuration = $Configuration
    architecture = 'x86'
    minimum_windows = 'XP SP3 / Windows 7'
    shared_by = @('modern', 'legacy')
    dll_sha256 = $hash
    yy_thunks = '1.2.2'
    vc_ltl = '5.3.1'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $package 'build-info.json') -Encoding utf8

$packageRoot = (Resolve-Path -LiteralPath $package).Path
$checksums = Get-ChildItem -LiteralPath $packageRoot -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative = $_.FullName.Substring($packageRoot.Length + 1).Replace('\', '/')
    $fileHash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$fileHash  $relative"
}
$checksums | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding utf8
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
$archive = Join-Path $Destination "ttp_i18n-x86-$Configuration.zip"
Compress-Archive -Path (Join-Path $package '*') -DestinationPath $archive -Force
$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$archiveHash  $([IO.Path]::GetFileName($archive))" |
    Set-Content -LiteralPath (Join-Path $Destination 'SHA256SUMS.txt') -Encoding utf8
$symbols = Join-Path $output 'ttp_i18n.pdb'
if (Test-Path -LiteralPath $symbols -PathType Leaf) {
    Copy-Item -LiteralPath $symbols -Destination $Destination -Force
}
Write-Output "Shared i18n package: $archive"
