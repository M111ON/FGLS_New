<#
.synopsis
  svc-launcher.ps1 — TTS/VLM/LLM Service Pipe Launcher (WinForms GUI)
  Like dgfs-launcher.ps1 but for pipe-svc.exe — pick service type, open/close pipe,
  write/read tensors, checkpoint/restore, and run daemon mode.

  Usage:
    .\svc-launcher.ps1                  # Launch GUI
    .\svc-launcher.ps1 -NoLaunch        # Just show help
#>

param(
    [switch]$NoLaunch
)

$scriptName = "svc-launcher.ps1"
$scriptVersion = "0.1.0"

# ── Help ──────────────────────────────────────────────────────
function Show-Help {
    @"
$scriptName v$scriptVersion — TTS/VLM/LLM Service Pipe Launcher

Usage:
  .\$scriptName                        # Launch GUI
  .\$scriptName -NoLaunch              # Show this help

Requires:
  - collection\pipe\pipe-svc.exe (built from pipe_service_cli.c)
  - Windows (WinForms)

Buttons:
  Open       — Open pipe of selected type with given name
  Close      — Close current pipe
  Status     — Show current pipe stats
  Write File — Pick a file to write as tensor
  Read File  — Read tensor to a file
  Daemon     — Run interactive daemon (keep-alive) mode
"@
}

if ($NoLaunch) { Show-Help; exit }

# ── Check for pipe-svc.exe ───────────────────────────────────
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$exePath = Join-Path $scriptDir "pipe-svc.exe"
$pipeDir = $scriptDir

# Try relative from script location
if (-not (Test-Path $exePath)) {
    # Also check collection\pipe\
    $exePath = Join-Path $scriptDir "collection\pipe\pipe-svc.exe"
    $pipeDir = Join-Path $scriptDir "collection\pipe"
}
if (-not (Test-Path $exePath)) {
    Write-Warning "pipe-svc.exe not found! Build it first:"
    Write-Warning "  cd collection\pipe && gcc -O0 -std=c11 -I.. -I../../runner -I../../collection -o pipe-svc.exe pipe_service_cli.c"
    $exePath = Read-Host "Enter full path to pipe-svc.exe (or press Enter to exit)"
    if (-not $exePath) { exit }
    $pipeDir = Split-Path -Parent $exePath
}

# ── WinForms ──────────────────────────────────────────────────
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName Microsoft.VisualBasic

$form = New-Object System.Windows.Forms.Form
$form.Text = "svc-launcher v$scriptVersion — Service Pipe Manager"
$form.Size = New-Object System.Drawing.Size(640, 520)
$form.StartPosition = "CenterScreen"
$form.Icon = [System.Drawing.SystemIcons]::Application

# Font
$font = New-Object System.Drawing.Font("Consolas", 9)
$fontBold = New-Object System.Drawing.Font("Consolas", 9, [System.Drawing.FontStyle]::Bold)

# ═══════════════════════════════════════════════════════════════
# LAYOUT
# ═══════════════════════════════════════════════════════════════
$x = 12
$y = 12
$labelW = 80
$fieldW = 180
$rowH = 28
$btnW = 90
$btnH = 26

# Helper: create Point (avoid PowerShell 5.1 arithmetic-in-args bug)
function Pt { param([int]$a,[int]$b) New-Object System.Drawing.Point($a,$b) }

# ── Row 0: Service Type ─────────────────────────────────────
$lblType = New-Object System.Windows.Forms.Label
$lblType.Text = "Service:"
$lblType.Location = Pt $x ($y+4)
$lblType.Size = New-Object System.Drawing.Size($labelW, 20)
$lblType.Font = $font
$form.Controls.Add($lblType)

$cmbType = New-Object System.Windows.Forms.ComboBox
$cmbType.Location = Pt ($x+$labelW) $y
$cmbType.Size = New-Object System.Drawing.Size(120, 22)
$cmbType.DropDownStyle = [System.Windows.Forms.ComboBoxStyle]::DropDownList
$cmbType.Font = $fontBold
$cmbType.Items.AddRange(@("auto", "tts", "vlm", "llm"))
$cmbType.SelectedIndex = 0
$form.Controls.Add($cmbType)

