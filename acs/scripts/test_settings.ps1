# プロジェクト設定 (Project Settings) ABI の検証ハーネス。
# DLL を bin から読み、create → settings_load_text → entry 列挙 → set/add/remove → serialize を確認。
# 注意: Windows PowerShell 5.1 には Marshal.PtrToStringUTF8 が無い → 手動 UTF-8 デコード。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin   # .NET の DLL 検索 (依存 DLL 解決) はプロセス CWD を見る

$sig = @"
using System;
using System.Runtime.InteropServices;
public static class S {
  const string D = "acs_editor_abi";
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_settings_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_count(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_entry(IntPtr h, int i, [Out] byte[] b, int cap);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_set(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string c, [MarshalAs(UnmanagedType.LPUTF8Str)] string k, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_add(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string c, [MarshalAs(UnmanagedType.LPUTF8Str)] string k, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_remove(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string c, [MarshalAs(UnmanagedType.LPUTF8Str)] string k);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_serialize(IntPtr h, [Out] byte[] b, int cap);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_get_value(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string c, [MarshalAs(UnmanagedType.LPUTF8Str)] string k, [Out] byte[] b, int cap);
}
"@
Add-Type -TypeDefinition $sig

function Utf8Z([byte[]]$b) {
  $n = [Array]::IndexOf($b, [byte]0); if ($n -lt 0) { $n = $b.Length }
  return [System.Text.Encoding]::UTF8.GetString($b, 0, $n)
}

$pass = 0; $fail = 0
function Check($name, $cond) {
  if ($cond) { Write-Host "  PASS $name"; $script:pass++ } else { Write-Host "  FAIL $name" -ForegroundColor Red; $script:fail++ }
}

  $h = [S]::acs_editor_create()
  Check "create" ($h -ne [IntPtr]::Zero)

  # 1. 既定スキーマ (空テキスト = 既定値)
  [S]::acs_editor_settings_load_text($h, "")
  $n0 = [S]::acs_editor_settings_count($h)
  Check "default schema has entries ($n0)" ($n0 -ge 10)

  $buf = New-Object byte[] 1024
  [void][S]::acs_editor_settings_get_value($h, "Rendering", "MsaaSamples", $buf, $buf.Length)
  Check "default MsaaSamples=8" ((Utf8Z $buf) -eq "8")

  # 2. INI テキストで上書き + ユーザー定義の取り込み
  $ini = "[Rendering]`nMsaaSamples=4`nAmbientColor=0.2,0.3,0.4`n`n[MyGame]`nDifficulty=Hard`n"
  [S]::acs_editor_settings_load_text($h, $ini)
  $buf = New-Object byte[] 1024
  [void][S]::acs_editor_settings_get_value($h, "Rendering", "MsaaSamples", $buf, $buf.Length)
  Check "loaded MsaaSamples=4" ((Utf8Z $buf) -eq "4")
  $buf = New-Object byte[] 1024
  [void][S]::acs_editor_settings_get_value($h, "MyGame", "Difficulty", $buf, $buf.Length)
  Check "user-defined MyGame.Difficulty=Hard" ((Utf8Z $buf) -eq "Hard")

  # 3. set / add / remove
  Check "set builtin" ([S]::acs_editor_settings_set($h, "Editor", "SnapMove", "5") -eq 1)
  Check "add custom" ([S]::acs_editor_settings_add($h, "MyGame", "Lives", "3") -eq 1)
  Check "remove custom" ([S]::acs_editor_settings_remove($h, "MyGame", "Lives") -eq 1)
  Check "cannot remove builtin" ([S]::acs_editor_settings_remove($h, "Editor", "SnapMove") -eq 0)

  # 4. serialize → 主要キーが含まれる
  $sbuf = New-Object byte[] 8192
  [void][S]::acs_editor_settings_serialize($h, $sbuf, $sbuf.Length)
  $txt = Utf8Z $sbuf
  Check "serialize has [Rendering]" ($txt.Contains("[Rendering]"))
  Check "serialize has MsaaSamples=4" ($txt.Contains("MsaaSamples=4"))
  Check "serialize has [MyGame]" ($txt.Contains("[MyGame]"))
  Check "serialize keeps Difficulty=Hard" ($txt.Contains("Difficulty=Hard"))
  Check "serialize dropped removed Lives" (-not $txt.Contains("Lives"))

  # 5. entry TSV フォーマット (category\tkey\tvalue\ttype\toptions\tbuiltin\tdesc)
  $ebuf = New-Object byte[] 1024
  [void][S]::acs_editor_settings_entry($h, 0, $ebuf, $ebuf.Length)
  $fields = (Utf8Z $ebuf).Split("`t")
  Check "entry has 7 TSV fields" ($fields.Length -ge 7)

Write-Host ""
Write-Host "RESULT: $pass passed, $fail failed" -ForegroundColor $(if ($fail -eq 0) { "Green" } else { "Red" })
