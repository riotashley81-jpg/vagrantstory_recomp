# Apply this project's changes to the two framework submodules (PowerShell).
# Same as scripts/apply_patches.sh; safe to re-run.
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Branch = 'vagrant-story-local'

git -C $Root submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'submodule update failed' }

foreach ($sub in 'psxrecomp', 'recomp-ui') {
    $dir = Join-Path $Root $sub
    if ((git -C $dir rev-parse --abbrev-ref HEAD) -eq $Branch) {
        Write-Host "${sub}: already on $Branch, skipping"
        continue
    }
    git -C $dir checkout -q -B $Branch
    $patches = Get-ChildItem (Join-Path $Root "patches/$sub") -Filter *.patch | Sort-Object Name
    git -C $dir -c user.name=patches -c user.email=patches@localhost `
        am -q --committer-date-is-author-date ($patches | ForEach-Object FullName)
    if ($LASTEXITCODE -ne 0) { throw "git am failed in $sub" }
    Write-Host "${sub}: applied $($patches.Count) patch(es) on $Branch"
}
