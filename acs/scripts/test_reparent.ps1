# 親子付け替え (acs_editor_node_reparent) の検証:
#   ・ワールド位置保持 (付け替えても視覚的に飛ばない → local が再計算される)
#   ・cycle 防止 (子孫を親にできない)
#   ・no-op (同一親) では undo を積まない
#   ・undo で元の親へ戻る
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class Rp {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_reparent(IntPtr h, int id, int parent);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_parent(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_get_transform(IntPtr h, int id, out float x, out float y, out float rot, out float sx, out float sy);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_undo(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_can_undo(IntPtr h);
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }
function LocalX($h,$id){ $x=0.0;$y=0.0;$r=0.0;$sx=0.0;$sy=0.0; [Rp]::acs_editor_node_get_transform($h,$id,[ref]$x,[ref]$y,[ref]$r,[ref]$sx,[ref]$sy); return $x }

# A(id1) を world(10,0)、B(id2) を world(5,0)。両方 root 直下。
$scene = "ACSCENE v1`n2`n1 -1 10 0 0 1 1 48 0.5 0.6 0.8 1 ParentA`n2 -1 5 0 0 1 1 48 0.5 0.6 0.8 1 ChildB`n"
$h=[Rp]::acs_editor_create()
Check "scene loaded" ([Rp]::acs_editor_scene_load_text($h,$scene) -eq 1)
Check "B starts under root" ([Rp]::acs_editor_node_parent($h,2) -eq -1)
Check "B local.x = 5 initially" ([math]::Abs((LocalX $h 2) - 5.0) -lt 1e-3)

Write-Host "[reparent B under A — world 位置保持]"
Check "reparent returns 1" ([Rp]::acs_editor_node_reparent($h,2,1) -eq 1)
Check "B parent now A(1)" ([Rp]::acs_editor_node_parent($h,2) -eq 1)
# world(5,0) を保持 → A は world(10,0) なので B local.x = 5-10 = -5
Check "B local.x = -5 (world preserved)" ([math]::Abs((LocalX $h 2) - (-5.0)) -lt 1e-3)
Check "can_undo after real reparent" ([Rp]::acs_editor_can_undo($h) -eq 1)

Write-Host "[no-op: 同一親へ再付け替え → undo を積まない]"
Check "reparent to same parent returns 0" ([Rp]::acs_editor_node_reparent($h,2,1) -eq 0)

Write-Host "[cycle: 祖先 A を子孫 B の下に → 拒否]"
Check "cyclic reparent returns 0" ([Rp]::acs_editor_node_reparent($h,1,2) -eq 0)
Check "A still under root" ([Rp]::acs_editor_node_parent($h,1) -eq -1)

Write-Host "[undo → B が root へ戻る (no-op/cycle は undo を積んでいない)]"
Check "undo returns 1" ([Rp]::acs_editor_undo($h) -eq 1)
Check "B back under root" ([Rp]::acs_editor_node_parent($h,2) -eq -1)
Check "B local.x = 5 restored" ([math]::Abs((LocalX $h 2) - 5.0) -lt 1e-3)
# 単一 undo で B が root に戻った時点で「no-op/cycle は積んでいない」は証明済み。
# (積んでいれば最初の undo は no-op の取り消しになり B はまだ A の下だったはず)。
# 残る undo はちょうど 1 = scene_load_text 自身の 1 エントリのみ。
Check "exactly the scene-load undo remains (=1)" ([Rp]::acs_editor_can_undo($h) -eq 1)

[Rp]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
