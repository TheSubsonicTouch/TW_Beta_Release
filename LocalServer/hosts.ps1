# Points the two dead hostnames this game has baked into its executable at 127.0.0.1, or
# puts the hosts file back. Called by Block-Online.bat / Restore-Online.bat, which supply
# the Administrator prompt.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File hosts.ps1 block
#   powershell -NoProfile -ExecutionPolicy Bypass -File hosts.ps1 restore
param([ValidateSet('block', 'restore')][string]$Action = 'block')

$ErrorActionPreference = 'Stop'
$hosts  = Join-Path $env:SystemRoot 'System32\drivers\etc\hosts'
$backup = "$hosts.triadwars-backup"
$marker = '# Triad Wars local server - Block-Online / Restore-Online'
$entries = @(
    '127.0.0.1 prod.services.triadwars.com',
    '127.0.0.1 tw.cdn.sqexeu.com'
)

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $id).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host 'Windows will ask for Administrator - say yes, the window does the rest.'
    $args = '-NoProfile -ExecutionPolicy Bypass -File "{0}" {1}' -f $PSCommandPath, $Action
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $args
    exit 0
}

$text = if (Test-Path -LiteralPath $hosts) { [IO.File]::ReadAllText($hosts) } else { '' }

if ($Action -eq 'block') {
    if ($text -match 'prod\.services\.triadwars\.com') {
        Write-Host 'Already blocked: the game already stays on this computer.'
        exit 0
    }
    Copy-Item -LiteralPath $hosts -Destination $backup -Force
    $lines = $text.TrimEnd() + "`r`n`r`n$marker`r`n" + ($entries -join "`r`n") + "`r`n"
    [IO.File]::WriteAllText($hosts, $lines, [Text.Encoding]::ASCII)
    Write-Host 'Blocked: prod.services.triadwars.com and tw.cdn.sqexeu.com now point here.'
    Write-Host 'The local server answers both, so nothing leaves this machine.'
} else {
    if (Test-Path -LiteralPath $backup) {
        Copy-Item -LiteralPath $backup -Destination $hosts -Force
        Remove-Item -LiteralPath $backup -Force
        Write-Host 'Restored the hosts file from its backup.'
    } elseif ($text -match [regex]::Escape($marker)) {
        $kept = $text -split "`r?`n" | Where-Object {
            $_ -ne $marker -and $entries -notcontains $_
        }
        [IO.File]::WriteAllText($hosts, (($kept -join "`r`n").TrimEnd() + "`r`n"),
                                [Text.Encoding]::ASCII)
        Write-Host 'Removed the two entries from the hosts file.'
    } else {
        Write-Host 'Nothing to restore: the hosts file has no Triad Wars entries.'
    }
}

try { & ipconfig /flushdns | Out-Null } catch { }
