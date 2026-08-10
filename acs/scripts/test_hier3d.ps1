# 3D hierarchy ABI (reparent / parent query / hierarchical serialize round-trip) verification.
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin

$sig = @"
using System;
using System.Runtime.InteropServices;
public static class TH {
  const string D = "acs_editor_abi";
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_set_view3d(IntPtr h, int on);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_count(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node3d(IntPtr h, int prim, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_reparent3d(IntPtr h, int child, int parent);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_parent(IntPtr h, int id);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene3d_serialize(IntPtr h, [Out] byte[] b, int cap);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene3d_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
}
"@
Add-Type -TypeDefinition $sig
function Utf8Z([byte[]]$b){ $n=[Array]::IndexOf($b,[byte]0); if($n -lt 0){$n=$b.Length}; return [System.Text.Encoding]::UTF8.GetString($b,0,$n) }
$pass=0;$fail=0; function Check($n,$c){ if($c){Write-Host "  PASS $n";$script:pass++}else{Write-Host "  FAIL $n" -ForegroundColor Red;$script:fail++} }

$h = [IntPtr]::Zero
$h2 = [IntPtr]::Zero
try {
  $h = [TH]::acs_editor_create()
  Check "create" ($h -ne [IntPtr]::Zero)
  [TH]::acs_editor_set_view3d($h, 1)
  Check "3D view starts empty" ([TH]::acs_editor_node3d_count($h) -eq 0)

  $a = [TH]::acs_editor_add_node3d($h, 0, "ParentFixture")
  $b = [TH]::acs_editor_add_node3d($h, 1, "ChildFixture")
  Check "explicit ids start at 1" ($a -eq 1 -and $b -eq 2)
  Check "two explicit nodes" ([TH]::acs_editor_node3d_count($h) -eq 2)
  Check "parent fixture starts at root" ([TH]::acs_editor_node3d_parent($h, $a) -eq -1)

  Check "reparent succeeds" ([TH]::acs_editor_reparent3d($h, $b, $a) -eq 1)
  Check "child fixture uses explicit parent" ([TH]::acs_editor_node3d_parent($h, $b) -eq $a)
  Check "nested hierarchy still has two nodes" ([TH]::acs_editor_node3d_count($h) -eq 2)

  # 明示的な親子fixtureだけを別ハンドルへ往復する。
  $buf = New-Object byte[] 65536
  [void][TH]::acs_editor_scene3d_serialize($h, $buf, $buf.Length)
  $txt = Utf8Z $buf
  $h2 = [TH]::acs_editor_create()
  Check "load succeeds" ([TH]::acs_editor_scene3d_load_text($h2, $txt) -eq 1)
  Check "loaded two nodes" ([TH]::acs_editor_node3d_count($h2) -eq 2)
  Check "loaded child keeps parent" ([TH]::acs_editor_node3d_parent($h2, $b) -eq $a)

  Check "reparent to root succeeds" ([TH]::acs_editor_reparent3d($h, $b, -1) -eq 1)
  Check "child fixture returns to root" ([TH]::acs_editor_node3d_parent($h, $b) -eq -1)
}
finally {
  [TH]::acs_editor_destroy($h2)
  [TH]::acs_editor_destroy($h)
}

Write-Host ""
Write-Host "RESULT: $pass passed, $fail failed" -ForegroundColor $(if($fail -eq 0){"Green"}else{"Red"})
if($fail -gt 0){ exit 1 }
