# 複数選択 ABI の P/Invoke 検証 (design の verification プラン 1-9 を実装)。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class Ms {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selected(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select_toggle(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select_all(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select_none(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_at(IntPtr h, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_contains(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_delete(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_duplicate(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_node_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_node_id_at(IntPtr h, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_node_delete(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_get_transform(IntPtr h, int id, out float x, out float y, out float rot, out float sx, out float sy);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_gizmo_set_mode(IntPtr h, int m);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_gizmo_begin(IntPtr h, float x, float y);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_gizmo_update(IntPtr h, float x, float y);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_gizmo_end(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_can_undo(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_undo(IntPtr h);
}
"@
Add-Type -TypeDefinition $src

$script:pass = 0; $script:fail = 0
function Check($name, $cond) {
    if ($cond) { $script:pass++; Write-Host "  PASS  $name" }
    else       { $script:fail++; Write-Host "  FAIL  $name" -ForegroundColor Red }
}
function Tx($h,$id){ $x=0.0;$y=0.0;$r=0.0;$sx=0.0;$sy=0.0; [Ms]::acs_editor_node_get_transform($h,$id,[ref]$x,[ref]$y,[ref]$r,[ref]$sx,[ref]$sy); return ,@($x,$y) }
function NodeExists($h,$id){ $c=[Ms]::acs_editor_node_count($h); for($i=0;$i -lt $c;$i++){ if([Ms]::acs_editor_node_id_at($h,$i) -eq $id){ return $true } }; return $false }

$h = [Ms]::acs_editor_create()

Write-Host "`n[1] initial demo selection"
Check "count==1"        ([Ms]::acs_editor_selection_count($h) -eq 1)
Check "primary==1"      ([Ms]::acs_editor_selected($h) -eq 1)
Check "contains(1)"     ([Ms]::acs_editor_selection_contains($h,1) -eq 1)

Write-Host "`n[2] toggle add/remove"
[Ms]::acs_editor_select_toggle($h,4)
Check "count==2"        ([Ms]::acs_editor_selection_count($h) -eq 2)
Check "primary==4"      ([Ms]::acs_editor_selected($h) -eq 4)
Check "contains(1)&&(4)" (([Ms]::acs_editor_selection_contains($h,1) -eq 1) -and ([Ms]::acs_editor_selection_contains($h,4) -eq 1))
[Ms]::acs_editor_select_toggle($h,4)
Check "count==1 after re-toggle" ([Ms]::acs_editor_selection_count($h) -eq 1)
Check "primary==1 (remaining)"   ([Ms]::acs_editor_selected($h) -eq 1)
Check "contains(4)==0"           ([Ms]::acs_editor_selection_contains($h,4) -eq 0)

Write-Host "`n[3] single + none"
[Ms]::acs_editor_select($h,5)
Check "single: count==1 primary==5" (([Ms]::acs_editor_selection_count($h) -eq 1) -and ([Ms]::acs_editor_selected($h) -eq 5))
[Ms]::acs_editor_select_none($h)
Check "none: count==0 primary==-1"  (([Ms]::acs_editor_selection_count($h) -eq 0) -and ([Ms]::acs_editor_selected($h) -eq -1))

