param([Parameter(Mandatory=$true)][string]$Flatc)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskVersion = & $Flatc --version
if ($taskVersion -ne 'flatc version 25.2.10') { throw "Expected flatc 25.2.10; got $taskVersion" }
& $Flatc --csharp -o "$taskRepo/csharp/benchmarks/Helix.SerializationBenchmarks/Generated" "$taskRepo/csharp/benchmarks/Helix.SerializationBenchmarks/world_state.fbs"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
