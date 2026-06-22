# Headless P/Invoke verification of 3D parity additions (no GUI window, no mouse):
#   - acs_editor_distribute3d_selection
#   - acs_editor_node3d_component_prop_get / _set  (+ CPROP3D serialize round-trip)
#   - acs_editor_node3d_invoke_method (guard)
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
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_id_at(IntPtr h, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select3d(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_select3d_toggle(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_selected3d_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_align3d_selection(IntPtr h, int mode);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_distribute3d_selection(IntPtr h, int axis);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_component_count(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_component_prop_get(IntPtr h, int id, int slot, int prop, out float x, out float y, out float z, out float w);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_component_prop_set(IntPtr h, int id, int slot, int prop, float x, float y, float z, float w);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_invoke_method(IntPtr h, int id, int slot, [MarshalAs(UnmanagedType.LPUTF8Str)] string m);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_undo(IntPtr h);
    public static string Serialize(IntPtr h){ var b=new byte[256*1024]; acs_editor_scene3d_serialize(h,b,b.Length); int n=Array.IndexOf(b,(byte)0); if(n<0)n=b.Length; return Encoding.UTF8.GetString(b,0,n); }
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }
# Parse "N3D <id> <parent> <prim> px ..." -> X of a given id from serialized scene text.
function NodeX($txt,$id){
  foreach($line in ($txt -split "`n")){
    if($line -match "^N3D\s+$id\s+\S+\s+\S+\s+(\S+)"){ return [double]$matches[1] }
  }
  return [double]::NaN
}

$scene = @"
ACS3D v2
N3D 1 -1 2 10.0000 0.0000 0.0000 0.0000 0.0000 0.0000 16.0000 1.0000 16.0000 0.320 0.340 0.380 1.000 Ground
N3D 2 -1 0 -2.0000 0.5000 0.0000 0.0000 0.0000 0.0000 1.0000 1.0000 1.0000 0.850 0.450 0.350 1.000 Cube
CMP3D 2 FRigidBody2D
CPROP3D 2 0 2 0.9000 0.0000 0.0000 0.0000
CPROP3D 2 0 3 7.0000 0.0000 0.0000 0.0000
N3D 3 -1 1 0.0000 0.6000 0.2000 0.0000 0.0000 0.0000 1.2000 1.2000 1.2000 0.400 0.620 0.920 1.000 Sphere
"@

$h=[E]::acs_editor_create()
[E]::acs_editor_set_view3d($h,1)
Check "scene3d_load_text ok" ([E]::acs_editor_scene3d_load_text($h,$scene) -ne 0)
Check "3 nodes loaded" ([E]::acs_editor_node3d_count($h) -eq 3)

Write-Host "`n[component prop GET] CPROP3D values restored on load"
Check "cube has 1 component" ([E]::acs_editor_node3d_component_count($h,2) -eq 1)
$fx=0.0;$fy=0.0;$fz=0.0;$fw=0.0
[void][E]::acs_editor_node3d_component_prop_get($h,2,0,2,[ref]$fx,[ref]$fy,[ref]$fz,[ref]$fw)
Check "friction(prop2)=0.9 (loaded from CPROP3D)" ([math]::Abs($fx-0.9) -lt 0.001)
$mx=0.0
[void][E]::acs_editor_node3d_component_prop_get($h,2,0,3,[ref]$mx,[ref]$fy,[ref]$fz,[ref]$fw)
Check "mass(prop3)=7.0 (loaded from CPROP3D)" ([math]::Abs($mx-7.0) -lt 0.001)

Write-Host "`n[component prop SET + serialize] CPROP3D round-trip"
Check "prop_set mass=3 ok" ([E]::acs_editor_node3d_component_prop_set($h,2,0,3,3.0,0.0,0.0,0.0) -ne 0)
$mx2=0.0
[void][E]::acs_editor_node3d_component_prop_get($h,2,0,3,[ref]$mx2,[ref]$fy,[ref]$fz,[ref]$fw)
Check "prop_get reflects mass=3" ([math]::Abs($mx2-3.0) -lt 0.001)
$txt1=[E]::Serialize($h)
Check "serialize emits 'CPROP3D 2 0 3 3.0000'" ($txt1 -match "CPROP3D 2 0 3 3\.0000")
Check "serialize still emits friction 'CPROP3D 2 0 2 0.9000'" ($txt1 -match "CPROP3D 2 0 2 0\.9000")

Write-Host "`n[invoke_method guard] non-existent method returns 0, no crash"
Check "invoke bogus -> 0" ([E]::acs_editor_node3d_invoke_method($h,2,0,"__nope__") -eq 0)

Write-Host "`n[distribute3d X] middle node spreads evenly between ends"
[E]::acs_editor_select3d($h,1); [E]::acs_editor_select3d_toggle($h,2); [E]::acs_editor_select3d_toggle($h,3)
Check "3 selected" ([E]::acs_editor_selected3d_count($h) -eq 3)
Check "distribute3d returns 3" ([E]::acs_editor_distribute3d_selection($h,0) -eq 3)
$td=[E]::Serialize($h)
# sorted by X: Cube(-2,id2), Sphere(0,id3), Ground(10,id1). middle=Sphere -> -2 + (10-(-2))/2 = 4
Check "Sphere(id3) middle -> X=4.0" ([math]::Abs((NodeX $td 3)-4.0) -lt 0.01)
Check "Cube(id2) end unchanged X=-2" ([math]::Abs((NodeX $td 2)+2.0) -lt 0.01)
Check "Ground(id1) end unchanged X=10" ([math]::Abs((NodeX $td 1)-10.0) -lt 0.01)
Check "undo restores Sphere X=0" ((& { [void][E]::acs_editor_undo($h); [math]::Abs((NodeX ([E]::Serialize($h)) 3)) -lt 0.01 }))

Write-Host "`n[distribute3d guard] <3 selected -> 0"
[E]::acs_editor_select3d($h,2)   # single
Check "distribute3d <3 -> 0" ([E]::acs_editor_distribute3d_selection($h,0) -eq 0)

[E]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
if($fail -gt 0){ exit 1 }
