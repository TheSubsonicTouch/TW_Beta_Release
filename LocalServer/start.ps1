# Starts just the local server and exits - kept for shortcuts made before launch.py.
# Play-TriadWars.bat is the normal entry point: it starts the server *and* the game,
# and writes a report if the game dies on launch.
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here

$python = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $python) {
    Write-Host 'Python was not found on PATH. Install Python 3.9+ and try again.'
    exit 1
}

& $python.Source (Join-Path $here 'launch.py') server --root $root
exit $LASTEXITCODE
