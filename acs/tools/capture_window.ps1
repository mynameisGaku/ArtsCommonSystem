# capture_window.ps1 — exe を起動し、ウィンドウを前面化して PNG キャプチャ後 kill する。
# 用途: ビルドした GUI サンプルの描画結果を自分の目で確認する (feedback_screenshot_verify)。
#   powershell -File capture_window.ps1 -Exe <path.exe> -Out <path.png> [-Sleep 4]
param(
    [Parameter(Mandatory=$true)] [string]$Exe,
    [Parameter(Mandatory=$true)] [string]$Out,
    [int]$Sleep = 4
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class WinCap {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hWnd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$p = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Seconds $Sleep
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 12; $i++) {
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 700
}
if ($h -eq [IntPtr]::Zero) {
    Write-Output "NO_WINDOW_HANDLE (process alive=$(-not $p.HasExited))"
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    exit 2
}
[WinCap]::ShowWindow($h, 5) | Out-Null   # SW_SHOW
# 自プロセス (Claude のターミナル等) より確実に前面化するため TOPMOST を強制する。
# HWND_TOPMOST=-1, SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW = 0x0043
[WinCap]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0043) | Out-Null
[WinCap]::BringWindowToTop($h) | Out-Null
[WinCap]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 900
$r = New-Object WinCap+RECT
[WinCap]::GetWindowRect($h, [ref]$r) | Out-Null
$w  = $r.Right - $r.Left
$ht = $r.Bottom - $r.Top
if ($w -le 0 -or $ht -le 0) { Write-Output "BAD_RECT $w x $ht"; Stop-Process -Id $p.Id -Force; exit 3 }
$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Stop-Process -Id $p.Id -Force
Write-Output "CAPTURED ${w}x${ht} -> $Out"
