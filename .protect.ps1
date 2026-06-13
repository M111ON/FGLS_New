# .protect.ps1 — git clean interceptor
# วางใน project root แล้วเรียกใช้ครั้งเดียว:
#   powershell -ExecutionPolicy Bypass -File .protect.ps1
#
# สร้าง git wrapper function ใน PowerShell profile
# ที่ intercept `git clean` → ต้องยืนยันก่อนเสมอ

$profilePath = $PROFILE.CurrentUserAllHosts
$projectRoot = (Get-Item -Path $PSScriptRoot).FullName

$wrapper = @'

# ── git clean protector (auto-installed by .protect.ps1) ──
function git {
    param(
        [Parameter(ValueFromRemainingArguments)] $args
    )
    $cmd = $args -join ' '
    if ($cmd -match '^\s*clean\b') {
        $target = (Get-Location).Path
        Write-Host "`n⚠️  GIT CLEAN DETECTED" -ForegroundColor Red -BackgroundColor Black
        Write-Host "   Directory: $target" -ForegroundColor Yellow
        Write-Host "   Command:   git $cmd`n" -ForegroundColor Yellow
        if ($target -match 'FGLS_new') {
            Write-Host "   ⚠️  This is the FGLS project! Cancelling automatically." -ForegroundColor Red
            Write-Host "   💡 Use 'git clean --dry-run' first to preview, then add -f explicitly." -ForegroundColor Cyan
            return
        }
        $confirm = Read-Host "Type YES to confirm clean"
        if ($confirm -ne "YES") {
            Write-Host "Cancelled." -ForegroundColor Green
            return
        }
    }
    & "C:\Program Files\Git\bin\git.exe" $args
}
# ── end git clean protector ──

'@

# Check if already installed
if (Test-Path $profilePath) {
    $content = Get-Content $profilePath -Raw
    if ($content -match 'git clean protector') {
        Write-Host "✅ Protector already installed in profile: $profilePath" -ForegroundColor Green
        exit 0
    }
} else {
    # Create profile directory
    $profileDir = Split-Path $profilePath -Parent
    if (!(Test-Path $profileDir)) {
        New-Item -ItemType Directory -Path $profileDir -Force | Out-Null
    }
}

# Install
Add-Content -Path $profilePath -Value $wrapper -Encoding UTF8
Write-Host "✅ git clean protector installed in: $profilePath" -ForegroundColor Green
Write-Host "   เปิด PowerShell ใหม่ เพื่อให้มีผล" -ForegroundColor Yellow
Write-Host "   หรือรัน: . $profilePath" -ForegroundColor Yellow
