param([string]$Workflow = (Join-Path $PSScriptRoot '../.github/workflows/manual-build.yml'))
$ErrorActionPreference = 'Stop'

# Exercise the workflow's own scripts with real ZIP files and a mocked GitHub CLI.
# No network calls or remote publication are performed.
$text = Get-Content -LiteralPath $Workflow -Raw -Encoding utf8
$blocks = @([regex]::Matches($text, '(?m)^        run: \|\r?\n((?:          .*\r?\n|\r?\n)*)') |
    ForEach-Object { $_.Groups[1].Value -replace '(?m)^          ', '' })
foreach ($block in $blocks) {
    $tokens = $null; $errors = $null
    [void][Management.Automation.Language.Parser]::ParseInput($block, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "Invalid workflow PowerShell: $errors" }
}
function Get-Step([string]$Marker) {
    $matches = @($blocks | Where-Object { $_.Contains($Marker) })
    if ($matches.Count -ne 1) { throw "Missing or ambiguous workflow step: $Marker" }
    return $matches[0]
}
$validation = Get-Step '$releaseDate ='
$packaging = [scriptblock]::Create((Get-Step './tools/package.ps1'))
$preparation = [scriptblock]::Create((Get-Step 'function Read-VersionTag'))
$publication = [scriptblock]::Create((Get-Step 'gh release create'))
$notes = [regex]::Match($text, '(?m)^          RELEASE_INSTALL_NOTES: \|\r?\n((?:            .*\r?\n|\r?\n)*)')
if (-not $notes.Success -or -not $text.Contains("&& 'manual-i18n-release'") -or
    -not $text.Contains('needs.build.outputs.release_date')) {
    throw 'Release notes, shared publication concurrency or captured build date are missing.'
}
foreach ($job in @('prepare_release', 'github_release')) {
    $jobText = [regex]::Match($text, "(?ms)^  ${job}:\r?\n(.*?)(?=^  [a-z_]+:|\z)").Groups[1].Value
    if (-not $jobText.Contains('if: inputs.release_a_version')) { throw "Ungated release job: $job" }
    $dependency = if ($job -eq 'prepare_release') { 'build' } else { 'prepare_release' }
    if (-not $jobText.Contains("needs: $dependency")) { throw "Wrong dependency for $job" }
    if (($jobText -match 'contents: write') -ne ($job -eq 'github_release')) {
        throw 'Only the publication job should have repository write permission.'
    }
}
$savedEnvironment = @{}
foreach ($name in @('BUILD_CONFIGURATION', 'PUBLISH_RELEASE', 'PACKAGE_VERSION', 'RELEASE_DATE',
    'RELEASE_VERSION', 'RELEASE_INSTALL_NOTES', 'GITHUB_SHA', 'GITHUB_REF', 'GITHUB_REPOSITORY',
    'GITHUB_SERVER_URL', 'GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_OUTPUT')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$savedExitCode = $global:LASTEXITCODE
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
$fixture = Join-Path $temporaryRoot ('TtpI18nRelease-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
$packageScript = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../tools/package.ps1')).Path
Push-Location $fixture
try {
    $dateCases = @(
        @('2026-09-04T15:59:59Z', 'Release', 'true', '2026.09.04'),
        @('2026-09-04T16:00:00Z', 'Release', 'true', '2026.09.05'),
        @('2026-12-31T16:00:00Z', 'Release', 'true', '2027.01.01'),
        @('2024-02-28T16:00:00Z', 'Release', 'true', '2024.02.29'),
        @('2026-09-04T16:00:00Z', 'Debug', 'true', ''),
        @('2026-09-04T16:00:00Z', 'RelWithDebInfo', 'true', ''),
        @('2026-09-04T16:00:00Z', 'Debug', 'false', '2026.09.05'),
        @('2026-09-04T16:00:00Z', 'RelWithDebInfo', 'false', '2026.09.05'))
    foreach ($case in $dateCases) {
        $env:GITHUB_OUTPUT = Join-Path $fixture ('date-' + [guid]::NewGuid().ToString('N'))
        $env:BUILD_CONFIGURATION = $case[1]
        $env:PUBLISH_RELEASE = $case[2]
        $code = $validation.Replace('[DateTimeOffset]::UtcNow', "([DateTimeOffset]'$($case[0])')")
        $failure = $null
        try { & ([scriptblock]::Create($code)) } catch { $failure = $_ }
        if ($case[3]) {
            if ($failure -or (Get-Content -LiteralPath $env:GITHUB_OUTPUT -Raw -Encoding utf8).Trim() -cne "value=$($case[3])") {
                throw "Incorrect Beijing date: $case / $failure"
            }
        } elseif (-not $failure -or $failure.ToString() -notlike '*requires the Release*') {
            throw "Non-Release publication was not rejected correctly: $case / $failure"
        }
    }
    $env:GITHUB_SHA = '0123456789012345678901234567890123456789'
    $env:GITHUB_REF = 'refs/heads/main'
    $env:GITHUB_REPOSITORY = 'fixture/TTPlayerI18n'
    $env:GITHUB_SERVER_URL = 'https://github.com'
    $env:GITHUB_RUN_ID = '123'
    $env:GITHUB_RUN_ATTEMPT = '2'
    $env:PACKAGE_VERSION = '2026.09.05'
    $env:RELEASE_INSTALL_NOTES = $notes.Groups[1].Value -replace '(?m)^            ', ''
    New-Item -ItemType Directory -Path tools | Out-Null
    Copy-Item -LiteralPath $packageScript -Destination tools/package.ps1
    $expectedEntries = @('AddIn/ttp_i18n.dll', 'i18n/chs/ttplayer.po', 'i18n/cht/ttplayer.po',
        'i18n/en_US/ttplayer.mo', 'i18n/en_US/ttplayer.po', 'i18n/README.md', 'i18n/ttplayer.pot', 'SHA256SUMS.txt') | Sort-Object
    foreach ($configuration in @('Release', 'RelWithDebInfo', 'Debug')) {
        $env:BUILD_CONFIGURATION = $configuration
        $output = "build/$configuration"
        New-Item -ItemType Directory -Path "$output/AddIn", "$output/licenses" -Force | Out-Null
        'DLL fixture' | Set-Content -LiteralPath "$output/AddIn/ttp_i18n.dll"
        'Unpackaged PDB' | Set-Content -LiteralPath "$output/ttp_i18n.pdb"
        'Unpackaged license' | Set-Content -LiteralPath "$output/licenses/license.txt"
        foreach ($locale in @('chs', 'cht', 'en_US')) {
            New-Item -ItemType Directory -Path "$output/i18n/$locale/LC_MESSAGES" -Force | Out-Null
            'PO fixture' | Set-Content -LiteralPath "$output/i18n/$locale/ttplayer.po"
            'Stale catalog' | Set-Content -LiteralPath "$output/i18n/$locale/LC_MESSAGES/ttplayer.po"
        }
        'MO fixture' | Set-Content -LiteralPath "$output/i18n/en_US/ttplayer.mo"
        'Catalog readme' | Set-Content -LiteralPath "$output/i18n/README.md"
        'POT fixture' | Set-Content -LiteralPath "$output/i18n/ttplayer.pot"
        $dllHash = (Get-FileHash -LiteralPath "$output/AddIn/ttp_i18n.dll" -Algorithm SHA256).Hash.ToLowerInvariant()
        @{sha256=$dllHash;architecture='x86';minimum_subsystem='5.01';executable='ttp_i18n.dll';
          inventories=@('5.1.2600.txt','6.1.7600.txt')} | ConvertTo-Json |
            Set-Content -LiteralPath "$output/i18n-legacy-imports.json" -Encoding utf8
        & $packaging | Out-Null
        $archive = 'artifact/ttp_i18n-x86-2026.09.05.zip'
        $expanded = Join-Path $fixture "expanded-$configuration"
        Expand-Archive -LiteralPath $archive -DestinationPath $expanded
        $entries = @(Get-ChildItem -LiteralPath $expanded -File -Recurse | ForEach-Object {
            $_.FullName.Substring($expanded.Length + 1).Replace('\', '/')
        } | Sort-Object)
        if (($entries -join ',') -cne ($expectedEntries -join ',')) { throw "Unexpected ZIP entries: $entries" }
        $innerHashes = @(Get-Content -LiteralPath "$expanded/SHA256SUMS.txt" -Encoding utf8)
        if ($innerHashes.Count -ne $expectedEntries.Count - 1) { throw 'Incorrect inner checksum count.' }
        foreach ($entry in $entries | Where-Object { $_ -ne 'SHA256SUMS.txt' }) {
            $hash = (Get-FileHash -LiteralPath (Join-Path $expanded $entry) -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($innerHashes -cnotcontains "$hash  $entry") { throw "Incorrect inner checksum: $entry" }
        }
        $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
        if ((Get-Content -LiteralPath artifact/SHA256SUMS.txt -Raw -Encoding utf8).Trim() -cne
            "$hash  ttp_i18n-x86-2026.09.05.zip") { throw 'Incorrect outer checksum.' }
        $info = Get-Content -LiteralPath artifact/build-info.json -Raw -Encoding utf8 | ConvertFrom-Json
        if ($info.commit -cne $env:GITHUB_SHA -or $info.configuration -cne $configuration -or
            $info.package_version -cne $env:PACKAGE_VERSION -or $info.architecture -cne 'x86') {
            throw 'Incorrect build provenance.'
        }
    }
    & $packageScript -BuildDirectory build -Configuration Release -Destination local | Out-Null
    if (-not (Test-Path -LiteralPath local/ttp_i18n-x86-Release.zip)) { throw 'Local package name regressed.' }
    foreach ($badVersion in @('2026.02.30', '2026.9.5', '../other', '2026.09.05p1', '')) {
        $failure = $null
        try { & $packageScript -BuildDirectory build -Destination invalid -PackageVersion $badVersion | Out-Null }
        catch { $failure = $_ }
        if (-not $failure -or $failure.ToString() -notlike '*PackageVersion must be*') {
            throw "Invalid package date was not rejected: $badVersion / $failure"
        }
    }
    $env:BUILD_CONFIGURATION = 'Release'
    & $packaging | Out-Null
    $originalArchive = Join-Path $fixture 'artifact/ttp_i18n-x86-2026.09.05.zip'
    $originalHash = (Get-FileHash -LiteralPath $originalArchive -Algorithm SHA256).Hash.ToLowerInvariant()
    $scenarios = @('first', 'previous', 'same-day', 'numeric-patch', 'gaps', 'patch-only', 'draft',
        'many-tags', 'invalid-tags', 'wrong-commit', 'wrong-configuration', 'wrong-architecture',
        'wrong-date', 'wrong-hash', 'wrong-checksum-name', 'tag-api-error', 'release-api-error',
        'collision', 'invalid-date', 'exhausted', 'overflow', 'prepared-commit', 'prepared-version',
        'prepared-configuration', 'prepared-hash')
    foreach ($scenario in $scenarios) {
        & {
            $caseRoot = Join-Path $fixture $scenario
            New-Item -ItemType Directory -Path "$caseRoot/artifact" -Force | Out-Null
            Copy-Item -LiteralPath $originalArchive, (Join-Path $fixture 'artifact/build-info.json'),
                (Join-Path $fixture 'artifact/SHA256SUMS.txt') -Destination "$caseRoot/artifact"
            Push-Location $caseRoot
            try {
                $env:RELEASE_DATE = '2026.09.05'
                $env:RELEASE_VERSION = ''
                $env:GITHUB_OUTPUT = Join-Path $caseRoot 'outputs.txt'
                $tagNames = @('2026.01.03', '2026.09.04p2'); $releaseNames = @()
                $expectedVersion = '2026.09.05'; $previous = '2026.09.04p2'; $expectedError = ''
                $observed = @{creates=0;apis=0;arguments=@()}
                $info = Get-Content -LiteralPath artifact/build-info.json -Raw -Encoding utf8 | ConvertFrom-Json
                switch ($scenario) {
                    'first' { $tagNames = @(); $previous = '' }
                    'same-day' { $tagNames += '2026.09.05'; $expectedVersion += 'p1'; $previous = '2026.09.05' }
                    'numeric-patch' { $tagNames += @('2026.09.05p2','2026.09.05p10','2026.09.05'); $expectedVersion += 'p11'; $previous = '2026.09.05p10' }
                    'gaps' { $tagNames += @('2026.09.05p3','2026.09.05p1'); $expectedVersion += 'p4'; $previous = '2026.09.05p3' }
                    'patch-only' { $tagNames += '2026.09.05p1'; $expectedVersion += 'p2'; $previous = '2026.09.05p1' }
                    'draft' { $releaseNames = @('2026.09.05','2026.09.05p2'); $expectedVersion += 'p3' }
                    'many-tags' { $tagNames = @(1..150 | ForEach-Object { "other-$_" }) + '2026.09.05'; $expectedVersion += 'p1'; $previous = '2026.09.05' }
                    'invalid-tags' { $tagNames += @('2099.02.30','2099.99.99','2026.09.05-beta','2026.09.05p0','v2026.09.05') }
                    'wrong-commit' { $info.commit = 'other'; $expectedError = '*does not match this Release build*' }
                    'wrong-configuration' { $info.configuration = 'Debug'; $expectedError = '*does not match this Release build*' }
                    'wrong-architecture' { $info.architecture = 'x64'; $expectedError = '*does not match this Release build*' }
                    'wrong-date' { $info.package_version = '2026.09.04'; $expectedError = '*does not match this Release build*' }
                    'wrong-hash' { 'changed ZIP' | Add-Content -LiteralPath artifact/ttp_i18n-x86-2026.09.05.zip; $expectedError = '*package SHA-256 verification failed*' }
                    'wrong-checksum-name' { "$originalHash  wrong.zip" | Set-Content -LiteralPath artifact/SHA256SUMS.txt; $expectedError = '*package SHA-256 verification failed*' }
                    'tag-api-error' { $expectedError = '*Could not read repository tags*' }
                    'release-api-error' { $expectedError = '*Could not read existing releases*' }
                    'collision' { $expectedError = '*GitHub publication failed*' }
                    'invalid-date' { $env:RELEASE_DATE = '2026.02.30'; $info.package_version = $env:RELEASE_DATE; $expectedError = '*invalid Beijing build date*' }
                    'exhausted' { $tagNames += '2026.09.05p9223372036854775807'; $expectedError = '*patch number is exhausted*' }
                    'overflow' { $tagNames += '2026.09.05p9223372036854775808'; $expectedError = '*patch number is too large*' }
                    'prepared-commit' { $expectedError = '*Prepared release does not match*' }
                    'prepared-version' { $expectedError = '*Prepared release does not match*' }
                    'prepared-configuration' { $expectedError = '*Prepared release does not match*' }
                    'prepared-hash' { $expectedError = '*Prepared release SHA-256 verification failed*' }
                }
                $info | ConvertTo-Json | Set-Content -LiteralPath artifact/build-info.json -Encoding utf8
                function gh {
                    $global:LASTEXITCODE = 0
                    if ($args[0] -eq 'api') {
                        ++$observed.apis
                        if ($args -notcontains '--paginate') { throw 'Must paginate repository history.' }
                        if ($args[1] -eq "repos/$env:GITHUB_REPOSITORY/tags?per_page=100") {
                            if ($scenario -eq 'tag-api-error') { $global:LASTEXITCODE = 1; return }
                            $tagNames
                        } elseif ($args[1] -eq "repos/$env:GITHUB_REPOSITORY/releases?per_page=100") {
                            if ($scenario -eq 'release-api-error') { $global:LASTEXITCODE = 1; return }
                            $releaseNames
                        } else { throw "Unexpected API request: $args" }
                    } elseif ($args[0] -eq 'release' -and $args[1] -eq 'create') {
                        ++$observed.creates; $observed.arguments = @($args)
                        if ($scenario -eq 'collision') { $global:LASTEXITCODE = 1 }
                    } else { throw "Unexpected GitHub operation: $args" }
                }
                $failure = $null
                try {
                    & $preparation | Out-Null
                    $outputs = Get-Content -LiteralPath $env:GITHUB_OUTPUT -Encoding utf8
                    $env:RELEASE_VERSION = ($outputs | Where-Object { $_ -like 'version=*' }).Substring(8)
                    if ($outputs -cnotcontains 'artifact_name=ttp_i18n-release-123-2') { throw 'Incorrect prepared artifact name.' }
                    if ($scenario.StartsWith('prepared-')) {
                        $release = Get-Content -LiteralPath artifact/release-info.json -Raw -Encoding utf8 | ConvertFrom-Json
                        switch ($scenario) {
                            'prepared-commit' { $release.commit = 'other' }
                            'prepared-version' { $release.version = '2026.09.05p99' }
                            'prepared-configuration' { $release.configuration = 'Debug' }
                            'prepared-hash' { 'changed ZIP' | Add-Content -LiteralPath "artifact/ttp_i18n-x86-$expectedVersion.zip" }
                        }
                        $release | ConvertTo-Json | Set-Content -LiteralPath artifact/release-info.json -Encoding utf8
                    }
                    & $publication | Out-Null
                } catch { $failure = $_ }
                if ($expectedError) {
                    if (-not $failure -or $failure.ToString() -notlike $expectedError) {
                        throw "Wrong rejection for ${scenario}: $failure"
                    }
                } elseif ($failure) { throw "Unexpected failure for ${scenario}: $failure" }
                $expectedCreates = [int](-not $expectedError -or $scenario -eq 'collision')
                if ($observed.creates -ne $expectedCreates) { throw "Wrong publication count: $scenario" }
                if ($expectedCreates) {
                    $archiveName = "ttp_i18n-x86-$expectedVersion.zip"
                    $expectedArguments = @('release','create',$expectedVersion,"artifact/$archiveName",'artifact/SHA256SUMS.txt',
                        '--repo',$env:GITHUB_REPOSITORY,'--target',$env:GITHUB_SHA,'--title',$expectedVersion,'--notes-file','artifact/release-notes.md')
                    if (($observed.arguments -join "`0") -cne ($expectedArguments -join "`0")) { throw "Incorrect publication arguments: $scenario" }
                    if ((Get-FileHash -LiteralPath "artifact/$archiveName" -Algorithm SHA256).Hash.ToLowerInvariant() -cne $originalHash -or
                        (Get-Content -LiteralPath artifact/SHA256SUMS.txt -Raw -Encoding utf8).Trim() -cne "$originalHash  $archiveName") {
                        throw "ZIP bytes or outer manifest changed incorrectly: $scenario"
                    }
                    $logUrl = if ($previous) { "https://github.com/fixture/TTPlayerI18n/compare/$previous...$expectedVersion" }
                              else { "https://github.com/fixture/TTPlayerI18n/commits/$expectedVersion" }
                    $actualNotes = Get-Content -LiteralPath artifact/release-notes.md -Raw -Encoding utf8
                    if (-not $actualNotes.Contains($logUrl) -or
                        -not $actualNotes.Contains($env:RELEASE_INSTALL_NOTES.Replace('{version}', $expectedVersion)) -or
                        $actualNotes.Contains('{version}')) { throw "Incorrect release notes: $scenario" }
                    if ($expectedVersion -ne '2026.09.05' -and (Test-Path -LiteralPath artifact/ttp_i18n-x86-2026.09.05.zip)) {
                        throw 'Patch release retained the unversioned archive name.'
                    }
                }
            } finally { Pop-Location }
        }
    }
    Write-Output "$($dateCases.Count) date/configuration cases, 3 real ZIP packages, local naming, invalid dates and $($scenarios.Count) mocked release cases passed. No remote writes."
} finally {
    Pop-Location
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process') }
    $global:LASTEXITCODE = $savedExitCode
    $cleanupPath = [IO.Path]::GetFullPath($fixture)
    if ([IO.Path]::GetDirectoryName($cleanupPath) -cne $temporaryRoot -or
        [IO.Path]::GetFileName($cleanupPath) -notmatch '^TtpI18nRelease-[a-f0-9]{32}$') {
        throw 'Refusing to remove an unexpected test directory.'
    }
    Remove-Item -LiteralPath $cleanupPath -Recurse -Force
}
