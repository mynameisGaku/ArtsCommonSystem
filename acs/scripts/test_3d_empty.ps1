# Headless P/Invoke verification of 3D empty (group) node (no GUI, no mouse).
#   - acs_editor_add_empty3d (kind=6) + EMPTY3D serialize round-trip + reparent + subtree copy
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class E {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_set_view3d(IntPtr h, int on);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene3d_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene3d_serialize(IntPtr h, byte[] buf, int cap);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_copy_subtree3d(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_paste_subtree3d(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int parent);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_empty3d(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string name);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_kind(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_parent(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_reparent3d(IntPtr h, int child, int parent);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_id_at(IntPtr h, int i);
    public static string Copy3D(IntPtr h,int id){ IntPtr p=acs_editor_copy_subtree3d(h,id); if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; if(n==0) return ""; var b=new byte[n]; Marshal.Copy(p,b,0,n); return Encoding.UTF8.GetString(b); }
    public static string Serialize(IntPtr h){ var b=new byte[256*1024]; acs_editor_scene3d_serialize(h,b,b.Length); int n=Array.IndexOf(b,(byte)0); if(n<0)n=b.Length; return Encoding.UTF8.GetString(b,0,n); }
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

$scene = @"
ACS3D v2
N3D 10 -1 0 0 0 0 0 0 0 1 1 1 0.8 0.8 0.85 1 Mover
"@

$h=[E]::acs_editor_create()
[E]::acs_editor_set_view3d($h,1)
[void][E]::acs_editor_scene3d_load_text($h,$scene)
Check "1 node loaded" ([E]::acs_editor_node3d_count($h) -eq 1)

Write-Host "[add_empty3d] empty group node, kind=6"
$eid=[E]::acs_editor_add_empty3d($h,"Group")
Check "add_empty3d returns id" ($eid -ge 0)
Check "empty node kind == 6" ([E]::acs_editor_node3d_kind($h,$eid) -eq 6)
Check "now 2 nodes" ([E]::acs_editor_node3d_count($h) -eq 2)
Check "existing primitive (10) still kind 0 (Cube)" ([E]::acs_editor_node3d_kind($h,10) -eq 0)

Write-Host "`n[reparent under empty] empty acts as a group parent"
Check "reparent node 10 under empty" ([E]::acs_editor_reparent3d($h,10,$eid) -eq 1)
Check "node 10 parent == empty" ([E]::acs_editor_node3d_parent($h,10) -eq $eid)

Write-Host "`n[EMPTY3D serialize round-trip]"
$txt=[E]::Serialize($h)
Check "serialize emits 'EMPTY3D <eid>'" ($txt -match "EMPTY3D $eid(\b|`n)")
[void][E]::acs_editor_scene3d_load_text($h,$txt)
Check "empty kind still 6 after reload" ([E]::acs_editor_node3d_kind($h,$eid) -eq 6)
Check "child link (10 under empty) survived reload" ([E]::acs_editor_node3d_parent($h,10) -eq $eid)

Write-Host "`n[copy_subtree3d of empty group] is_empty carried + child preserved"
$cp=[E]::Copy3D($h,$eid)
Check "subtree text contains EMPTY3D" ($cp -match "EMPTY3D")
$root = [int][E]::acs_editor_paste_subtree3d($h,$cp,-1)
Check "paste returns new root" ($root -ge 0)
Check "pasted root is empty (kind 6)" ([E]::acs_editor_node3d_kind($h,$root) -eq 6)
Check "now 4 nodes (orig 2 + pasted 2)" ([E]::acs_editor_node3d_count($h) -eq 4)
# 子は決定的に root-1 (Cube id 10 < Empty id 11、同一 offset → pasted も同差)。インライン呼出で検証
# (変数代入時に PS がメソッド戻り値を取りこぼす癖を避け、Check 引数として直接評価する)。
Check "pasted child (root-1) parented under pasted empty root" ([E]::acs_editor_node3d_parent($h, ($root - 1)) -eq $root)
Check "pasted child is the Cube (kind 0)" ([E]::acs_editor_node3d_kind($h, ($root - 1)) -eq 0)

[E]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
if($fail -gt 0){ exit 1 }
