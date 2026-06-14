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

$h = [TH]::acs_editor_create()
Check "create" ($h -ne [IntPtr]::Zero)
[TH]::acs_editor_set_view3d($h, 1)                 # seed 3 (Ground=1,Cube=2,Sphere=3)
$a = [TH]::acs_editor_add_node3d($h, 0, "A")       # id 4
$b = [TH]::acs_editor_add_node3d($h, 1, "B")       # id 5
Check "5 nodes after add" ([TH]::acs_editor_node3d_count($h) -eq 5)
Check "A parent is root (-1)" ([TH]::acs_editor_node3d_parent($h, $a) -eq -1)

# reparent B under A
Check "reparent ok" ([TH]::acs_editor_reparent3d($h, $b, $a) -eq 1)
Check "B parent is A" ([TH]::acs_editor_node3d_parent($h, $b) -eq $a)
Check "still 5 nodes (DFS counts nested)" ([TH]::acs_editor_node3d_count($h) -eq 5)

# serialize -> load -> hierarchy preserved
$buf = New-Object byte[] 65536
[void][TH]::acs_editor_scene3d_serialize($h, $buf, $buf.Length)
$txt = Utf8Z $buf
$h2 = [TH]::acs_editor_create()
[void][TH]::acs_editor_scene3d_load_text($h2, $txt)
Check "loaded 5 nodes" ([TH]::acs_editor_node3d_count($h2) -eq 5)
Check "loaded B parent is A" ([TH]::acs_editor_node3d_parent($h2, $b) -eq $a)

# reparent back to root
Check "reparent to root ok" ([TH]::acs_editor_reparent3d($h, $b, -1) -eq 1)
Check "B parent is root again" ([TH]::acs_editor_node3d_parent($h, $b) -eq -1)

Write-Host ""
Write-Host "RESULT: $pass passed, $fail failed" -ForegroundColor $(if($fail -eq 0){"Green"}else{"Red"})
