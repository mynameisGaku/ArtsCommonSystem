# Launch a GUI validator on a secondary monitor without taking focus, capture
# its own HWND, then terminate only the process started by this script unless
# -KeepRunning is supplied after a successful capture.
param(
    [Parameter(Mandatory = $true)] [string]$Exe,
    [Parameter(Mandatory = $true)] [string]$Out,
    [int]$Sleep = 4,
    [string[]]$Arguments = @(),
    [int]$MonitorIndex = -1,
    [ValidateSet('Auto', 'PrintWindow', 'Screen')]
    # DXGI/D3D12 child surfaces can make PrintWindow report success while
    # returning a black GPU region. The validator is deliberately kept visible
    # on the target monitor, so screen capture is the reliable visual-QA default.
    [string]$CaptureMode = 'Screen',
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

public static class WinCap {
  [DllImport("user32.dll")]
  public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
  [DllImport("user32.dll")]
  public static extern bool ShowWindow(IntPtr hWnd, int command);
  [DllImport("user32.dll")]
  public static extern bool SetWindowPos(
      IntPtr hWnd, IntPtr insertAfter, int x, int y, int width, int height,
      uint flags);
  [DllImport("user32.dll")]
  public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")]
  public static extern bool IsWindow(IntPtr hWnd);
  [DllImport("user32.dll")]
  private static extern uint GetWindowThreadProcessId(
      IntPtr hWnd, out uint processId);

  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  private static extern bool CreateProcessW(
      string applicationName, StringBuilder commandLine,
      IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles,
      uint creationFlags, IntPtr environment, string currentDirectory,
      ref STARTUPINFO startupInfo, out PROCESS_INFORMATION processInformation);
  [DllImport("kernel32.dll")]
  private static extern bool CloseHandle(IntPtr handle);
  [DllImport("kernel32.dll", SetLastError=true)]
  private static extern bool GetExitCodeProcess(
      IntPtr process, out uint exitCode);
  [DllImport("kernel32.dll", SetLastError=true)]
  private static extern bool TerminateProcess(
      IntPtr process, uint exitCode);

  [StructLayout(LayoutKind.Sequential)]
  public struct RECT {
    public int Left, Top, Right, Bottom;
  }

  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  private struct STARTUPINFO {
    public uint cb;
    public string lpReserved;
    public string lpDesktop;
    public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    public uint dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }

  [StructLayout(LayoutKind.Sequential)]
  private struct PROCESS_INFORMATION {
    public IntPtr hProcess, hThread;
    public uint dwProcessId, dwThreadId;
  }

  public sealed class LaunchedProcess : IDisposable {
    private IntPtr processHandle;

    internal LaunchedProcess(int processId, IntPtr handle) {
      ProcessId = processId;
      processHandle = handle;
    }

    public int ProcessId { get; private set; }

    public void LeaveRunning() {
      IntPtr handle = processHandle;
      processHandle = IntPtr.Zero;
      if (handle != IntPtr.Zero) {
        // Closing this script's process handle does not terminate or otherwise
        // interact with the exact process that CreateProcessW launched.
        CloseHandle(handle);
      }
    }

    public void Dispose() {
      IntPtr handle = processHandle;
      processHandle = IntPtr.Zero;
      if (handle == IntPtr.Zero) return;

      uint exitCode;
      if (!GetExitCodeProcess(handle, out exitCode) || exitCode == 259u) {
        // The handle identifies the exact process CreateProcessW returned, so
        // cleanup cannot accidentally terminate a later process that reused
        // the same numeric PID.
        TerminateProcess(handle, 1u);
      }
      CloseHandle(handle);
    }
  }

  private static string QuoteArgument(string value) {
    if (value.Length > 0 &&
        value.IndexOfAny(new char[] {' ', '\t', '"'}) < 0) {
      return value;
    }
    var result = new StringBuilder("\"");
    int slashes = 0;
    foreach (char ch in value) {
      if (ch == '\\') {
        ++slashes;
      } else if (ch == '"') {
        result.Append('\\', slashes * 2 + 1).Append('"');
        slashes = 0;
      } else {
        result.Append('\\', slashes).Append(ch);
        slashes = 0;
      }
    }
    result.Append('\\', slashes * 2).Append('"');
    return result.ToString();
  }

  public static bool IsProcessForeground(int processId) {
    IntPtr foreground = GetForegroundWindow();
    if (foreground == IntPtr.Zero) return false;
    uint foregroundProcessId;
    GetWindowThreadProcessId(foreground, out foregroundProcessId);
    return foregroundProcessId == unchecked((uint)processId);
  }

  public static LaunchedProcess StartNoActivate(
      string exe, string[] args, int initialX, int initialY) {
    var command = new StringBuilder(QuoteArgument(exe));
    foreach (string arg in args ?? new string[0]) {
      command.Append(' ').Append(QuoteArgument(arg ?? ""));
    }

    var startup = new STARTUPINFO();
    startup.cb = (uint)Marshal.SizeOf(typeof(STARTUPINFO));
    startup.dwFlags = 0x00000001u | 0x00000004u;
    startup.wShowWindow = 4; // SW_SHOWNOACTIVATE
    startup.dwX = initialX;
    startup.dwY = initialY;

    PROCESS_INFORMATION info;
    bool started = CreateProcessW(
        exe, command, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero,
        Path.GetDirectoryName(exe), ref startup, out info);
    if (!started) {
      throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    CloseHandle(info.hThread);
    try {
      return new LaunchedProcess(checked((int)info.dwProcessId), info.hProcess);
    } catch {
      TerminateProcess(info.hProcess, 1u);
      CloseHandle(info.hProcess);
      throw;
    }
  }
}
"@

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "Executable not found: $Exe"
}
$Exe = [System.IO.Path]::GetFullPath(
    (Get-Item -LiteralPath $Exe).FullName)
$Out = [System.IO.Path]::GetFullPath($Out)

$screens = [System.Windows.Forms.Screen]::AllScreens
$targetScreen = $null
if ($MonitorIndex -ge 0 -and $MonitorIndex -lt $screens.Count) {
    $targetScreen = $screens[$MonitorIndex]
} else {
    $targetScreen = $screens |
        Where-Object { -not $_.Primary } |
        Select-Object -First 1
}
if ($null -eq $targetScreen) {
    $targetScreen = [System.Windows.Forms.Screen]::PrimaryScreen
    Write-Output 'NO_SECONDARY_MONITOR; using primary without activation'
}

$work = $targetScreen.WorkingArea
$launch = $null
$process = $null
$bitmap = $null
$graphics = $null
$raisedForScreenCapture = $false

function Assert-ValidatorNotForeground {
    param([int]$ValidatorProcessId)
    if ([WinCap]::IsProcessForeground($ValidatorProcessId)) {
        throw 'Validator unexpectedly took foreground focus'
    }
}

function Resolve-ValidatorWindow {
    param(
        [System.Diagnostics.Process]$ValidatorProcess,
        [int]$ValidatorProcessId,
        [IntPtr]$FallbackWindow,
        [int]$Attempts
    )

    for ($attempt = 0; $attempt -lt $Attempts; ++$attempt) {
        $ValidatorProcess.Refresh()
        Assert-ValidatorNotForeground $ValidatorProcessId
        if ($ValidatorProcess.HasExited) {
            throw "Process exited before capture (exit=$($ValidatorProcess.ExitCode))"
        }

        $candidate = $ValidatorProcess.MainWindowHandle
        if ($null -ne $candidate -and
            $candidate -ne [IntPtr]::Zero -and
            [WinCap]::IsWindow($candidate)) {
            return $candidate
        }
        if ($FallbackWindow -ne [IntPtr]::Zero -and
            [WinCap]::IsWindow($FallbackWindow)) {
            return $FallbackWindow
        }
        Start-Sleep -Milliseconds 250
    }
    return [IntPtr]::Zero
}

try {
    $launch = [WinCap]::StartNoActivate(
        $Exe, $Arguments, $work.Left + 24, $work.Top + 24)
    $childProcessId = $launch.ProcessId
    $process = [System.Diagnostics.Process]::GetProcessById($childProcessId)

    $window = Resolve-ValidatorWindow `
        $process $childProcessId ([IntPtr]::Zero) 120
    if ($window -eq [IntPtr]::Zero) {
        throw 'Timed out waiting for the validator window'
    }

    $initial = New-Object WinCap+RECT
    if (-not [WinCap]::GetWindowRect($window, [ref]$initial)) {
        throw 'GetWindowRect failed before placement'
    }
    $margin = 24
    $initialWidth = [Math]::Max(1, $initial.Right - $initial.Left)
    $initialHeight = [Math]::Max(1, $initial.Bottom - $initial.Top)
    $availableWidth = [Math]::Max(1, $work.Width - 2 * $margin)
    $availableHeight = [Math]::Max(1, $work.Height - 2 * $margin)
    $targetWidth = [Math]::Min(
        $initialWidth, $availableWidth)
    $targetHeight = [Math]::Min(
        $initialHeight, $availableHeight)
    $targetX = $work.Left +
        [Math]::Max(0, [int](($work.Width - $targetWidth) / 2))
    $targetY = $work.Top +
        [Math]::Max(0, [int](($work.Height - $targetHeight) / 2))

    # HWND_TOP=0; SWP_NOACTIVATE|SWP_SHOWWINDOW=0x0050.
    $moveOk = [WinCap]::SetWindowPos(
        $window, [IntPtr]::Zero, $targetX, $targetY,
        $targetWidth, $targetHeight, 0x0050)
    if (-not $moveOk) {
        throw 'SetWindowPos failed while placing the validator'
    }
    [WinCap]::ShowWindow($window, 4) | Out-Null # SW_SHOWNOACTIVATE
    Assert-ValidatorNotForeground $childProcessId

    $remainingMilliseconds = [Math]::Max(
        [long]0, ([long]$Sleep) * 1000L)
    while ($remainingMilliseconds -gt 0) {
        $waitChunk = [Math]::Min([long]250, $remainingMilliseconds)
        Start-Sleep -Milliseconds ([int]$waitChunk)
        $remainingMilliseconds -= $waitChunk
        Assert-ValidatorNotForeground $childProcessId
        $process.Refresh()
        if ($process.HasExited) {
            throw "Process exited before capture (exit=$($process.ExitCode))"
        }
    }

    # WPF may recreate its top-level HWND during startup. Re-resolve and place
    # the final handle without activation before capturing it. If both the
    # cached HWND and MainWindowHandle are temporarily unavailable, retry
    # rather than replacing a valid handle with null/zero.
    $finalWindow = Resolve-ValidatorWindow `
        $process $childProcessId $window 20
    if ($finalWindow -eq [IntPtr]::Zero) {
        throw 'Validator window disappeared before capture'
    }
    $window = $finalWindow
    # The editor restores its saved layout during Loaded. Re-apply the target
    # monitor even when the HWND did not change, otherwise that late restore
    # can silently move the validator back to the primary display.
    $moveOk = [WinCap]::SetWindowPos(
        $window, [IntPtr]::Zero, $targetX, $targetY,
        $targetWidth, $targetHeight, 0x0050)
    if (-not $moveOk) {
        throw 'SetWindowPos failed before final capture'
    }
    [WinCap]::ShowWindow($window, 4) | Out-Null
    Start-Sleep -Milliseconds 500
    # Win the last race against a layout-save callback without activating the
    # window, then validate the physical capture rectangle below.
    $moveOk = [WinCap]::SetWindowPos(
        $window, [IntPtr]::Zero, $targetX, $targetY,
        $targetWidth, $targetHeight, 0x0050)
    if (-not $moveOk) {
        throw 'SetWindowPos failed during final placement verification'
    }
    if (-not [WinCap]::IsWindow($window)) {
        throw 'Validator window disappeared before capture'
    }
    Assert-ValidatorNotForeground $childProcessId

    $rect = New-Object WinCap+RECT
    if (-not [WinCap]::GetWindowRect($window, [ref]$rect)) {
        throw 'GetWindowRect failed before capture'
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Invalid window rectangle: ${width}x${height}"
    }
    if ($rect.Left -lt $work.Left -or $rect.Top -lt $work.Top -or
        $rect.Right -gt $work.Right -or $rect.Bottom -gt $work.Bottom) {
        throw (
            "Validator is outside target monitor working area: " +
            "window=$($rect.Left),$($rect.Top),$($rect.Right),$($rect.Bottom) " +
            "work=$($work.Left),$($work.Top),$($work.Right),$($work.Bottom)")
    }

    $outDirectory = [System.IO.Path]::GetDirectoryName($Out)
    if (-not [string]::IsNullOrEmpty($outDirectory)) {
        [System.IO.Directory]::CreateDirectory($outDirectory) | Out-Null
    }

    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $printOk = $false
    $captureMethod = ''
    if ($CaptureMode -eq 'Screen') {
        # A GPU child HWND cannot be captured reliably with PrintWindow. Raise
        # the validator only for the screen-copy interval so an unrelated
        # foreground window cannot occlude the evidence. SWP_NOACTIVATE keeps
        # keyboard/mouse ownership with the user's current application.
        $raiseFlags = 0x0053 # NOSIZE|NOMOVE|NOACTIVATE|SHOWWINDOW
        if (-not [WinCap]::SetWindowPos(
                $window, [IntPtr](-1), 0, 0, 0, 0, $raiseFlags)) {
            throw 'SetWindowPos failed while raising validator for capture'
        }
        $raisedForScreenCapture = $true
        # WPF親画面を前面へ移した直後は、別HWNDのGPU子画面が一時的に親の下へ隠れる。
        # 子画面の再配置と次のPresentを待ち、Editor背景だけを誤って証拠画像にしない。
        Start-Sleep -Milliseconds 1000
        Assert-ValidatorNotForeground $childProcessId
        $size = New-Object System.Drawing.Size($width, $height)
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $size)
        $captureMethod = 'Screen'
    } else {
        $hdc = $graphics.GetHdc()
        try {
            $printOk = [WinCap]::PrintWindow($window, $hdc, 2)
        } finally {
            $graphics.ReleaseHdc($hdc)
        }
        if ($printOk) {
            $captureMethod = 'PrintWindow'
        } elseif ($CaptureMode -eq 'PrintWindow') {
            throw 'PrintWindow failed in explicit PrintWindow capture mode'
        } else {
            $size = New-Object System.Drawing.Size($width, $height)
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $size)
            $captureMethod = 'ScreenFallback'
        }
    }
    $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    Write-Output (
        "CAPTURED ${width}x${height} monitor=$($targetScreen.DeviceName) " +
        "rect=$($rect.Left),$($rect.Top),$($rect.Right),$($rect.Bottom) " +
        "move=$moveOk method=$captureMethod print=$printOk -> $Out")
    if ($KeepRunning) {
        $launch.LeaveRunning()
        Write-Output (
            "LEFT_RUNNING pid=$childProcessId " +
            "monitor=$($targetScreen.DeviceName)")
    }
} finally {
    try {
        if ($raisedForScreenCapture -and
            $null -ne $window -and
            $window -ne [IntPtr]::Zero -and
            [WinCap]::IsWindow($window)) {
            # HWND_NOTOPMOST=-2; preserve focus and the validated rectangle.
            [WinCap]::SetWindowPos(
                $window, [IntPtr](-2), 0, 0, 0, 0, 0x0013) | Out-Null
        }
        if ($null -ne $graphics) {
            $graphics.Dispose()
        }
        if ($null -ne $bitmap) {
            $bitmap.Dispose()
        }
    } finally {
        try {
            if ($null -ne $launch) {
                $launch.Dispose()
            }
        } finally {
            if ($null -ne $process) {
                $process.Dispose()
            }
        }
    }
}
