# Build the managed bridge.
#
# Not part of the CMake build, deliberately. It needs a C# compiler and a .NET
# runtime, neither of which the rest of this project requires, and a machine
# without them should still build and run GoBot -- it simply cannot call into
# the other helper's code.
#
# There is no .NET SDK on the machine this was written for, only the compiler
# that comes inside Visual Studio's build tools. So the references are the
# runtime's own assemblies rather than the SDK's reference packs, and the
# runtime configuration is written by hand rather than generated.

$ErrorActionPreference = 'Stop'

$csc = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\Roslyn\csc.exe"
if (-not (Test-Path $csc)) {
    $found = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio" -Recurse -Filter csc.exe `
             -ErrorAction SilentlyContinue -Depth 6 | Select-Object -First 1
    if (-not $found) { throw "No C# compiler found. Visual Studio build tools are needed." }
    $csc = $found.FullName
}

# The newest .NET 6 runtime present. The helper targets net6.0; a newer runtime
# would load it by roll-forward, but matching it exactly avoids finding out
# which of its dependencies disagree.
$runtime = Get-ChildItem "C:\Program Files\dotnet\shared\Microsoft.NETCore.App" `
           -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '6.*' } |
           Sort-Object Name | Select-Object -Last 1
if (-not $runtime) { throw "No .NET 6 runtime installed; the helper needs one." }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path (Split-Path -Parent $here) "build\bin\Release"
New-Item -ItemType Directory -Force $out | Out-Null

# Only the assemblies that actually carry managed metadata: that folder also
# holds native libraries, and csc refuses the whole command line over one.
$refs = Get-ChildItem $runtime.FullName -Filter *.dll | ForEach-Object {
    try { [void][System.Reflection.AssemblyName]::GetAssemblyName($_.FullName)
          "-r:`"$($_.FullName)`"" } catch {}
}

Write-Host "compiling against $($refs.Count) assemblies from $($runtime.Name)"

# Built as one array first. Splatting the pieces inline -- @(...) + $refs + @(...)
# straight after the call operator -- parses as something else entirely and the
# compiler never sees the source file.
$arguments = @('-nologo', '-nostdlib', '-noconfig', '-langversion:latest',
               '-target:library', "-out:$out\GoBotBridge.dll")
$arguments += $refs
$arguments += "$here\GoBotBridge.cs"

& $csc @arguments
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

Copy-Item "$here\GoBotBridge.runtimeconfig.json" "$out\GoBotBridge.runtimeconfig.json" -Force
Write-Host "built $out\GoBotBridge.dll"
