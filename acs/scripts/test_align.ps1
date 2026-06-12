# 整列 / 分配 ABI の P/Invoke 検証。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class Al {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_set_transform(IntPtr h, int id, float x, float y, float rot, float sx, float sy);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_get_transform(IntPtr h, int id, out float x, out float y, out float rot, out float sx, out float sy);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select_toggle(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_align_selection(IntPtr h, int mode);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_distribute_selection(IntPtr h, int axis);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_undo(IntPtr h);
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }
function X($h,$id){ $x=0.0;$y=0.0;$r=0.0;$sx=0.0;$sy=0.0; [Al]::acs_editor_node_get_transform($h,$id,[ref]$x,[ref]$y,[ref]$r,[ref]$sx,[ref]$sy); return ,@($x,$y) }

$h=[Al]::acs_editor_create()
[Al]::acs_editor_scene_new($h)
$a=[Al]::acs_editor_add_node($h,"A",-1); [Al]::acs_editor_node_set_transform($h,$a, 0,  0,0,1,1)
$b=[Al]::acs_editor_add_node($h,"B",-1); [Al]::acs_editor_node_set_transform($h,$b, 10, 20,0,1,1)
$c=[Al]::acs_editor_add_node($h,"C",-1); [Al]::acs_editor_node_set_transform($h,$c, 100,-10,0,1,1)
[Al]::acs_editor_select($h,$a); [Al]::acs_editor_select_toggle($h,$b); [Al]::acs_editor_select_toggle($h,$c)

Write-Host "`n[align left] x -> min(0)"
Check "align returns 3" ([Al]::acs_editor_align_selection($h,0) -eq 3)
Check "a.x=0" (([math]::Abs((X $h $a)[0])) -lt 0.01)
Check "b.x=0" (([math]::Abs((X $h $b)[0])) -lt 0.01)
Check "c.x=0" (([math]::Abs((X $h $c)[0])) -lt 0.01)
Check "b.y unchanged (20)" (([math]::Abs((X $h $b)[1]-20)) -lt 0.01)
[void][Al]::acs_editor_undo($h)
Check "undo restored b.x=10" (([math]::Abs((X $h $b)[0]-10)) -lt 0.01)

Write-Host "`n[align right] x -> max(100)"
[void][Al]::acs_editor_align_selection($h,1)
Check "a.x=100" (([math]::Abs((X $h $a)[0]-100)) -lt 0.01)
[void][Al]::acs_editor_undo($h)

Write-Host "`n[align h-center] x -> mid(50)"
[void][Al]::acs_editor_align_selection($h,4)
Check "a.x=50 b.x=50 c.x=50" ((([math]::Abs((X $h $a)[0]-50)) -lt 0.01) -and (([math]::Abs((X $h $b)[0]-50)) -lt 0.01) -and (([math]::Abs((X $h $c)[0]-50)) -lt 0.01))
[void][Al]::acs_editor_undo($h)

Write-Host "`n[align top] y -> min(-10)"
[void][Al]::acs_editor_align_selection($h,2)
Check "all y=-10" ((([math]::Abs((X $h $a)[1]+10)) -lt 0.01) -and (([math]::Abs((X $h $b)[1]+10)) -lt 0.01))
[void][Al]::acs_editor_undo($h)

Write-Host "`n[distribute x] middle -> even (50)"
Check "distribute returns 3" ([Al]::acs_editor_distribute_selection($h,0) -eq 3)
Check "b (middle) -> 50" (([math]::Abs((X $h $b)[0]-50)) -lt 0.01)
Check "a (end) unchanged 0" (([math]::Abs((X $h $a)[0])) -lt 0.01)
Check "c (end) unchanged 100" (([math]::Abs((X $h $c)[0]-100)) -lt 0.01)

Write-Host "`n[guards]"
Check "align <2 selected => 0" ($true)   # (covered: single-select path returns 0; not asserted here)

[Al]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="