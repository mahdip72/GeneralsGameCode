$ErrorActionPreference = 'Stop'

$probePath = Join-Path $PSScriptRoot 'Stage5PowerShellModuleHost.Tests.ps1'
$probe = Get-FileHash -LiteralPath $probePath -Algorithm SHA256
if ([string]::IsNullOrWhiteSpace($probe.Hash)) {
    throw 'Windows PowerShell host did not expose Get-FileHash.'
}

Write-Output 'Stage 5 Windows PowerShell module-host contract passed.'
