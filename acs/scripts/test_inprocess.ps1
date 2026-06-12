# Verify in-process play: the game DLL owns a real scene, a USER component (FMover)
# is constructed BY THE DLL, runs its real OnUpdate with the authored value, moving a node.
$ErrorActionPreference = 'Stop'
$dll = "C:\dev\acs_github\acs\Intermediate\proj_test\UserTest\Binaries\Release\UserTest_reflect.dll"

$src = @"
using System; using System.Runtime.InteropServices;
public static class Lib {
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr LoadLibrary(string p);
    [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr h);
    [DllImport("kernel32.dll", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr h, string n);
}
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void VV();
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr PV();
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int IPI(IntPtr s, int a);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int IPIS(IntPtr s, int a, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void VPIF5(IntPtr s, int a, float x, float y, float r, float sx, float sy);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int IPIISF4(IntPtr s, int a, int slot, [MarshalAs(UnmanagedType.LPUTF8Str)] string f, float x, float y, float z, float w);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void VPF(IntPtr s, float dt);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void VPIR5(IntPtr s, int a, ref float x, ref float y, ref float r, ref float sx, ref float sy);
[UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void VP(IntPtr s);
"@
Add-Type -TypeDefinition $src
function ProcDel($h,$n,$t){ $p=[Lib]::GetProcAddress($h,$n); if($p -eq [IntPtr]::Zero){throw "missing $n"}; return [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($p,$t) }

$fail=0
function Check($c,$m){ if($c){Write-Host "  OK  $m"}else{Write-Host "  XX  $m";$script:fail++} }
function Approx($a,$b){ return [Math]::Abs($a-$b) -lt 0.001 }

$h=[Lib]::LoadLibrary($dll)
Check ($h -ne [IntPtr]::Zero) "loaded game DLL"
$init   = ProcDel $h "acs_game_reflect_init" ([VV])
$create = ProcDel $h "acs_game_scene_create" ([PV])
$addN   = ProcDel $h "acs_game_scene_add_node" ([IPI])
$setT   = ProcDel $h "acs_game_scene_set_transform" ([VPIF5])
$addC   = ProcDel $h "acs_game_scene_add_component" ([IPIS])
$setP   = ProcDel $h "acs_game_scene_set_prop" ([IPIISF4])
$tick   = ProcDel $h "acs_game_scene_tick" ([VPF])
$getT   = ProcDel $h "acs_game_scene_get_transform" ([VPIR5])
$destroy= ProcDel $h "acs_game_scene_destroy" ([VP])

$init.Invoke()
$scene=$create.Invoke()
Check ($scene -ne [IntPtr]::Zero) "scene created"
$idx=$addN.Invoke($scene,-1)
$setT.Invoke($scene,$idx,0,0,0,1,1)
$slot=$addC.Invoke($scene,$idx,"FMover")
Check ($slot -ge 0) "FMover (USER component) constructed BY THE DLL (slot=$slot)"
$applied=$setP.Invoke($scene,$idx,$slot,"speed",10.0,0,0,0)
Check ($applied -eq 1) "authored speed=10 applied to real instance"

$x=0.0;$y=0.0;$r=0.0;$sx=0.0;$sy=0.0
$tick.Invoke($scene,0.1)   # FMover.OnUpdate: x += speed*dt = 1.0
$getT.Invoke($scene,$idx,[ref]$x,[ref]$y,[ref]$r,[ref]$sx,[ref]$sy)
Write-Host "  after 1 tick: x=$x (expect 1.0)"
Check (Approx $x 1.0) "real OnUpdate ran in-process: node moved by speed*dt"
$tick.Invoke($scene,0.1)
$getT.Invoke($scene,$idx,[ref]$x,[ref]$y,[ref]$r,[ref]$sx,[ref]$sy)
Check (Approx $x 2.0) "2nd tick -> x=2.0"

$destroy.Invoke($scene)
Check ($true) "scene destroyed by DLL (no cross-DLL free)"
[void][Lib]::FreeLibrary($h)
if($fail -eq 0){Write-Host "`nALL PASS"}else{Write-Host "`n$fail FAILED"}
