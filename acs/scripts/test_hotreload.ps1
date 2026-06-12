# Hot reload smoke test:
#  launch editor on a project, enable Hot Reload, modify Game.cpp externally,
#  then verify the editor auto-rebuilds + relaunches the game.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$proj = "C:\dev\acs_github\acs\Intermediate\proj_test\BuildTest\BuildTest.acsproject"
$gamecpp = "C:\dev\acs_github\acs\Intermediate\proj_test\BuildTest\Source\Game.cpp"
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
$exe = Join-Path $bin "AcsEditor.exe"

if (-not ('WinH' -as [type])) {
@"
using System; using System.Runtime.InteropServices;
public static class WinH {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@ | ForEach-Object { Add-Type -TypeDefinition $_ }
}

# Reset Game.cpp clear color to the dark default before the test.
$txt = [System.IO.File]::ReadAllText($gamecpp)
$txt = $txt -replace 'SetClearColor\([^\)]*\)', 'SetClearColor(0.04f, 0.045f, 0.055f)'
[System.IO.File]::WriteAllText($gamecpp, $txt, (New-Object System.Text.UTF8Encoding $false))

$p = Start-Process $exe -ArgumentList $proj -PassThru
try {
  for ($i=0; $i -lt 60 -and $p.MainWindowHandle -eq 0; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh() }
  Start-Sleep -Milliseconds 3500
  $p.Refresh(); $h = $p.MainWindowHandle
  [void][WinH]::ShowWindow($h, 3); Start-Sleep -Milliseconds 1000
  [void][WinH]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x43); Start-Sleep -Milliseconds 200
  [void][WinH]::SetForegroundWindow($h); Start-Sleep -Milliseconds 500
  [void][WinH]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x43)
  $r = New-Object WinH+RECT; [void][WinH]::GetWindowRect($h, [ref]$r)

  # Click the Hot Reload toggle (~445,80 window-relative).
  [void][WinH]::SetCursorPos($r.Left + 445, $r.Top + 80); Start-Sleep -Milliseconds 200
  [WinH]::mouse_event(0x02,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 60
  [WinH]::mouse_event(0x04,0,0,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 800
  Write-Host "clicked Hot Reload toggle"

  # Modify Game.cpp -> triggers watcher -> rebuild + relaunch.
  Start-Sleep -Milliseconds 800
  $txt2 = [System.IO.File]::ReadAllText($gamecpp)
  $txt2 = $txt2 -replace 'SetClearColor\([^\)]*\)', 'SetClearColor(0.45f, 0.06f, 0.10f)'
  [System.IO.File]::WriteAllText($gamecpp, $txt2, (New-Object System.Text.UTF8Encoding $false))
  Write-Host "modified Game.cpp (clear color -> red). waiting for rebuild..."

  Start-Sleep -Milliseconds 16000   # debounce + rebuild + relaunch

  # Re-foreground the editor and capture its console.
  [void][WinH]::SetWindowPos($h, [IntPtr](-1), 0,0,0,0, 0x43); Start-Sleep -Milliseconds 200
  [void][WinH]::SetForegroundWindow($h); Start-Sleep -Milliseconds 400
  [void][WinH]::SetWindowPos($h, [IntPtr](-2), 0,0,0,0, 0x43)
  [void][WinH]::GetWindowRect($h, [ref]$r)
  $w = $r.Right-$r.Left; $hh = $r.Bottom-$r.Top
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $hh))
  $out = "C:\dev\acs_github\acs\Intermediate\hotreload.png"
  $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
  Write-Host "shot: $out"
}
finally {
  Get-Process BuildTest -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  if ($p -and -not $p.HasExited) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 500; if (-not $p.HasExited) { $p.Kill() } }
}
