param(
    [switch]$Run = $true
)

$ErrorActionPreference = "Stop"

$cc = "gcc"
$cflags = @("-O2", "-Wall", "-Wextra")
$includes = @(
    "-Isrc",
    "-Icore/core",
    "-Icore/geo_headers",
    "-Icore/pogls_engine",
    "-Igeopixel",
    "-Iactive_updates"
)

$tests = @(
    "test_dispatch_v2",
    "test_ground_lcgw",
    "test_rewind_lcgw",
    "test_metatron_route",
    "test_frustum_wire",
    "test_geopixel_session_feed",
    "test_startup_trace_payload"
)

New-Item -ItemType Directory -Force -Path "build/integration" | Out-Null

foreach ($t in $tests) {
    $src = "tests/integration/$t.c"
    $out = "build/integration/$t.exe"
    Write-Host "Compiling $src"
    & $cc @cflags @includes $src "-o" $out
}

if ($Run) {
    foreach ($t in $tests) {
        $exe = "build/integration/$t.exe"
        Write-Host "Running $exe"
        & $exe
    }
}

Write-Host "Integration build complete."
