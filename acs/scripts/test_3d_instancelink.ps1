# Headless P/Invoke verification of 3D prefab/blueprint instance-link (no GUI, no mouse):
#   - acs_editor_node3d_set/get_prefab_src  (+ PFAB3D serialize round-trip)
#   - copy_subtree3d carries PFAB3D (C# StripPrefabLinks removes it for templates)
#   - ReinstantiateInstance3D sequence (get_transform -> delete -> paste3d -> set_transform -> set_prefab_src)
#   - FindPrefabInstances3D scan (count nodes sharing a prefab_src)
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
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_copy_subtree3d(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_paste_subtree3d(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int parent);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_set_prefab_src(IntPtr h, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_node3d_get_prefab_src(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_id_at(IntPtr h, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_parent(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_delete_node3d(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_get_transform(IntPtr h, int id, float[] o9);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node3d_set_transform(IntPtr h, int id, float px,float py,float pz, float rx,float ry,float rz, float sx,float sy,float sz);
    static string Z(IntPtr p){ if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; if(n==0) return ""; var b=new byte[n]; Marshal.Copy(p,b,0,n); return Encoding.UTF8.GetString(b); }
    public static string Prefab(IntPtr h,int id){ return Z(acs_editor_node3d_get_prefab_src(h,id)); }
    public static string Copy3D(IntPtr h,int id){ return Z(acs_editor_copy_subtree3d(h,id)); }
    public static string Serialize(IntPtr h){ var b=new byte[256*1024]; acs_editor_scene3d_serialize(h,b,b.Length); int n=Array.IndexOf(b,(byte)0); if(n<0)n=b.Length; return Encoding.UTF8.GetString(b,0,n); }
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

$scene = @"
ACS3D v2
N3D 1 -1 0 0.0000 0.0000 0.0000 0.0000 0.0000 0.0000 1.0000 1.0000 1.0000 0.800 0.800 0.850 1.000 Inst
N3D 2 -1 1 2.0000 0.0000 0.0000 0.0000 0.0000 0.0000 1.0000 1.0000 1.0000 0.400 0.620 0.920 1.000 Other
"@
$P = "C:/proj/Assets/foo.acsprefab"

$h=[E]::acs_editor_create()
[E]::acs_editor_set_view3d($h,1)
[void][E]::acs_editor_scene3d_load_text($h,$scene)

Write-Host "[set/get prefab_src]"
Check "set_prefab_src(1) ok" ([E]::acs_editor_node3d_set_prefab_src($h,1,$P) -eq 1)
Check "get_prefab_src(1) == path" ([E]::Prefab($h,1) -eq $P)
Check "get_prefab_src(2) == empty (not an instance)" ([E]::Prefab($h,2) -eq "")

Write-Host "`n[PFAB3D serialize round-trip]"
$txt=[E]::Serialize($h)
Check "serialize emits 'PFAB3D 1 <path>'" ($txt -match [regex]::Escape("PFAB3D 1 $P"))
[void][E]::acs_editor_scene3d_load_text($h,$txt)   # reload from serialized text
Check "prefab_src(1) persisted across save/load" ([E]::Prefab($h,1) -eq $P)

Write-Host "`n[copy_subtree3d carries PFAB3D] (C# StripPrefabLinks removes it for templates)"
$cp=[E]::Copy3D($h,1)
Check "subtree text contains PFAB3D" ($cp -match "PFAB3D")

Write-Host "`n[ReinstantiateInstance3D sequence] transform + link preserved"
[void][E]::acs_editor_node3d_set_transform($h,1, 5,6,7, 0,0,0, 2,2,2)
$t=New-Object 'single[]' 9
[void][E]::acs_editor_node3d_get_transform($h,1,$t)
$parent=[E]::acs_editor_node3d_parent($h,1)
[void][E]::acs_editor_delete_node3d($h,1)
$tmpl = "ACS3D v2`nN3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.8 0.8 0.85 1 Foo`n"
$nid=[E]::acs_editor_paste_subtree3d($h,$tmpl,$parent)
Check "reinstantiate paste ok (nid>=0)" ($nid -ge 0)
[void][E]::acs_editor_node3d_set_transform($h,$nid, $t[0],$t[1],$t[2], $t[3],$t[4],$t[5], $t[6],$t[7],$t[8])
[void][E]::acs_editor_node3d_set_prefab_src($h,$nid,$P)
$t2=New-Object 'single[]' 9
[void][E]::acs_editor_node3d_get_transform($h,$nid,$t2)
Check "new instance keeps transform pos=(5,6,7)" (([math]::Abs($t2[0]-5)) -lt 0.01 -and ([math]::Abs($t2[1]-6)) -lt 0.01 -and ([math]::Abs($t2[2]-7)) -lt 0.01)
Check "new instance keeps scale=(2,2,2)" (([math]::Abs($t2[6]-2)) -lt 0.01)
Check "new instance keeps prefab_src link" ([E]::Prefab($h,$nid) -eq $P)

Write-Host "`n[FindPrefabInstances3D scan] count nodes sharing prefab_src"
[void][E]::acs_editor_node3d_set_prefab_src($h,2,$P)   # make node 2 also an instance of same prefab
$cnt=[E]::acs_editor_node3d_count($h); $matches=0
for($i=0;$i -lt $cnt;$i++){ $nidi=[E]::acs_editor_node3d_id_at($h,$i); if([E]::Prefab($h,$nidi) -eq $P){ $matches++ } }
Check "2 nodes share the prefab link" ($matches -eq 2)

[E]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
if($fail -gt 0){ exit 1 }