Write-Host "`n[4] multi-move (same world delta, one undo step)"
[Ms]::acs_editor_select($h,1); [Ms]::acs_editor_select_toggle($h,4)   # {1,4} primary=4 (Enemy @520,150)
[Ms]::acs_editor_gizmo_set_mode($h,0)
$b1 = Tx $h 1; $b4 = Tx $h 4
$u0 = [Ms]::acs_editor_can_undo($h)
$grab = [Ms]::acs_editor_gizmo_begin($h, 520, 150)   # primary=4 の自由移動ハンドル
Check "gizmo grabbed free handle (axis 3)" ($grab -eq 3)
[Ms]::acs_editor_gizmo_update($h, 545, 165)
[Ms]::acs_editor_gizmo_update($h, 570, 180)          # delta (50,30)
[Ms]::acs_editor_gizmo_end($h)
$a1 = Tx $h 1; $a4 = Tx $h 4
$d1x = $a1[0]-$b1[0]; $d1y = $a1[1]-$b1[1]
$d4x = $a4[0]-$b4[0]; $d4y = $a4[1]-$b4[1]
Write-Host ("    node1 d=({0:0.##},{1:0.##})  node4 d=({2:0.##},{3:0.##})" -f $d1x,$d1y,$d4x,$d4y)
Check "node1 delta ~ (50,30)" ((([math]::Abs($d1x-50)) -lt 0.5) -and (([math]::Abs($d1y-30)) -lt 0.5))
Check "node4 delta == node1 delta" ((([math]::Abs($d1x-$d4x)) -lt 0.01) -and (([math]::Abs($d1y-$d4y)) -lt 0.01))
$u1 = [Ms]::acs_editor_can_undo($h)
Check "exactly one undo step pushed" (($u1 - $u0) -eq 1)
[void][Ms]::acs_editor_undo($h)
$r1 = Tx $h 1; $r4 = Tx $h 4
Check "undo restored both" ((([math]::Abs($r1[0]-$b1[0])) -lt 0.01) -and (([math]::Abs($r4[0]-$b4[0])) -lt 0.01))
Check "undo restored selection {1,4}" (([Ms]::acs_editor_selection_count($h) -eq 2) -and ([Ms]::acs_editor_selection_contains($h,1) -eq 1) -and ([Ms]::acs_editor_selection_contains($h,4) -eq 1))

Write-Host "`n[5] parent+child both selected -> child not double-moved"
[Ms]::acs_editor_select($h,1); [Ms]::acs_editor_select_toggle($h,2)   # 2 is child of 1, primary=2 (@372,230)
$c2b = Tx $h 2   # node2 local (unchanged baseline)
$p1b = Tx $h 1
$grab2 = [Ms]::acs_editor_gizmo_begin($h, 372, 230)  # node2 world center
Check "grab child-primary free handle" ($grab2 -eq 3)
[Ms]::acs_editor_gizmo_update($h, 422, 260)          # delta (50,30)
[Ms]::acs_editor_gizmo_end($h)
$p1a = Tx $h 1; $c2a = Tx $h 2
# node1 (root) local should move by (50,30); node2 (child, follows) local UNCHANGED (not re-set => no double)
Check "parent moved (50,30)" ((([math]::Abs(($p1a[0]-$p1b[0])-50)) -lt 0.5) -and (([math]::Abs(($p1a[1]-$p1b[1])-30)) -lt 0.5))
Check "child local unchanged (follows parent, not re-positioned)" ((([math]::Abs($c2a[0]-$c2b[0])) -lt 0.01) -and (([math]::Abs($c2a[1]-$c2b[1])) -lt 0.01))
[void][Ms]::acs_editor_undo($h)

Write-Host "`n[6] selection_delete (batch, one undo)"
[Ms]::acs_editor_select($h,4); [Ms]::acs_editor_select_toggle($h,5)
$n0 = [Ms]::acs_editor_node_count($h)
$ud0 = [Ms]::acs_editor_can_undo($h)
$del = [Ms]::acs_editor_selection_delete($h)
Check "deleted 2" ($del -eq 2)
Check "node_count -2" ([Ms]::acs_editor_node_count($h) -eq ($n0-2))
Check "node 4 gone" (-not (NodeExists $h 4))
Check "node 5 gone" (-not (NodeExists $h 5))
Check "selection empty after delete" (([Ms]::acs_editor_selection_count($h) -eq 0) -and ([Ms]::acs_editor_selected($h) -eq -1))
Check "one undo step" (([Ms]::acs_editor_can_undo($h) - $ud0) -eq 1)
[void][Ms]::acs_editor_undo($h)
Check "undo restored node_count" ([Ms]::acs_editor_node_count($h) -eq $n0)
Check "undo restored selection {4,5}" (([Ms]::acs_editor_selection_count($h) -eq 2) -and ([Ms]::acs_editor_selection_contains($h,4) -eq 1) -and ([Ms]::acs_editor_selection_contains($h,5) -eq 1))

