# コンポーネント・プロパティ編集 ABI の P/Invoke 検証。
# スキーマ列挙 (count/name/kind)、既定値、set→serialize→reload の往復を確認する。
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
[Environment]::CurrentDirectory = $bin

$src = @"
using System;
using System.Runtime.InteropServices;
public static class EdProp {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_version();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t, int p);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_add_component(IntPtr h, int id, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_component_count(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_scene_serialize(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    // スキーマ
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_component_prop_count([MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_component_prop_name_at([MarshalAs(UnmanagedType.LPUTF8Str)] string t, int i);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_component_prop_kind_at([MarshalAs(UnmanagedType.LPUTF8Str)] string t, int i);
    // 値
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_component_prop_get(IntPtr h, int id, int slot, int prop, out float x, out float y, out float z, out float w);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_component_prop_set(IntPtr h, int id, int slot, int prop, float x, float y, float z, float w);
    public static string S(IntPtr p){
        if (p == IntPtr.Zero) return "";
        int len = 0; while (Marshal.ReadByte(p, len) != 0) len++;
        byte[] b = new byte[len]; Marshal.Copy(p, b, 0, len);
        return System.Text.Encoding.UTF8.GetString(b);
    }
}
"@
Add-Type -TypeDefinition $src -Language CSharp

$ver = [EdProp]::S([EdProp]::acs_editor_version())
Write-Host "ABI: $ver"

$h = [EdProp]::acs_editor_create()
if ($h -eq [IntPtr]::Zero) { throw "create failed" }

# --- ASprite2DComponent のスキーマ ---
$pc = [EdProp]::acs_editor_component_prop_count("ASprite2DComponent")
Write-Host "`n[schema] ASprite2DComponent prop_count = $pc (expect 3)"
for ($i=0; $i -lt $pc; $i++) {
    $nm = [EdProp]::S([EdProp]::acs_editor_component_prop_name_at("ASprite2DComponent",$i))
    $k  = [EdProp]::acs_editor_component_prop_kind_at("ASprite2DComponent",$i)
    Write-Host ("  [{0}] {1} kind={2}" -f $i,$nm,$k)
}
# 非コンポーネントは 0 件であること
$nc = [EdProp]::acs_editor_component_prop_count("FHealthSystem")
Write-Host "[schema] FHealthSystem prop_count = $nc (expect 0)"

# --- 付与して既定値を読み取る ---
[EdProp]::acs_editor_scene_new($h)
$id = [EdProp]::acs_editor_add_node($h, "Hero", -1)
[void][EdProp]::acs_editor_node_add_component($h, $id, "ASprite2DComponent")
[void][EdProp]::acs_editor_node_add_component($h, $id, "AFire2DComponent")
Write-Host "`n[attach] node $id comp_count = $([EdProp]::acs_editor_node_component_count($h,$id)) (expect 2)"

$x=0.0; $y=0.0; $z=0.0; $w=0.0
[void][EdProp]::acs_editor_node_component_prop_get($h,$id,0,0,[ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[default] sprite.size  = ({0},{1}) (expect 1,1)" -f $x,$y)
[void][EdProp]::acs_editor_node_component_prop_get($h,$id,0,1,[ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[default] sprite.tint  = ({0},{1},{2},{3}) (expect 1,1,1,1)" -f $x,$y,$z,$w)
[void][EdProp]::acs_editor_node_component_prop_get($h,$id,0,2,[ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[default] sprite.pivot = ({0},{1}) (expect 0.5,0.5)" -f $x,$y)
[void][EdProp]::acs_editor_node_component_prop_get($h,$id,1,1,[ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[default] fire.intensity = ({0}) (expect 1)" -f $x)

# --- 値を設定し、serialize・reload の往復を検証する ---
[void][EdProp]::acs_editor_node_component_prop_set($h,$id,0,1, 0.25, 0.5, 0.75, 0.9)   # tint
[void][EdProp]::acs_editor_node_component_prop_set($h,$id,1,0, 2.5, 4.0, 0, 0)          # fire.size
$txt = [EdProp]::S([EdProp]::acs_editor_scene_serialize($h))
Write-Host "`n[serialize] CPROP lines:"
($txt -split "`n") | Where-Object { $_ -like "CPROP*" } | ForEach-Object { Write-Host "  $_" }

# 新しい host へ再読み込みする
$h2 = [EdProp]::acs_editor_create()
$ok = [EdProp]::acs_editor_scene_load_text($h2, $txt)
Write-Host "`n[reload] load_text = $ok (expect 1)"
[void][EdProp]::acs_editor_node_component_prop_get($h2,$id,0,1,[ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[reload] sprite.tint = ({0},{1},{2},{3}) (expect 0.25,0.5,0.75,0.9)" -f $x,$y,$z,$w)
[void][EdProp]::acs_editor_node_component_prop_get($h2,$id,1,0,[ref]$x,[ref]$y,[ref]$z,[ref]$w)
Write-Host ("[reload] fire.size  = ({0},{1}) (expect 2.5,4)" -f $x,$y)

[EdProp]::acs_editor_destroy($h)
[EdProp]::acs_editor_destroy($h2)
Write-Host "`nDONE"
