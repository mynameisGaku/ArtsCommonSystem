# ラバーバンド (矩形) 選択 ABI の P/Invoke 検証。
# 既定カメラ (screen==world) で demo シーンのノード中心を矩形に入れて選択を確認する。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class Bx {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selected(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_selection_contains(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int  acs_editor_select_box(IntPtr h, float x0, float y0, float x1, float y1, int additive);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_set_marquee(IntPtr h, int active, float x0, float y0, float x1, float y1);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_render(IntPtr h, float dt);
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

$scene = @"
ACSCENE v1
5
1 -1 320 230 0 1 1 56 0.18 0.62 0.80 1 Player
2 1 52 0 0 1 1 30 0.86 0.56 0.30 1 Sprite
3 1 -38 24 0 1 1 26 0.45 0.80 0.45 1 Collider
4 -1 520 150 0.5 1 1 48 0.86 0.36 0.42 1 Enemy
5 -1 180 330 0 1 1 24 0.82 0.76 0.32 1 Pickup
"@

$h = [Bx]::acs_editor_create()
if ($h -eq [IntPtr]::Zero) { throw "acs_editor_create failed" }
if ([Bx]::acs_editor_scene_load_text($h, $scene) -ne 1) {
    [Bx]::acs_editor_destroy($h)
    throw "acs_editor_scene_load_text failed"
}

# demo: 1 Player(320,230) 2 Sprite(372,230) 3 Collider(282,254) 4 Enemy(520,150) 5 Pickup(180,330)
Write-Host "`n[1] box replace selects centers inside (1,2,3)"
$n = [Bx]::acs_editor_select_box($h, 250,200, 400,280, 0)
Check "returned 3 in box" ($n -eq 3)
Check "count==3"        ([Bx]::acs_editor_selection_count($h) -eq 3)
Check "contains 1,2,3"  ((([Bx]::acs_editor_selection_contains($h,1)) -eq 1) -and (([Bx]::acs_editor_selection_contains($h,2)) -eq 1) -and (([Bx]::acs_editor_selection_contains($h,3)) -eq 1))
Check "excludes 4,5"    ((([Bx]::acs_editor_selection_contains($h,4)) -eq 0) -and (([Bx]::acs_editor_selection_contains($h,5)) -eq 0))
Check "primary in set"  ([Bx]::acs_editor_selection_contains($h, [Bx]::acs_editor_selected($h)) -eq 1)

Write-Host "`n[2] box additive keeps existing primary + adds"
[Bx]::acs_editor_select($h,4)           # {4} primary=4
$n = [Bx]::acs_editor_select_box($h, 250,200, 400,280, 1)
Check "count==4 (4 + 1,2,3)" ([Bx]::acs_editor_selection_count($h) -eq 4)
Check "contains 4 and 1"     ((([Bx]::acs_editor_selection_contains($h,4)) -eq 1) -and (([Bx]::acs_editor_selection_contains($h,1)) -eq 1))
Check "additive primary stays 4" ([Bx]::acs_editor_selected($h) -eq 4)

Write-Host "`n[3] empty box (replace) clears selection"
$n = [Bx]::acs_editor_select_box($h, 0,0, 10,10, 0)
Check "returned 0"     ($n -eq 0)
Check "count==0"       ([Bx]::acs_editor_selection_count($h) -eq 0)
Check "primary==-1"    ([Bx]::acs_editor_selected($h) -eq -1)

Write-Host "`n[4] reversed-corner box normalizes (same result)"
$n = [Bx]::acs_editor_select_box($h, 400,280, 250,200, 0)   # x0>x1, y0>y1
Check "reversed selects 3" ($n -eq 3)

Write-Host "`n[5] set_marquee does not crash (overlay state)"
[Bx]::acs_editor_set_marquee($h, 1, 100,100, 300,300)
[Bx]::acs_editor_set_marquee($h, 0, 0,0,0,0)
Check "set_marquee ok" $true

[Bx]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
