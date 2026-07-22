# 3D subtree の serialize/paste と sprite clear を P/Invoke でヘッドレス検証する (GUI・マウス不要)。
#   - acs_editor_copy_subtree3d / acs_editor_paste_subtree3d  (Prefab/Blueprint 往復)
#   - acs_editor_node3d_clear_sprite
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
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_copy_subtree3d(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_paste_subtree3d(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int parent);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_parent(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_kind(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_component_count(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_clear_sprite(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_undo(IntPtr h);
    public static string Copy3D(IntPtr h, int id){
        IntPtr p = acs_editor_copy_subtree3d(h,id);
        if (p == IntPtr.Zero) return "";
        int n = 0; while (Marshal.ReadByte(p, n) != 0) n++;
        if (n == 0) return "";
        byte[] b = new byte[n]; Marshal.Copy(p, b, 0, n);
        return Encoding.UTF8.GetString(b);
    }
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

# Parent(10) -> Child(11, ARigidBody2D mass=5)。加えてルートに Sprite node(12, fake path)。
$scene = @"
ACS3D v2
N3D 10 -1 0 0.0000 0.0000 0.0000 0.0000 0.0000 0.0000 1.0000 1.0000 1.0000 0.800 0.800 0.850 1.000 Parent
N3D 11 10 1 1.0000 0.0000 0.0000 0.0000 0.0000 0.0000 1.0000 1.0000 1.0000 0.400 0.620 0.920 1.000 Child
CMP3D 11 ARigidBody2D
CPROP3D 11 0 3 5.0000 0.0000 0.0000 0.0000
N3D 12 -1 2 -3.0000 0.0000 0.0000 0.0000 0.0000 0.0000 1.0000 1.0000 1.0000 1.000 1.000 1.000 1.000 Spr
SPR3D 12 fake.png
"@

$h=[E]::acs_editor_create()
[E]::acs_editor_set_view3d($h,1)
Check "load ok" ([E]::acs_editor_scene3d_load_text($h,$scene) -ne 0)
Check "3 nodes loaded" ([E]::acs_editor_node3d_count($h) -eq 3)

Write-Host "`n[sprite clear] node 12 sprite -> plane"
Check "node12 kind=4 (Sprite)" ([E]::acs_editor_node3d_kind($h,12) -eq 4)
Check "clear_sprite returns 1" ([E]::acs_editor_node3d_clear_sprite($h,12) -eq 1)
Check "node12 kind=2 (Plane) after clear" ([E]::acs_editor_node3d_kind($h,12) -eq 2)
Check "clear_sprite on non-sprite (10) -> 0" ([E]::acs_editor_node3d_clear_sprite($h,10) -eq 0)

Write-Host "`n[copy_subtree3d] serialize subtree rooted at Parent(10)"
$txt = [E]::Copy3D($h,10)
Check "subtree has ACS3D header" ($txt -match "ACS3D")
Check "subtree root parent forced -1 (N3D 10 -1)" ($txt -match "N3D 10 -1 ")
Check "subtree child under 10 (N3D 11 10)" ($txt -match "N3D 11 10 ")
Check "subtree carries component (CMP3D 11 ARigidBody2D)" ($txt -match "CMP3D 11 ARigidBody2D")
Check "subtree carries prop (CPROP3D 11 0 3 5.0000)" ($txt -match "CPROP3D 11 0 3 5\.0000")
Check "subtree EXCLUDES out-of-tree sprite node 12" (-not ($txt -match "N3D 12 "))

Write-Host "`n[paste_subtree3d to root] new ids, hierarchy + component preserved"
$root = [E]::acs_editor_paste_subtree3d($h,$txt,-1)
Check "paste returns a new root id (>=13)" ($root -ge 13)
Check "now 5 nodes" ([E]::acs_editor_node3d_count($h) -eq 5)
Check "pasted root is at scene root (parent -1)" ([E]::acs_editor_node3d_parent($h,$root) -eq -1)
# child id = root + 1 (offset は subtree 内の id 間隔 10->root、11->root+1 を維持する)
$child = $root + 1
Check "pasted child parent = pasted root" ([E]::acs_editor_node3d_parent($h,$child) -eq $root)
Check "pasted child kept its component" ([E]::acs_editor_node3d_component_count($h,$child) -eq 1)

Write-Host "`n[paste_subtree3d under a parent] reparents root under target"
$root2 = [E]::acs_editor_paste_subtree3d($h,$txt,10)
Check "paste#2 returns new root" ($root2 -ge 13 -and $root2 -ne $root)
Check "now 7 nodes" ([E]::acs_editor_node3d_count($h) -eq 7)
Check "paste#2 root reparented under node 10" ([E]::acs_editor_node3d_parent($h,$root2) -eq 10)

Write-Host "`n[undo] last paste removed"
[void][E]::acs_editor_undo($h)
Check "back to 5 nodes after undo" ([E]::acs_editor_node3d_count($h) -eq 5)

[E]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
if($fail -gt 0){ exit 1 }
