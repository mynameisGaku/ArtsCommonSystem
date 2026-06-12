# Verify acs_editor_instantiate_scene mechanics with an ENGINE component
# (statically linked in editor_abi -> constructible): attach real components, tick, clear.
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class EdI {
  const string D = "acs_editor_abi";
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int p);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_add_component(IntPtr h, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_instantiate_scene(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_tick_instances(IntPtr h, float dt);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_clear_instances(IntPtr h);
  [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_instance_count(IntPtr h);
}
"@
Add-Type -TypeDefinition $src
$fail=0
function Check($c,$m){ if($c){Write-Host "  OK  $m"}else{Write-Host "  XX  $m";$script:fail++} }

$h=[EdI]::acs_editor_create(); [EdI]::acs_editor_scene_new($h)
$a=[EdI]::acs_editor_add_node($h,"A",-1)
[void][EdI]::acs_editor_node_add_component($h,$a,"FPrimitiveRenderer2D")
$b=[EdI]::acs_editor_add_node($h,"B",-1)
[void][EdI]::acs_editor_node_add_component($h,$b,"FPrimitiveRenderer2D")

Check ([EdI]::acs_editor_instance_count($h) -eq 0) "no real components before instantiate"
$n=[EdI]::acs_editor_instantiate_scene($h)
Check ($n -eq 2) "instantiate_scene built 2 real engine components (got $n)"
Check ([EdI]::acs_editor_instance_count($h) -eq 2) "2 real components attached"

# tick should run real OnUpdate without crashing
for($i=0;$i -lt 10;$i++){ [EdI]::acs_editor_tick_instances($h,0.016) }
Check ($true) "ticked 10 frames without crash"

[EdI]::acs_editor_clear_instances($h)
Check ([EdI]::acs_editor_instance_count($h) -eq 0) "clear_instances removed all real components"

# re-instantiate works (idempotent)
$n2=[EdI]::acs_editor_instantiate_scene($h)
Check ($n2 -eq 2) "re-instantiate works (got $n2)"
[EdI]::acs_editor_clear_instances($h)

[EdI]::acs_editor_destroy($h)
if($fail -eq 0){Write-Host "`nALL PASS"}else{Write-Host "`n$fail FAILED"}
