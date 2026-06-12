# Verify the polygon tool produces POLY (collider) + RPLY (smooth render) lines,
# and that serialize -> scene_new -> load -> serialize is a stable round-trip.
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force -ErrorAction SilentlyContinue
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class EdP {
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

$h = [EdP]::acs_editor_create()
[EdP]::acs_editor_scene_new($h)

# Draw a 6-point ring -> smooth blob.
[EdP]::acs_editor_poly_begin($h)
$pts = @(@(700,210),@(820,290),@(820,420),@(700,500),@(580,420),@(580,290))
foreach ($p in $pts) { [EdP]::acs_editor_poly_add_point($h, [float]$p[0], [float]$p[1]) }
$nid = [EdP]::acs_editor_poly_finalize($h)
Check ($nid -gt 0) "finalize created node id=$nid"

$s1 = [EdP]::S([EdP]::acs_editor_scene_serialize($h))
$poly1 = @(($s1 -split "`n") | Where-Object { $_ -like "POLY *" })
$rply1 = @(($s1 -split "`n") | Where-Object { $_ -like "RPLY *" })
Check ($poly1.Count -eq 1) "exactly one POLY (collider) line"
Check ($rply1.Count -eq 1) "exactly one RPLY (smooth render) line"

# collider <= 8 verts, render >> collider (smooth).
$pc = (($poly1[0] -split '\s+')[2]) -as [int]
$rc = (($rply1[0] -split '\s+')[2]) -as [int]
Check ($pc -ge 3 -and $pc -le 8) "collider count $pc in [3,8]"
Check ($rc -ge 12) "render count $rc is smooth (>=12)"
Check ($rc -gt $pc) "render ($rc) finer than collider ($pc)"
Write-Host "  POLY: $($poly1[0].Substring(0,[Math]::Min(60,$poly1[0].Length)))..."
Write-Host "  RPLY: $($rply1[0].Substring(0,[Math]::Min(60,$rply1[0].Length)))..."

# Round-trip: new scene, load text, serialize again -> POLY/RPLY identical.
[EdP]::acs_editor_scene_new($h)
[void][EdP]::acs_editor_scene_load_text($h, $s1)
$s2 = [EdP]::S([EdP]::acs_editor_scene_serialize($h))
$poly2 = @(($s2 -split "`n") | Where-Object { $_ -like "POLY *" })
$rply2 = @(($s2 -split "`n") | Where-Object { $_ -like "RPLY *" })
Check ($poly2.Count -eq 1 -and $poly2[0] -eq $poly1[0]) "POLY identical after round-trip"
Check ($rply2.Count -eq 1 -and $rply2[0] -eq $rply1[0]) "RPLY identical after round-trip"

[EdP]::acs_editor_destroy($h)
if ($fail -eq 0) { Write-Host "`nALL PASS" } else { Write-Host "`n$fail FAILED" }
