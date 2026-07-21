# Builds a fully static, single-file CallsRecorder.exe (no runtime dependencies).
$ErrorActionPreference = "Stop"
$src = Get-ChildItem "$PSScriptRoot\src\*.cpp" | ForEach-Object { $_.FullName }
windres "$PSScriptRoot\src\app.rc" -O coff -o "$PSScriptRoot\src\app.res.o"
if ($LASTEXITCODE -ne 0) { exit 1 }
g++ -std=c++17 -O2 -Wall `
    -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 `
    -municode -mwindows -static -s `
    $src "$PSScriptRoot\src\app.res.o" -o "$PSScriptRoot\CallsRecorder.exe" `
    -lole32 -loleaut32 -luuid -lmfplat -lmfreadwrite -lmfuuid `
    -lshell32 -lshlwapi -lgdi32 -luser32 -ladvapi32
if ($LASTEXITCODE -eq 0) {
    $exe = Get-Item "$PSScriptRoot\CallsRecorder.exe"
    "Built {0} ({1:N0} KB)" -f $exe.Name, ($exe.Length / 1KB)
}
