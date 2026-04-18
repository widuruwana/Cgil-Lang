param(
    [Parameter(Mandatory=$true)]
    [string]$MainFile
)

# Extract the target .exe name
$exe = $MainFile -replace "\.gil$", ".exe"

# Delete the old executable if it exists so we don't run stale code
if (Test-Path $exe) { Remove-Item $exe }

# THE UNITY BUILD: Pass all standard libraries and the main file at once!
..\cgilc.exe .\std_io.gil .\std_test.gil $MainFile -o $exe

# If compilation succeeded, run it
if (Test-Path $exe) {
    Write-Host "`n[ RUNNING $exe ]" -ForegroundColor DarkGray
    & .\$exe
}