Write-Host "`n[7] selection_duplicate (batch, one undo)"
[Ms]::acs_editor_select($h,1); [Ms]::acs_editor_select_toggle($h,4)   # Player subtree(3) + Enemy(1)
$n0 = [Ms]::acs_editor_node_count($h)
$ud0 = [Ms]::acs_editor_can_undo($h)
$dup = [Ms]::acs_editor_selection_duplicate($h)
Check "duplicated 2 roots" ($dup -eq 2)
Check "node_count +4 (subtree 3 + 1)" ([Ms]::acs_editor_node_count($h) -eq ($n0+4))
Check "selection==2 clones" ([Ms]::acs_editor_selection_count($h) -eq 2)
$p = [Ms]::acs_editor_selected($h)
Check "primary is last clone (in set, != originals)" (([Ms]::acs_editor_selection_contains($h,$p) -eq 1) -and ($p -ne 1) -and ($p -ne 4))
Check "one undo step" (([Ms]::acs_editor_can_undo($h) - $ud0) -eq 1)
[void][Ms]::acs_editor_undo($h)
Check "undo restored node_count" ([Ms]::acs_editor_node_count($h) -eq $n0)
Check "undo restored selection {1,4}" (([Ms]::acs_editor_selection_count($h) -eq 2) -and ([Ms]::acs_editor_selection_contains($h,1) -eq 1) -and ([Ms]::acs_editor_selection_contains($h,4) -eq 1))

Write-Host "`n[8] reconcile: legacy single delete prunes a multi-selected member"
[Ms]::acs_editor_select($h,1); [Ms]::acs_editor_select_toggle($h,2)   # parent 1 + child 2
[void][Ms]::acs_editor_node_delete($h,1)   # delete parent (reaps child 2 too)
Check "selection drops dead ids" (([Ms]::acs_editor_selection_contains($h,1) -eq 0) -and ([Ms]::acs_editor_selection_contains($h,2) -eq 0))
$cnt = [Ms]::acs_editor_selection_count($h); $pri = [Ms]::acs_editor_selected($h)
Check "invariant after reconcile" ((($cnt -eq 0) -and ($pri -eq -1)) -or (($cnt -gt 0) -and ([Ms]::acs_editor_selection_contains($h,$pri) -eq 1)))
[void][Ms]::acs_editor_undo($h)

Write-Host "`n[9] select_all + invariant"
[Ms]::acs_editor_select_all($h)
$ac = [Ms]::acs_editor_selection_count($h)
Check "select_all count==node_count" ($ac -eq [Ms]::acs_editor_node_count($h))
Check "all: primary in set" ([Ms]::acs_editor_selection_contains($h, [Ms]::acs_editor_selected($h)) -eq 1)
[Ms]::acs_editor_select_none($h)
Check "none: invariant (0 <-> -1)" (([Ms]::acs_editor_selection_count($h) -eq 0) -and ([Ms]::acs_editor_selected($h) -eq -1))

Write-Host "`n[10] gizmo grab without drag => no undo step (deferred PushUndo)"
[Ms]::acs_editor_select($h,1)   # node1 @ (320,230) after all undos
[Ms]::acs_editor_gizmo_set_mode($h,0)
$ug0 = [Ms]::acs_editor_can_undo($h)
$g = [Ms]::acs_editor_gizmo_begin($h, 320, 230)
Check "grabbed free handle" ($g -eq 3)
[Ms]::acs_editor_gizmo_end($h)   # release without any update
Check "no undo step for no-op grab" (([Ms]::acs_editor_can_undo($h) - $ug0) -eq 0)

[Ms]::acs_editor_destroy($h)
Write-Host "`n==== $script:pass passed, $script:fail failed ===="