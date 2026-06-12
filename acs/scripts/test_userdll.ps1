# Verify the editor loads user-defined component types from a project reflection DLL,
# registers them, and can attach them to a node.
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin
$dll = "C:\dev\acs_github\acs\Intermediate\proj_test\UserTest\Binaries\Release\UserTest_reflect.dll"

$src = @"
using System; using System.Runtime.InteropServices;
public static class EdU {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_load_game_dll(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_user_type_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_user_type_name_at(IntPtr h, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_add_component(IntPtr h, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_component_count(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_component_prop_count([MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_component_prop_name_at([MarshalAs(UnmanagedType.LPUTF8Str)] string t, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_scene_serialize(IntPtr h);
    public static string S(IntPtr p){ if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; var b=new byte[n]; Marshal.Copy(p,b,0,n); return System.Text.Encoding.UTF8.GetString(b); }
}
"@
Add-Type -TypeDefinition $src
$fail=0
function Check($c,$m){ if($c){Write-Host "  OK  $m"}else{Write-Host "  XX  $m";$script:fail++} }

$h=[EdU]::acs_editor_create(); [EdU]::acs_editor_scene_new($h)
Check (Test-Path $dll) "reflect dll exists"
$imported=[EdU]::acs_editor_load_game_dll($h,$dll)
Check ($imported -ge 1) "load_game_dll imported $imported user type(s)"
Check ([EdU]::acs_editor_user_type_count($h) -ge 1) "user_type_count >= 1"

$names=@(); for($i=0;$i -lt [EdU]::acs_editor_user_type_count($h);$i++){ $names+=[EdU]::S([EdU]::acs_editor_user_type_name_at($h,$i)) }
Write-Host "  user types: $($names -join ', ')"
Check ($names -contains "FMoverComponent") "FMoverComponent discovered"

# Attach the user component to a node (uses the editor's registry → must succeed).
$id=[EdU]::acs_editor_add_node($h,"MyObj",-1)
$r=[EdU]::acs_editor_node_add_component($h,$id,"FMoverComponent")
Check ($r -ne 0) "attached FMoverComponent to node (ret=$r)"
Check ([EdU]::acs_editor_node_component_count($h,$id) -ge 1) "node has 1 component"
Check ([EdU]::acs_editor_component_prop_count("FMoverComponent") -eq 2) "schema has 2 props (got $([EdU]::acs_editor_component_prop_count('FMoverComponent')))"
$p0=[EdU]::S([EdU]::acs_editor_component_prop_name_at("FMoverComponent",0))
$p1=[EdU]::S([EdU]::acs_editor_component_prop_name_at("FMoverComponent",1))
Check ($p0 -eq "speed" -and $p1 -eq "amplitude") "prop names speed, amplitude (got $p0, $p1)"

# Serialize -> COMP line with the user type name (name-based, so it round-trips).
$s=[EdU]::S([EdU]::acs_editor_scene_serialize($h))
Check (($s -split "`n") -contains "COMP $id FMoverComponent") "serializes COMP $id FMoverComponent"

# Idempotent reload (hot reload): loading again should not duplicate.
$again=[EdU]::acs_editor_load_game_dll($h,$dll)
Check ([EdU]::acs_editor_user_type_count($h) -eq $names.Count) "reload does not duplicate (count stable=$($names.Count))"

[EdU]::acs_editor_destroy($h)
if($fail -eq 0){Write-Host "`nALL PASS"}else{Write-Host "`n$fail FAILED"}
