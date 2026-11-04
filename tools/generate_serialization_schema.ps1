param([Parameter(Mandatory=$true)][string]$Flatc)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
if ((& $Flatc --version) -ne 'flatc version 25.2.10') { throw 'Expected flatc 25.2.10' }
& $Flatc --csharp --gen-object-api --gen-onefile -o "$taskRepo/csharp/Helix.Protocol.FlatBuffers/Generated" "$taskRepo/protos/helix_mmo.fbs"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