# ── Row 0b: Pipe Name ────────────────────────────────────────
$lblName = New-Object System.Windows.Forms.Label
$lblName.Text = "Pipe Name:"
$lblName.Location = Pt ($x+$labelW+130) ($y+4)
$lblName.Size = New-Object System.Drawing.Size(80, 20)
$lblName.Font = $font
$form.Controls.Add($lblName)

$txtName = New-Object System.Windows.Forms.TextBox
$txtName.Location = Pt ($x+$labelW+210) $y
$txtName.Size = New-Object System.Drawing.Size(180, 22)
$txtName.Text = "my-service"
$txtName.Font = $font
$form.Controls.Add($txtName)

# ── Row 1: Backend File ──────────────────────────────────────
$y = 40
$lblBack = New-Object System.Windows.Forms.Label
$lblBack.Text = "Backend:"
$lblBack.Location = Pt $x ($y+4)
$lblBack.Size = New-Object System.Drawing.Size($labelW, 20)
$lblBack.Font = $font
$form.Controls.Add($lblBack)

$txtBack = New-Object System.Windows.Forms.TextBox
$txtBack.Location = Pt ($x+$labelW) $y
$txtBack.Size = New-Object System.Drawing.Size(350, 22)
$txtBack.Font = $font
$txtBack.Text = Join-Path $pipeDir "svc.pipe"
$form.Controls.Add($txtBack)

$btnBrowse = New-Object System.Windows.Forms.Button
$btnBrowse.Text = "Browse..."
$btnBrowse.Location = Pt ($x+$labelW+360) ($y-1)
$btnBrowse.Size = New-Object System.Drawing.Size(80, 24)
$btnBrowse.Font = $font
$form.Controls.Add($btnBrowse)

# ── Row 2: Action Buttons ────────────────────────────────────
$y = 72
$btnOpen = New-Object System.Windows.Forms.Button
$btnOpen.Text = "Open"
$btnOpen.Location = Pt $x $y
$btnOpen.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnOpen.Font = $fontBold
$btnOpen.BackColor = [System.Drawing.Color]::LightGreen
$form.Controls.Add($btnOpen)

$btnClose = New-Object System.Windows.Forms.Button
$btnClose.Text = "Close"
$btnClose.Location = Pt ($x+$btnW+6) $y
$btnClose.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnClose.Font = $font
$form.Controls.Add($btnClose)

$btnStatus = New-Object System.Windows.Forms.Button
$btnStatus.Text = "Status"
$btnStatus.Location = Pt ($x+($btnW+6)*2) $y
$btnStatus.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnStatus.Font = $font
$form.Controls.Add($btnStatus)

$btnWrite = New-Object System.Windows.Forms.Button
$btnWrite.Text = "Write File"
$btnWrite.Location = Pt ($x+($btnW+6)*3) $y
$btnWrite.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnWrite.Font = $font
$form.Controls.Add($btnWrite)

$btnRead = New-Object System.Windows.Forms.Button
$btnRead.Text = "Read File"
$btnRead.Location = Pt ($x+($btnW+6)*4) $y
$btnRead.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnRead.Font = $font
$form.Controls.Add($btnRead)

# ── Row 3: More Buttons ──────────────────────────────────────
$y = 100
$btnCheckpoint = New-Object System.Windows.Forms.Button
$btnCheckpoint.Text = "Checkpoint"
$btnCheckpoint.Location = Pt $x $y
$btnCheckpoint.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnCheckpoint.Font = $font
$form.Controls.Add($btnCheckpoint)

$btnRestore = New-Object System.Windows.Forms.Button
$btnRestore.Text = "Restore"
$btnRestore.Location = Pt ($x+$btnW+6) $y
$btnRestore.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnRestore.Font = $font
$form.Controls.Add($btnRestore)

$btnDaemon = New-Object System.Windows.Forms.Button
$btnDaemon.Text = "Daemon"
$btnDaemon.Location = Pt ($x+($btnW+6)*2) $y
$btnDaemon.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnDaemon.Font = $fontBold
$btnDaemon.BackColor = [System.Drawing.Color]::LightSkyBlue
$form.Controls.Add($btnDaemon)

$btnClassify = New-Object System.Windows.Forms.Button
$btnClassify.Text = "Classify"
$btnClassify.Location = Pt ($x+($btnW+6)*3) $y
$btnClassify.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnClassify.Font = $font
$form.Controls.Add($btnClassify)

