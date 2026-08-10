# 3D シーン ABI (ノード追加 / transform / シリアライズ往復) の検証。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin

$sig = @"
using System;
using System.Runtime.InteropServices;
public static class T3 {
  const string D = "acs_editor_abi";
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_set_view3d(IntPtr h, int on);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_count(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node3d(IntPtr h, int prim, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_set_transform(IntPtr h, int id, float px, float py, float pz, float rx, float ry, float rz, float sx, float sy, float sz);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_get_transform(IntPtr h, int id, [Out] float[] o9);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_id_at(IntPtr h, int i);
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
  $h = [T3]::acs_editor_create()
  Check "create" ($h -ne [IntPtr]::Zero)
  [T3]::acs_editor_set_view3d($h, 1)
  Check "3D view starts empty" ([T3]::acs_editor_node3d_count($h) -eq 0)

  $emptyBuf = New-Object byte[] 64
  [void][T3]::acs_editor_scene3d_serialize($h, $emptyBuf, $emptyBuf.Length)
  Check "empty scene is canonical" ((Utf8Z $emptyBuf) -ceq "ACS3D v2`n")

  $id = [T3]::acs_editor_add_node3d($h, 1, "RoundTripNode")
  Check "first explicit node has id 1" ($id -eq 1)
  Check "one explicit node" ([T3]::acs_editor_node3d_count($h) -eq 1)
  [void][T3]::acs_editor_node3d_set_transform($h, $id, 3.5, 2.0, -1.0, 0,45,0, 1.5,1.5,1.5)
  $tf = New-Object float[] 9
  [void][T3]::acs_editor_node3d_get_transform($h, $id, $tf)
  Check "transform stored (pos.x=3.5)" ([Math]::Abs($tf[0]-3.5) -lt 1e-3)
  Check "transform stored (rot.y=45)" ([Math]::Abs($tf[4]-45) -lt 1e-3)

  # 明示的に作ったノードだけを別ハンドルへ往復する。
  $buf = New-Object byte[] 65536
  [void][T3]::acs_editor_scene3d_serialize($h, $buf, $buf.Length)
  $txt = Utf8Z $buf
  Check "serialize has ACS3D header" ($txt.StartsWith("ACS3D v2`n"))
  Check "serialize has explicit node" ($txt.Contains("RoundTripNode"))

  $h2 = [T3]::acs_editor_create()
  Check "load succeeds" ([T3]::acs_editor_scene3d_load_text($h2, $txt) -eq 1)
  Check "loaded one node" ([T3]::acs_editor_node3d_count($h2) -eq 1)
  $tf2 = New-Object float[] 9
  [void][T3]::acs_editor_node3d_get_transform($h2, $id, $tf2)
  Check "roundtrip pos.x=3.5" ([Math]::Abs($tf2[0]-3.5) -lt 1e-2)
  Check "roundtrip scale=1.5" ([Math]::Abs($tf2[6]-1.5) -lt 1e-2)
}
finally {
  [T3]::acs_editor_destroy($h2)
  [T3]::acs_editor_destroy($h)
}

Write-Host ""
Write-Host "RESULT: $pass passed, $fail failed" -ForegroundColor $(if($fail -eq 0){"Green"}else{"Red"})
if($fail -gt 0){ exit 1 }
