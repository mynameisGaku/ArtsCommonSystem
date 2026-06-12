# Robustness: polygon finalize must not crash on degenerate input
# (2 points rejected, 3 points, collinear points, many points).
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class EdE {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_poly_begin(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_poly_add_point(IntPtr h, float sx, float sy);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_poly_finalize(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_scene_serialize(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    public static string S(IntPtr p){ if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; var b=new byte[n]; Marshal.Copy(p,b,0,n); return System.Text.Encoding.UTF8.GetString(b); }
}
"@
Add-Type -TypeDefinition $src
$fail = 0
function Check($cond, $msg) { if ($cond) { Write-Host "  OK  $msg" } else { Write-Host "  XX  $msg"; $script:fail++ } }
function Draw($h, $pts) { [EdE]::acs_editor_poly_begin($h); foreach ($p in $pts) { [EdE]::acs_editor_poly_add_point($h, [float]$p[0], [float]$p[1]) }; return [EdE]::acs_editor_poly_finalize($h) }

$h = [EdE]::acs_editor_create(); [EdE]::acs_editor_scene_new($h)

# 2 points -> rejected (-1), no crash.
$r = Draw $h @(@(100,100),@(200,200))
Check ($r -eq -1) "2 points rejected (returned -1)"

# 3 points (triangle) -> valid node.
$r = Draw $h @(@(300,100),@(400,300),@(200,300))
Check ($r -gt 0) "3 points -> node id=$r"

# collinear points -> must not crash; node may or may not get a collider but no AV.
$r = Draw $h @(@(100,100),@(200,100),@(300,100),@(400,100))
Check ($true) "collinear points survived (id=$r, no crash)"

# many points (40) on a circle -> capped, smooth.
$pts = @()
for ($i=0; $i -lt 40; $i++){ $a = $i * 0.157; $pts += ,@([int](500 + 150*[Math]::Cos($a)), [int](350 + 150*[Math]::Sin($a))) }
$r = Draw $h $pts
Check ($r -gt 0) "40 points -> node id=$r (capped)"

# Serialize whole scene and round-trip it -> no crash, RPLY count never exceeds 64.
$s1 = [EdE]::S([EdE]::acs_editor_scene_serialize($h))
$rplyMax = 0
foreach ($l in ($s1 -split "`n")) { if ($l -like "RPLY *") { $c = (($l -split '\s+')[2]) -as [int]; if ($c -gt $rplyMax) { $rplyMax = $c } } }
Check ($rplyMax -le 64) "max RPLY count $rplyMax <= 64 (render buffer cap)"
[EdE]::acs_editor_scene_new($h)
[void][EdE]::acs_editor_scene_load_text($h, $s1)
$s2 = [EdE]::S([EdE]::acs_editor_scene_serialize($h))
Check ($s2.Length -gt 0) "round-trip load+serialize survived"

[EdE]::acs_editor_destroy($h)
if ($fail -eq 0) { Write-Host "`nALL PASS" } else { Write-Host "`n$fail FAILED" }
