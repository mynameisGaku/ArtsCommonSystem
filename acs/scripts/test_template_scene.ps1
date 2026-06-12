# Validate the project-template ACSCENE strings load correctly via the editor ABI.
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class EdT {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_scene_serialize(IntPtr h);
    public static string S(IntPtr p){ if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; var b=new byte[n]; Marshal.Copy(p,b,0,n); return System.Text.Encoding.UTF8.GetString(b); }
}
"@
Add-Type -TypeDefinition $src
$fail = 0
function Check($cond, $msg) { if ($cond) { Write-Host "  OK  $msg" } else { Write-Host "  XX  $msg"; $script:fail++ } }

$blank = "ACSCENE v1`n0`n"

$twod = @"
ACSCENE v1
3
1 -1 0.0000 220.0000 0.0000 7.0000 0.4000 48.00 0.250 0.280 0.340 1.000 Ground
2 -1 0.0000 -160.0000 0.0000 1.0000 1.0000 48.00 0.150 0.850 1.000 1.000 Player
3 -1 -260.0000 40.0000 0.0000 1.2000 3.0000 48.00 0.220 0.240 0.300 1.000 WallLeft
COMP 1 FPrimitiveRenderer2D
COMP 1 FRigidBody2D
COMP 2 FPrimitiveRenderer2D
COMP 2 FRigidBody2D
COMP 3 FPrimitiveRenderer2D
COMP 3 FRigidBody2D
CPROP 1 1 0 0.000 0.000 0.000 0.000
CPROP 2 1 0 1.000 0.000 0.000 0.000
CPROP 3 1 0 0.000 0.000 0.000 0.000
SEL -1 0
"@

$h = [EdT]::acs_editor_create()

[EdT]::acs_editor_scene_new($h)
$r = [EdT]::acs_editor_scene_load_text($h, $blank)
Check ($r -ne 0) "blank scene loaded (ret=$r)"
Check ([EdT]::acs_editor_node_count($h) -eq 0) "blank scene has 0 nodes"

[EdT]::acs_editor_scene_new($h)
$r = [EdT]::acs_editor_scene_load_text($h, $twod)
Check ($r -ne 0) "2D template scene loaded (ret=$r)"
Check ([EdT]::acs_editor_node_count($h) -eq 3) "2D template has 3 nodes"

$s = [EdT]::S([EdT]::acs_editor_scene_serialize($h))
$comp = @(($s -split "`n") | Where-Object { $_ -like "COMP *" })
$cpr  = @(($s -split "`n") | Where-Object { $_ -like "CPROP *" })
Check ($comp.Count -eq 6) "6 COMP lines round-tripped (got $($comp.Count))"
# bodyType: Ground/Wall static(0), Player dynamic(1)
Check (($s -split "`n") -contains "CPROP 1 1 0 0.0000 0.0000 0.0000 0.0000") "Ground bodyType=Static preserved"
Check (($s -split "`n") -contains "CPROP 2 1 0 1.0000 0.0000 0.0000 0.0000") "Player bodyType=Dynamic preserved"
Write-Host "  CPROP lines:"; $cpr | ForEach-Object { Write-Host "    $_" }

[EdT]::acs_editor_destroy($h)
if ($fail -eq 0) { Write-Host "`nALL PASS" } else { Write-Host "`n$fail FAILED" }
