# Launch AcsEditor on a project, click Play, HOLD a key while the node moves, then screenshot.
# Pure ASCII only (avoids cp932 dame-moji BOM corruption). For verifying input-driven components.
# Usage: scripts\shot_input_test.ps1 -Out out.png -Project path.acsproject -PlayClick "40,72" -VK 0x44 -HoldMs 1300
param(
    [string]$Out       = "C:\dev\acs_github\acs\Intermediate\editor_input_shot.png",
    [string]$Project   = "C:\dev\acs_github\acs\Intermediate\proj_test\UserTest\UserTest.acsproject",
    [string]$PlayClick = "40,72",
    [int]   $VK        = 0x44,   # 'D' key
    [int]   $Wait      = 6000,
    [int]   $HoldMs    = 1300
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
$exe = Join-Path $bin "AcsEditor.exe"
if (-not (Test-Path $exe)) { Write-Host "ERROR: exe not found: $exe"; exit 1 }

if (-not ('WinI' -as [type])) {
$sig = @"
using System; using System.Runtime.InteropServices;
public static class WinI {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
Add-Type -TypeDefinition $sig
}

$p = Start-Process $exe -ArgumentList $Project -PassThru
try {
    for ($i = 0; $i -lt 60 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    Start-Sleep -Milliseconds $Wait
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq 0) { Write-Host "ERROR: no main window handle"; $p.Kill(); exit 1 }
    [void][WinI]::ShowWindow($h, 3)   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 1200
    [void][WinI]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x43)
    Start-Sleep -Milliseconds 200
    [void][WinI]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 500
    [void][WinI]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x43)

    $r = New-Object WinI+RECT
    [void][WinI]::GetWindowRect($h, [ref]$r)
    $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top

    # Click Play.
    $xy = $PlayClick -split ','
    $cx = $r.Left + [int]$xy[0]; $cy = $r.Top + [int]$xy[1]
    [void][WinI]::SetCursorPos($cx, $cy); Start-Sleep -Milliseconds 150
    [WinI]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [WinI]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 800

    # Re-foreground so key events reach the WPF window, then HOLD the key.
    [void][WinI]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 300
    [WinI]::keybd_event([byte]$VK, 0, 0, [IntPtr]::Zero)         # key down
    Start-Sleep -Milliseconds $HoldMs                            # node moves while held
    # capture while still held
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $hh))
    [WinI]::keybd_event([byte]$VK, 0, 2, [IntPtr]::Zero)         # key up (KEYEVENTF_KEYUP)

    $dir = Split-Path $Out -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "shot saved: $Out ($w x $hh)"
}
finally {
    if ($p -and -not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 400; if (-not $p.HasExited) { $p.Kill() } }
}
