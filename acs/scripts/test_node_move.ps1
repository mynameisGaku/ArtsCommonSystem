# Verify acs_editor_node_move reorders siblings (before/after) and reparents (child).
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin
$src = @"
using System; using System.Runtime.InteropServices;
public static class EdM {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_move(IntPtr h, int id, int target, int mode);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_id_at(IntPtr h, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_parent(IntPtr h, int id);
}
"@
Add-Type -TypeDefinition $src
$fail = 0
function Check($cond,$msg){ if($cond){Write-Host "  OK  $msg"}else{Write-Host "  XX  $msg";$script:fail++} }
function RootOrder($h){ $o=@(); $n=[EdM]::acs_editor_node_count($h); for($i=0;$i -lt $n;$i++){ $id=[EdM]::acs_editor_node_id_at($h,$i); if([EdM]::acs_editor_node_parent($h,$id) -lt 0){ $o+=$id } }; return ,$o }

$h=[EdM]::acs_editor_create(); [EdM]::acs_editor_scene_new($h)
$a=[EdM]::acs_editor_add_node($h,"A",-1)
$b=[EdM]::acs_editor_add_node($h,"B",-1)
$c=[EdM]::acs_editor_add_node($h,"C",-1)
Check ((RootOrder $h) -join ',' -eq "$a,$b,$c") "initial order A,B,C"

# move C before A -> C,A,B
[void][EdM]::acs_editor_node_move($h,$c,$a,0)
Check ((RootOrder $h) -join ',' -eq "$c,$a,$b") "C before A -> C,A,B (got $((RootOrder $h) -join ','))"

# move B before C -> B,C,A
[void][EdM]::acs_editor_node_move($h,$b,$c,0)
Check ((RootOrder $h) -join ',' -eq "$b,$c,$a") "B before C -> B,C,A (got $((RootOrder $h) -join ','))"

# move B after A -> C,A,B
[void][EdM]::acs_editor_node_move($h,$b,$a,1)
Check ((RootOrder $h) -join ',' -eq "$c,$a,$b") "B after A -> C,A,B (got $((RootOrder $h) -join ','))"

# child: move B as child of A -> root order C,A ; B parent = A
[void][EdM]::acs_editor_node_move($h,$b,$a,2)
Check ((RootOrder $h) -join ',' -eq "$c,$a") "B made child of A -> root C,A"
Check ([EdM]::acs_editor_node_parent($h,$b) -eq $a) "B parent == A"

[EdM]::acs_editor_destroy($h)
if($fail -eq 0){Write-Host "`nALL PASS"}else{Write-Host "`n$fail FAILED"}
