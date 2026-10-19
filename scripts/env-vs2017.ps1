# KTile build environment (VS2017 / C++17)
# Usage:  . .\env-vs2017.ps1
# Verified: 2026-10-14, cl.exe 19.16.27054

$VsRoot   = "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community"
$VcTools  = "$VsRoot\VC\Tools\MSVC\14.16.27023"
$SdkRoot  = "C:\Program Files (x86)\Windows Kits\10"
$SdkVer   = "10.0.19041.0"

# ---- validate paths ----
$required = @(
    "$VcTools\bin\Hostx64\x64\cl.exe",
    "$VcTools\include",
    "$SdkRoot\Include\$SdkVer\um\Windows.h",
    "$SdkRoot\Include\$SdkVer\cppwinrt\winrt\base.h",
    "$SdkRoot\Lib\$SdkVer\um\x64\d2d1.lib",
    "$SdkRoot\Lib\$SdkVer\um\x64\dwrite.lib"
)
foreach ($p in $required) {
    if (-not (Test-Path $p)) { Write-Warning "MISSING: $p" }
}

# ---- PATH ----
$env:Path = @(
    "$VcTools\bin\Hostx64\x64",
    "$VcTools\bin\Hostx64",
    "$SdkRoot\bin\$SdkVer\x64",
    $env:Path
) -join ";"

# ---- INCLUDE (cppwinrt is a separate dir, must be added explicitly) ----
$env:INCLUDE = @(
    "$VcTools\include",
    "$SdkRoot\Include\$SdkVer\ucrt",
    "$SdkRoot\Include\$SdkVer\shared",
    "$SdkRoot\Include\$SdkVer\um",
    "$SdkRoot\Include\$SdkVer\winrt",
    "$SdkRoot\Include\$SdkVer\cppwinrt"
) -join ";"

# ---- LIB ----
$env:LIB = @(
    "$VcTools\lib\x64",
    "$SdkRoot\Lib\$SdkVer\ucrt\x64",
    "$SdkRoot\Lib\$SdkVer\um\x64"
) -join ";"

Write-Host "KTile build env ready (VS2017 / C++17 / SDK $SdkVer)" -ForegroundColor Green
Write-Host "  cl.exe : $VcTools\bin\Hostx64\x64\cl.exe"
Write-Host "  max C++: C++17 (_MSVC_LANG=201703)"
Write-Host ""
Write-Host "Recommended compile flags:" -ForegroundColor Yellow
Write-Host "  /std:c++17 /EHsc /W4 /permissive- /MT /utf-8"
Write-Host "  /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX"
Write-Host "  /D_WIN32_WINNT=0x0601   (Win7 target)"
Write-Host ""
Write-Host "Recommended libs:" -ForegroundColor Yellow
Write-Host "  d2d1 dwrite ole32 oleaut32 windowscodecs shell32 user32 gdi32 advapi32 comctl32"
