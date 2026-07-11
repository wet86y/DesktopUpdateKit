function Assert-ReleaseConfig {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Config
    )

    foreach ($propertyName in @("applicationId", "repository", "publishedExeName", "releaseAssetName", "projectFile", "artifactsDirectory", "runtimeIdentifier")) {
        if ($null -eq $Config.PSObject.Properties[$propertyName] -or [string]::IsNullOrWhiteSpace([string]$Config.$propertyName)) {
            throw "release.config.json is missing required property: $propertyName"
        }
    }

    if ($null -eq $Config.PSObject.Properties["downloadNodes"]) {
        throw "release.config.json is missing required property: downloadNodes"
    }

    Assert-DownloadNodes -Nodes $Config.downloadNodes
}

function Assert-DownloadNodes {
    param(
        [Parameter(Mandatory = $true)]
        [object]$Nodes
    )

    $nodeList = @($Nodes)
    if ($nodeList.Count -eq 0) {
        throw "release.config.json must declare at least one downloadNodes entry."
    }

    $seenIds = @{}
    $hasOfficialFallback = $false
    foreach ($node in $nodeList) {
        foreach ($propertyName in @("id", "template", "priority", "enabled")) {
            if ($null -eq $node.PSObject.Properties[$propertyName]) {
                throw "downloadNodes entry is missing required property: $propertyName"
            }
        }

        $id = ([string]$node.id).Trim()
        $template = ([string]$node.template).Trim()
        if ($id -notmatch '^[A-Za-z0-9_-]{1,64}$') {
            throw "downloadNodes id is invalid: $id"
        }
        if ($seenIds.ContainsKey($id.ToLowerInvariant())) {
            throw "downloadNodes contains duplicate id: $id"
        }
        $seenIds[$id.ToLowerInvariant()] = $true

        if (([regex]::Matches($template, [regex]::Escape('{url}'))).Count -ne 1) {
            throw "downloadNodes template must contain exactly one {url}: $id"
        }

        try {
            $priority = [int]$node.priority
        }
        catch {
            throw "downloadNodes priority must be an integer: $id"
        }

        if ($id -eq "github-direct") {
            if ($template -ne "{url}" -or -not [bool]$node.enabled -or $priority -ne 1000) {
                throw "github-direct must be enabled with template {url} and priority 1000."
            }
            $hasOfficialFallback = $true
            continue
        }

        try {
            $candidate = $template.Replace('{url}', 'https://github.com/owner/repository/releases/download/v1.0.0/application.exe')
            $uri = [uri]$candidate
        }
        catch {
            throw "downloadNodes template is not a valid URL: $id"
        }

        if ($uri.Scheme -ne 'https') {
            throw "downloadNodes template must use HTTPS: $id"
        }
    }

    if (-not $hasOfficialFallback) {
        throw "downloadNodes must include the enabled github-direct official fallback."
    }
}

function Assert-CleanGitWorktree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Root,
        [Parameter(Mandatory = $true)]
        [string]$Label
    )

    if (-not (Test-Path -LiteralPath (Join-Path $Root '.git'))) {
        throw "$Label is not a Git worktree: $Root"
    }

    $changes = @(git -C $Root status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect $Label Git worktree."
    }
    if ($changes.Count -gt 0) {
        throw "$Label has uncommitted changes. Commit the release inputs before publishing."
    }
}

function Assert-PreparedReleaseAssets {
    param(
        [Parameter(Mandatory = $true)]
        [string]$AssetDirectory,
        [Parameter(Mandatory = $true)]
        [object]$Config,
        [Parameter(Mandatory = $true)]
        [string]$Version
    )

    $assetName = [string]$Config.releaseAssetName
    $assetPath = Join-Path $AssetDirectory $assetName
    $shaPath = Join-Path $AssetDirectory "$assetName.sha256"
    $manifestPath = Join-Path $AssetDirectory 'update.json'
    foreach ($path in @($assetPath, $shaPath, $manifestPath)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "Prepared release asset not found: $path"
        }
    }

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([string]$manifest.version -ne $Version -or [string]$manifest.tag -ne "v$Version") {
        throw "update.json version or tag does not match the requested release version."
    }
    if (([string]$manifest.repository -ne [string]$Config.repository) -or
        ([string]$manifest.asset -ne $assetName) -or
        ([string]$manifest.sha256Asset -ne "$assetName.sha256")) {
        throw "update.json does not match release.config.json asset settings."
    }

    $actualHash = (Get-FileHash -LiteralPath $assetPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ([string]$manifest.sha256 -ne $actualHash) {
        throw "update.json SHA-256 does not match the EXE."
    }
    if ([int64]$manifest.size -ne (Get-Item -LiteralPath $assetPath).Length) {
        throw "update.json size does not match the EXE."
    }
    if ((Get-Content -LiteralPath $shaPath -Raw) -notmatch [regex]::Escape($actualHash)) {
        throw "The published SHA-256 asset does not match the EXE."
    }

    Assert-DownloadNodes -Nodes $manifest.downloadNodes
    Assert-DownloadNodes -Nodes $Config.downloadNodes
    $expectedNodes = @($Config.downloadNodes)
    $actualNodes = @($manifest.downloadNodes)
    if ($actualNodes.Count -ne $expectedNodes.Count) {
        throw "update.json downloadNodes count does not match release.config.json."
    }
    for ($index = 0; $index -lt $expectedNodes.Count; $index++) {
        $expected = $expectedNodes[$index]
        $actual = $actualNodes[$index]
        if (([string]$actual.id -ne [string]$expected.id) -or
            ([string]$actual.template -ne [string]$expected.template) -or
            ([int]$actual.priority -ne [int]$expected.priority) -or
            ([bool]$actual.enabled -ne [bool]$expected.enabled)) {
            throw "update.json downloadNodes entry $index does not match release.config.json."
        }
    }
}
