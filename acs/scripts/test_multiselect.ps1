# 複数選択 ABI の選択、移動、削除、複製、undo 整合を検証する。
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'editor_abi_test_support.ps1')

$result = Invoke-AcsEditorAbiTest -Configuration $Configuration -Body {
param($context)

$src = @"
using System; using System.Runtime.InteropServices;
public static class Ms {
    const string D = @"$($context.CSharpNativeDll)";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
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

$scene = @"
ACSCENE v1
5
1 -1 320 230 0 1 1 56 0.18 0.62 0.80 1 RootNode
2 1 52 0 0 1 1 30 0.86 0.56 0.30 1 ChildA
3 1 -38 24 0 1 1 26 0.45 0.80 0.45 1 ChildB
4 -1 520 150 0.5 1 1 48 0.86 0.36 0.42 1 MoveTarget
5 -1 180 330 0 1 1 24 0.82 0.76 0.32 1 ExtraTarget
SEL 1 1 1
"@

$h = [IntPtr]::Zero
try {
    $h = [Ms]::acs_editor_create()
    if($h -eq [IntPtr]::Zero){ throw "acs_editor_create failed" }
    Check "new host node count is zero" ([Ms]::acs_editor_node_count($h) -eq 0)
    Check "new host selection is empty" ([Ms]::acs_editor_selection_count($h) -eq 0)
    Check "new host primary is absent" ([Ms]::acs_editor_selected($h) -eq -1)
    if([Ms]::acs_editor_scene_load_text($h,$scene) -ne 1){ throw "acs_editor_scene_load_text failed" }
    Check "fixture contains five nodes" ([Ms]::acs_editor_node_count($h) -eq 5)

Write-Host "`n[1] explicit fixture selection"
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
[Ms]::acs_editor_select($h,1); [Ms]::acs_editor_select_toggle($h,4)   # {1,4} primary=4 (@520,150)
[Ms]::acs_editor_gizmo_set_mode($h,0)
$b1 = Tx $h 1; $b4 = Tx $h 4
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
Check "batch move is undoable" ([Ms]::acs_editor_can_undo($h) -eq 1)
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
$del = [Ms]::acs_editor_selection_delete($h)
Check "deleted 2" ($del -eq 2)
Check "node_count -2" ([Ms]::acs_editor_node_count($h) -eq ($n0-2))
Check "node 4 gone" (-not (NodeExists $h 4))
Check "node 5 gone" (-not (NodeExists $h 5))
Check "selection empty after delete" (([Ms]::acs_editor_selection_count($h) -eq 0) -and ([Ms]::acs_editor_selected($h) -eq -1))
Check "batch delete is undoable" ([Ms]::acs_editor_can_undo($h) -eq 1)
[void][Ms]::acs_editor_undo($h)
Check "undo restored node_count" ([Ms]::acs_editor_node_count($h) -eq $n0)
Check "undo restored selection {4,5}" (([Ms]::acs_editor_selection_count($h) -eq 2) -and ([Ms]::acs_editor_selection_contains($h,4) -eq 1) -and ([Ms]::acs_editor_selection_contains($h,5) -eq 1))

Write-Host "`n[7] selection_duplicate (batch, one undo)"
[Ms]::acs_editor_select($h,1); [Ms]::acs_editor_select_toggle($h,4)   # 3-node subtree + independent root
$n0 = [Ms]::acs_editor_node_count($h)
$dup = [Ms]::acs_editor_selection_duplicate($h)
Check "duplicated 2 roots" ($dup -eq 2)
Check "node_count +4 (subtree 3 + 1)" ([Ms]::acs_editor_node_count($h) -eq ($n0+4))
Check "selection==2 clones" ([Ms]::acs_editor_selection_count($h) -eq 2)
$p = [Ms]::acs_editor_selected($h)
Check "primary is last clone (in set, != originals)" (([Ms]::acs_editor_selection_contains($h,$p) -eq 1) -and ($p -ne 1) -and ($p -ne 4))
Check "batch duplicate is undoable" ([Ms]::acs_editor_can_undo($h) -eq 1)
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

Write-Host "`n[10] gizmo grab without drag does not shadow the previous undo"
[Ms]::acs_editor_select($h,1)
[Ms]::acs_editor_gizmo_set_mode($h,0)
$seedBefore = Tx $h 1
$seedGrab = [Ms]::acs_editor_gizmo_begin($h, $seedBefore[0], $seedBefore[1])
[Ms]::acs_editor_gizmo_update($h, $seedBefore[0]+20, $seedBefore[1]+10)
[Ms]::acs_editor_gizmo_end($h)
$seedMoved = Tx $h 1
Check "seed move grabbed and applied delta" (($seedGrab -eq 3) -and (([math]::Abs(($seedMoved[0]-$seedBefore[0])-20)) -lt 0.5) -and (([math]::Abs(($seedMoved[1]-$seedBefore[1])-10)) -lt 0.5))
$noOpGrab = [Ms]::acs_editor_gizmo_begin($h, $seedMoved[0], $seedMoved[1])
[Ms]::acs_editor_gizmo_end($h)
$undoResult = [Ms]::acs_editor_undo($h)
$seedRestored = Tx $h 1
Check "no-op grab leaves seed move as next undo" (($noOpGrab -eq 3) -and ($undoResult -eq 1) -and (([math]::Abs($seedRestored[0]-$seedBefore[0])) -lt 0.01) -and (([math]::Abs($seedRestored[1]-$seedBefore[1])) -lt 0.01))

}
finally {
    if($h -ne [IntPtr]::Zero){ [Ms]::acs_editor_destroy($h) }
}

$expectedPass = 46
if (($script:pass + $script:fail) -ne $expectedPass) { throw "unexpected assertion count" }
Write-Host "`n==== $script:pass passed, $script:fail failed ===="
[pscustomobject]@{Pass=$script:pass;Fail=$script:fail;Expected=$expectedPass}
}

if($result.Fail -ne 0){ exit 1 }