$btnSpeak = New-Object System.Windows.Forms.Button
$btnSpeak.Text = "Speak"
$btnSpeak.Location = Pt ($x+($btnW+6)*4) $y
$btnSpeak.Size = New-Object System.Drawing.Size($btnW, $btnH)
$btnSpeak.Font = $fontBold
$btnSpeak.BackColor = [System.Drawing.Color]::LightGreen
$form.Controls.Add($btnSpeak)

# ── Row 4: Classify input ────────────────────────────────────
$y = 130
$lblClassify = New-Object System.Windows.Forms.Label
$lblClassify.Text = "Tensor name:"
$lblClassify.Location = Pt $x ($y+4)
$lblClassify.Size = New-Object System.Drawing.Size($labelW, 20)
$lblClassify.Font = $font
$form.Controls.Add($lblClassify)

$txtClassify = New-Object System.Windows.Forms.TextBox
$txtClassify.Location = Pt ($x+$labelW) $y
$txtClassify.Size = New-Object System.Drawing.Size(350, 22)
$txtClassify.Font = $font
$txtClassify.Text = "model.vision.blocks.0.attn.qkv.weight"
$form.Controls.Add($txtClassify)

# ── Row 4b: Speak text ───────────────────────────────────────
$y += $rowH
$lblSpeak = New-Object System.Windows.Forms.Label
$lblSpeak.Text = "Speak text:"
$lblSpeak.Location = Pt $x ($y+4)
$lblSpeak.Size = New-Object System.Drawing.Size($labelW, 20)
$lblSpeak.Font = $font
$form.Controls.Add($lblSpeak)

$txtSpeak = New-Object System.Windows.Forms.TextBox
$txtSpeak.Location = Pt ($x+$labelW) $y
$txtSpeak.Size = New-Object System.Drawing.Size(400, 22)
$txtSpeak.Font = $font
$txtSpeak.Text = "Hello, this is Kokoro text to speech."
$form.Controls.Add($txtSpeak)

# ── Output textbox ────────────────────────────────────────────
$y += $rowH + 6
$lblOut = New-Object System.Windows.Forms.Label
$lblOut.Text = "Output:"
$lblOut.Location = Pt $x $y
$lblOut.Size = New-Object System.Drawing.Size(60, 20)
$lblOut.Font = $fontBold
$form.Controls.Add($lblOut)

$txtOutput = New-Object System.Windows.Forms.TextBox
$txtOutput.Location = Pt $x ($y+20)
$txtOutput.Size = New-Object System.Drawing.Size(600, 250)
$txtOutput.Multiline = $true
$txtOutput.ScrollBars = "Vertical"
$txtOutput.Font = New-Object System.Drawing.Font("Consolas", 8)
$txtOutput.ReadOnly = $true
$txtOutput.BackColor = [System.Drawing.Color]::FromArgb(240, 240, 240)
$form.Controls.Add($txtOutput)

# ── Help functions ───────────────────────────────────────────
function Write-OutputBox {
    param([string]$text)
    $txtOutput.AppendText($text + "`r`n")
    $txtOutput.SelectionStart = $txtOutput.TextLength
    $txtOutput.ScrollToCaret()
}

function Invoke-PipeSvc {
    param([string[]]$arguments)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.Arguments = ($arguments -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    if ($out) { Write-OutputBox $out.Trim() }
    if ($err) { Write-OutputBox "ERROR: $($err.Trim())" }
    return $p.ExitCode
}

function Get-SelectedType { $cmbType.SelectedItem.ToLower() }
function Get-PipeArg { "--pipe `"$($txtBack.Text)`"" }
function Get-TypeNameArg { "$(Get-SelectedType) $($txtName.Text)" }

# ── Button handlers ─────────────────────────────────────────
$btnOpen.Add_Click({
    $type = Get-SelectedType
    $name = $txtName.Text
    $back = $txtBack.Text
    if (-not $back) {
        Invoke-PipeSvc "open", $type, $name
    } else {
        Invoke-PipeSvc "open", $type, $name, "--pipe", "`"$back`""
    }
})

$btnClose.Add_Click({
    Invoke-PipeSvc "close"
})

