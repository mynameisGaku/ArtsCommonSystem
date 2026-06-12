# Launch the AcsEditor, wait for render, screenshot its window, then close it.
# Usage: & scripts\shot_editor.ps1 [-Out path.png] [-Wait ms]
param(
    [string]$Out   = "C:\dev\acs_github\acs\Intermediate\editor_shot.png",
    [int]   $Wait  = 3000,
    [string]$Click    = "",   # "x,y" (ウィンドウ左上からの相対座標) をキャプチャ前にクリック
    [string]$Drag     = "",   # "x1,y1,x2,y2" 左ドラッグ (スクラブ等の検証用)
    [int]   $PostWait = 0,    # クリック後・キャプチャ前の待機 ms (アニメ/物理の進行を見る)
    [string]$Keys     = "",   # クリック後に送るキーストローク (SendKeys 形式。例 "{ENTER}")
    [string]$LaunchArgs = "", # exe へ渡す起動引数 (例 プロジェクトの .acsproject パス)
    [switch]$NoMaximize,      # 最大化せず元サイズで撮る (固定サイズのランチャー等)
    [string]$DoubleClick = "" # -Click の後に "x,y" を素早く2回クリック (ダブルクリック検証用)
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
$exe = Join-Path $bin "AcsEditor.exe"
if (-not (Test-Path $exe)) { Write-Host "ERROR: exe not found: $exe"; exit 1 }

if (-not ('Win' -as [type])) {
$sig = @"
using System; using System.Runtime.InteropServices;
public static class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
Add-Type -TypeDefinition $sig
}

$p = if ($LaunchArgs -ne "") { Start-Process $exe -ArgumentList $LaunchArgs -PassThru } else { Start-Process $exe -PassThru }
try {
    for ($i = 0; $i -lt 60 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
    Start-Sleep -Milliseconds $Wait        # DX12 attach + first frames
    $p.Refresh()
    $h = $p.MainWindowHandle
    if ($h -eq 0) { Write-Host "ERROR: no main window handle"; $p.Kill(); exit 1 }
    if (-not $NoMaximize) { [void][Win]::ShowWindow($h, 3) }   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 1200
    # force above other windows (bypass foreground lock): topmost then notopmost
    [void][Win]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x43)   # HWND_TOPMOST, NOMOVE|NOSIZE|SHOWWINDOW
    Start-Sleep -Milliseconds 200
    [void][Win]::SetForegroundWindow($h)
    try { (New-Object -ComObject WScript.Shell).AppActivate($p.Id) | Out-Null } catch {}
    Start-Sleep -Milliseconds 700
    [void][Win]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x43)   # HWND_NOTOPMOST

    $r = New-Object Win+RECT
    [void][Win]::GetWindowRect($h, [ref]$r)
    $w = $r.Right - $r.Left; $hh = $r.Bottom - $r.Top
    if ($w -le 0 -or $hh -le 0) { Write-Host "ERROR: bad window rect $w x $hh"; $p.Kill(); exit 1 }

    if ($Click -ne "") {
        foreach ($pt in ($Click -split ';')) {   # "x,y;x,y" で複数クリック可
            $xy = $pt -split ','
            $cx = $r.Left + [int]$xy[0]; $cy = $r.Top + [int]$xy[1]
            [void][Win]::SetCursorPos($cx, $cy)
            Start-Sleep -Milliseconds 150
            [Win]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
            Start-Sleep -Milliseconds 60
            [Win]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
            Start-Sleep -Milliseconds 700
        }
    }

    if ($DoubleClick -ne "") {
        $xy = $DoubleClick -split ','
        $cx = $r.Left + [int]$xy[0]; $cy = $r.Top + [int]$xy[1]
        [void][Win]::SetCursorPos($cx, $cy); Start-Sleep -Milliseconds 120
        for ($d = 0; $d -lt 2; $d++) {
            [Win]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
            [Win]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
            Start-Sleep -Milliseconds 80                        # ダブルクリック閾値内
        }
        Start-Sleep -Milliseconds 700
    }

    if ($Drag -ne "") {
        $d = $Drag -split ','
        $x1 = $r.Left + [int]$d[0]; $y1 = $r.Top + [int]$d[1]
        $x2 = $r.Left + [int]$d[2]; $y2 = $r.Top + [int]$d[3]
        [void][Win]::SetCursorPos($x1, $y1); Start-Sleep -Milliseconds 120
        [Win]::mouse_event(0x02, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
        Start-Sleep -Milliseconds 60
        for ($k = 1; $k -le 12; $k++) {
            $xx = [int]($x1 + ($x2 - $x1) * $k / 12.0)
            $yy = [int]($y1 + ($y2 - $y1) * $k / 12.0)
            [void][Win]::SetCursorPos($xx, $yy); Start-Sleep -Milliseconds 30
        }
        [Win]::mouse_event(0x04, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
        Start-Sleep -Milliseconds 600
    }

    if ($Keys -ne "") {
        [void][Win]::SetForegroundWindow($h); Start-Sleep -Milliseconds 150
        [System.Windows.Forms.SendKeys]::SendWait($Keys)
        Start-Sleep -Milliseconds 500
    }

    if ($PostWait -gt 0) { Start-Sleep -Milliseconds $PostWait }   # 物理/アニメの進行を待つ

    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $hh))
    $dir = Split-Path $Out -Parent
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host "shot saved: $Out ($w x $hh)"
}
finally {
    if ($p -and -not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 400; if (-not $p.HasExited) { $p.Kill() } }
}
