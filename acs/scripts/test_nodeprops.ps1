# ノード表示プロパティ (色/base/可視/有効/レイヤ) の get/set + serialize 往復 (NFLG) 検証。
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'editor_abi_test_support.ps1')

$result = Invoke-AcsEditorAbiTest -Configuration $Configuration -Body {
param($context)

$src = @"
using System; using System.Runtime.InteropServices;
public static class Np {
    const string D = @"$($context.CSharpNativeDll)";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_scene_serialize(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_scene_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_scene_new(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_add_node(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeName, int parentId);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_count(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_undo(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_get_color(IntPtr h, int id, out float r, out float g, out float b, out float a);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_set_color(IntPtr h, int id, float r, float g, float b, float a);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern float acs_editor_node_get_base(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_set_base(IntPtr h, int id, float b);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_get_visible(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_set_visible(IntPtr h, int id, int v);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_get_enabled(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_set_enabled(IntPtr h, int id, int v);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_node_get_sortlayer(IntPtr h, int id);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_node_set_sortlayer(IntPtr h, int id, int l);
    public static string S(IntPtr p){ if(p==IntPtr.Zero) return ""; int n=0; while(Marshal.ReadByte(p,n)!=0)n++; var b=new byte[n]; Marshal.Copy(p,b,0,n); return System.Text.Encoding.UTF8.GetString(b); }
}
"@
Add-Type -TypeDefinition $src

$script:pass=0;$script:fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

$h=[IntPtr]::Zero
$h2=[IntPtr]::Zero
try {
    $h=[Np]::acs_editor_create()
    if($h -eq [IntPtr]::Zero){ throw "acs_editor_create failed" }
    Check "new host starts empty" ([Np]::acs_editor_node_count($h) -eq 0)

    [Np]::acs_editor_scene_new($h)
    Check "new scene stays empty" ([Np]::acs_editor_node_count($h) -eq 0)

    $id=[Np]::acs_editor_add_node($h,"NodePropertyFixture",-1)
    if($id -lt 0){ throw "acs_editor_add_node failed" }
    Check "property fixture has one node" ([Np]::acs_editor_node_count($h) -eq 1)

    Write-Host "[get/set color]"
    [Np]::acs_editor_node_set_color($h,$id, 0.1,0.2,0.3,0.4)
    $r=0.0;$g=0.0;$b=0.0;$a=0.0; [Np]::acs_editor_node_get_color($h,$id,[ref]$r,[ref]$g,[ref]$b,[ref]$a)
    Check "color round-trips" ((([math]::Abs($r-0.1)) -lt 1e-4) -and (([math]::Abs($a-0.4)) -lt 1e-4))

    Write-Host "[get/set base + undo]"
    $base0=[Np]::acs_editor_node_get_base($h,$id)
    [Np]::acs_editor_node_set_base($h,$id,99.0)
    Check "base set" (([math]::Abs([Np]::acs_editor_node_get_base($h,$id)-99.0)) -lt 1e-3)
    [void][Np]::acs_editor_undo($h)
    Check "base undo restored" (([math]::Abs([Np]::acs_editor_node_get_base($h,$id)-$base0)) -lt 1e-3)
    [Np]::acs_editor_node_set_base($h,$id,99.0)   # 往復検証用の値へ戻す。

    Write-Host "[visible/enabled/sortlayer]"
    [Np]::acs_editor_node_set_visible($h,$id,0)
    [Np]::acs_editor_node_set_enabled($h,$id,0)
    [Np]::acs_editor_node_set_sortlayer($h,$id,7)
    Check "visible=0" ([Np]::acs_editor_node_get_visible($h,$id) -eq 0)
    Check "enabled=0" ([Np]::acs_editor_node_get_enabled($h,$id) -eq 0)
    Check "sortlayer=7" ([Np]::acs_editor_node_get_sortlayer($h,$id) -eq 7)

    Write-Host "[serialize -> NFLG present -> reload round-trip]"
    $txt=[Np]::S([Np]::acs_editor_scene_serialize($h))
    Check "NFLG line emitted" (($txt -split "`n") | Where-Object { $_ -eq "NFLG $id 0 0 7" })
    $h2=[Np]::acs_editor_create()
    if($h2 -eq [IntPtr]::Zero){ throw "second acs_editor_create failed" }
    Check "reload host starts empty" ([Np]::acs_editor_node_count($h2) -eq 0)
    Check "reload ok" ([Np]::acs_editor_scene_load_text($h2,$txt) -eq 1)
    Check "reload contains one node" ([Np]::acs_editor_node_count($h2) -eq 1)
    Check "reload visible=0" ([Np]::acs_editor_node_get_visible($h2,$id) -eq 0)
    Check "reload enabled=0" ([Np]::acs_editor_node_get_enabled($h2,$id) -eq 0)
    Check "reload sortlayer=7" ([Np]::acs_editor_node_get_sortlayer($h2,$id) -eq 7)
    Check "reload base=99" (([math]::Abs([Np]::acs_editor_node_get_base($h2,$id)-99.0)) -lt 1e-3)
    $r2=0.0;$g2=0.0;$b2=0.0;$a2=0.0; [Np]::acs_editor_node_get_color($h2,$id,[ref]$r2,[ref]$g2,[ref]$b2,[ref]$a2)
    Check "reload color" (([math]::Abs($r2-0.1)) -lt 1e-3)
}
finally {
    if($h2 -ne [IntPtr]::Zero){ [Np]::acs_editor_destroy($h2) }
    if($h -ne [IntPtr]::Zero){ [Np]::acs_editor_destroy($h) }
}

$expectedPass = 18
if (($script:pass + $script:fail) -ne $expectedPass) { throw "unexpected assertion count" }
Write-Host "`n==== $script:pass passed, $script:fail failed ===="
[pscustomobject]@{Pass=$script:pass;Fail=$script:fail;Expected=$expectedPass}
}

if($result.Fail -ne 0){ exit 1 }