$btnStatus.Add_Click({
    $back = $txtBack.Text
    if ($back) { Invoke-PipeSvc "status", "--pipe", "`"$back`"" }
    else { Write-OutputBox "No backend path set" }
})

$btnWrite.Add_Click({
    if (-not $txtBack.Text) { Write-OutputBox "Need backend path"; return }
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select file to write as tensor"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $defaultName = [System.IO.Path]::GetFileNameWithoutExtension($dlg.FileName)
        $tensor = [Microsoft.VisualBasic.Interaction]::InputBox("Tensor name:", "Write Tensor", $defaultName)
        if (-not $tensor) { $tensor = $defaultName }
        Invoke-PipeSvc "write", $tensor, "--file", "`"$($dlg.FileName)`"", "--pipe", "`"$($txtBack.Text)`""
    }
})

$btnRead.Add_Click({
    if (-not $txtBack.Text) { Write-OutputBox "Need backend path"; return }
    $tensor = [Microsoft.VisualBasic.Interaction]::InputBox("Tensor name to read:", "Read Tensor", $txtClassify.Text)
    if (-not $tensor) { return }
    $dlg = New-Object System.Windows.Forms.SaveFileDialog
    $dlg.Title = "Save tensor as..."
    $dlg.FileName = "$tensor.bin"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        Invoke-PipeSvc "read", $tensor, "--out", "`"$($dlg.FileName)`"", "--pipe", "`"$($txtBack.Text)`""
    }
})

$btnCheckpoint.Add_Click({
    if (-not $txtBack.Text) { Write-OutputBox "Need backend path"; return }
    $snap = [Microsoft.VisualBasic.Interaction]::InputBox("Save checkpoint to:", "Checkpoint", "snap.bin")
    if (-not $snap) { return }
    Invoke-PipeSvc "checkpoint", $snap, "--pipe", "`"$($txtBack.Text)`""
})

$btnRestore.Add_Click({
    if (-not $txtBack.Text) { Write-OutputBox "Need backend path"; return }
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select checkpoint file to restore"
    $dlg.Filter = "Snap files (*.snap;*.bin)|*.snap;*.bin|All files (*.*)|*.*"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        Invoke-PipeSvc "restore", $dlg.FileName, "--pipe", "`"$($txtBack.Text)`""
    }
})

$btnDaemon.Add_Click({
    if (-not $txtBack.Text) { Write-OutputBox "Need backend path for daemon mode"; return }
    Write-OutputBox "Launching daemon in console window..."
    Write-OutputBox "Daemon commands: status, list, write name path, read name, close, quit"
    Write-OutputBox "Exit daemon with: quit"
    Start-Process -NoNewWindow -FilePath $exePath -ArgumentList "daemon", "--pipe", "$($txtBack.Text)"
})

$btnClassify.Add_Click({
    $name = $txtClassify.Text
    if ($name) { Invoke-PipeSvc "classify", $name }
})

$btnSpeak.Add_Click({
    $text = $txtSpeak.Text
    if (-not $text) { Write-OutputBox "Need text to speak"; return }
    $out = [Microsoft.VisualBasic.Interaction]::InputBox("Output WAV path:", "Speak", "speech.wav")
    if (-not $out) { return }
    Write-OutputBox "Generating speech: '$text'"
    Invoke-PipeSvc "speak", "`"$text`"", "--out", "`"$out`"", "--voice", "af_heart"
})

$btnBrowse.Add_Click({
    $dlg = New-Object System.Windows.Forms.SaveFileDialog
    $dlg.Title = "Backend file (DRamTile persistence)"
    $dlg.FileName = "svc.pipe"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $txtBack.Text = $dlg.FileName
    }
})

# ── Status bar ───────────────────────────────────────────────
$statusBar = New-Object System.Windows.Forms.StatusBar
$statusBar.Text = "Ready | pipe-svc: $exePath"
$form.Controls.Add($statusBar)

# ═══════════════════════════════════════════════════════════════
# SHOW
# ═══════════════════════════════════════════════════════════════
Write-OutputBox "svc-launcher v$scriptVersion ready"
Write-OutputBox "pipe-svc: $exePath"
Write-OutputBox "pipe dir: $pipeDir"
Write-OutputBox "Select service type, set backend, then Open -> Write/Read/Status"

[void]$form.ShowDialog()
