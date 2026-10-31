# Ensures the local Triad Wars server is listening on https://localhost:443.
# Exits 0 when the server is up, 1 otherwise.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Test-Server {
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $c.Connect('127.0.0.1', 443)
        $c.Close()
        return $true
    } catch { return $false }
}

if (Test-Server) {
    Write-Host 'Local server already running.'
    exit 0
}

$python = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $python) {
    Write-Host 'Python was not found on PATH. Install Python 3.9+ and try again.'
    exit 1
}

# The client must point at localhost; UserOptions.dat.orig keeps the original.
& $python.Source (Join-Path $here 'useroptions.py') set Environment localhost | Out-Null

Start-Process -FilePath $python.Source -ArgumentList 'server.py' -WorkingDirectory $here -WindowStyle Minimized

foreach ($i in 1..20) {
    Start-Sleep -Milliseconds 500
    if (Test-Server) {
        Write-Host 'Local server started.'
        exit 0
    }
}
Write-Host 'The server did not start listening on port 443. Is another program using that port?'
exit 1
