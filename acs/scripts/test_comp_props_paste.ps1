# copy/paste (subtree, id 再マップ) と duplicate / remove-shift でコンポーネント・
# プロパティ値が保たれるか検証。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class EdCpp {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_add_component(IntPtr h, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_remove_component_at(IntPtr h, int id, int index);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_copy_subtree(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_paste_subtree(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int parent);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_duplicate(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_component_prop_get(IntPtr h, int id, int slot, int prop, out float x, out float y, out float z, out float w);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_component_prop_set(IntPtr h, int id, int slot, int prop, float x, float y, float z, float w);
    public static string S(IntPtr p){ if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; var b=new byte[n]; Marshal.Copy(p,b,0,n); return System.Text.Encoding.UTF8.GetString(b); }
}
"@
Add-Type -TypeDefinition $src

$h = [EdCpp]::acs_editor_create()
[EdCpp]::acs_editor_scene_new($h)
$id = [EdCpp]::acs_editor_add_node($h, "Src", -1)
[void][EdCpp]::acs_editor_node_add_component($h, $id, "FSprite2DComponent")
[void][EdCpp]::acs_editor_node_component_prop_set($h, $id, 0, 1, 0.1, 0.2, 0.3, 0.4)  # tint 非既定

$x=0.0;$y=0.0;$z=0.0;$w=0.0

# --- duplicate (CloneSubtree 経由) ---
$dup = [EdCpp]::acs_editor_node_duplicate($h, $id)
[void][EdCpp]::acs_editor_node_component_prop_get($h, $dup, 0, 1, [ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[duplicate] node {0} sprite.tint = ({1},{2},{3},{4}) (expect 0.1,0.2,0.3,0.4)" -f $dup,$x,$y,$z,$w)

# --- copy + paste (SerializeSubtree -> PasteSubtree, id 再マップ) ---
$clip = [EdCpp]::S([EdCpp]::acs_editor_copy_subtree($h, $id))
Write-Host "`n[clip] CPROP lines:"; ($clip -split "`n") | Where-Object { $_ -like "CPROP*" } | ForEach-Object { Write-Host "  $_" }
$pasted = [EdCpp]::acs_editor_paste_subtree($h, $clip, -1)
[void][EdCpp]::acs_editor_node_component_prop_get($h, $pasted, 0, 1, [ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("`n[paste] node {0} sprite.tint = ({1},{2},{3},{4}) (expect 0.1,0.2,0.3,0.4)" -f $pasted,$x,$y,$z,$w)

# --- remove-component shift: 2つ付けて先頭を外し、残りの値が繰り上がるか ---
$id2 = [EdCpp]::acs_editor_add_node($h, "Shift", -1)
[void][EdCpp]::acs_editor_node_add_component($h, $id2, "FSprite2DComponent")  # slot0
[void][EdCpp]::acs_editor_node_add_component($h, $id2, "FFire2DComponent")    # slot1
[void][EdCpp]::acs_editor_node_component_prop_set($h, $id2, 1, 1, 7.0, 0, 0, 0) # fire.intensity=7
[void][EdCpp]::acs_editor_node_remove_component_at($h, $id2, 0)               # slot0 を外す
[void][EdCpp]::acs_editor_node_component_prop_get($h, $id2, 0, 1, [ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("`n[remove-shift] node {0} slot0(now fire).intensity = ({1}) (expect 7)" -f $id2,$x)

[EdCpp]::acs_editor_destroy($h)
Write-Host "`nDONE"