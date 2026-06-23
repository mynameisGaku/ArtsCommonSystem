# Headless P/Invoke verification of graphics quality presets (no GUI, no mouse).
#   - acs_editor_settings_set(Rendering/QualityLevel) -> ApplyQualityPreset wires the render knobs
#   - acs_editor_quality_shadow_size / _bloom_x100 reflect the active preset (these now drive the renderer:
#     shadow map resolution/bias/filter + bloom were previously dead knobs).
$ErrorActionPreference = 'Stop'
$bin = "C:\dev\acs_github\acs\editor\AcsEditor\bin\Release\net10.0-windows\win-x64"
Copy-Item "C:\dev\acs_github\acs\Binaries\Release\acs_editor_abi.dll" $bin -Force
[Environment]::CurrentDirectory = $bin

$src = @"
using System; using System.Runtime.InteropServices;
public static class E {
    const string D = "acs_editor_abi";
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern IntPtr acs_editor_create();
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_destroy(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_settings_load_text(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string t);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_settings_set(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string c, [MarshalAs(UnmanagedType.LPUTF8Str)] string k, [MarshalAs(UnmanagedType.LPUTF8Str)] string v);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_shadow_size(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_bloom_x100(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_exposure_x100(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_shadow_cascades(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_sun_direction(IntPtr h, float[] o3);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_sun_light_color(IntPtr h, float[] o3);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_sky_colors(IntPtr h, float[] o9);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_apply_lighting_preset(IntPtr h, [MarshalAs(UnmanagedType.LPUTF8Str)] string n);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern void acs_editor_set_show_grid3d(IntPtr h, int on);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_get_show_grid3d(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_ssao_x100(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_ssr_x100(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_ssgi_x100(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_taa(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_tonemap(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_auto_exposure(IntPtr h);
    [DllImport(D, CallingConvention=CallingConvention.Cdecl)] public static extern int acs_editor_quality_fog_x1000(IntPtr h);
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

$h=[E]::acs_editor_create()
[E]::acs_editor_settings_load_text($h,"")   # register builtin settings (ResetToDefaults) — headless has no attach

# expected: level -> (shadow size, bloom x100, cascades). Must match the ApplyQualityPreset table.
# CSM (cascades>=2) is Ultra/Highest only; default High stays single-cascade for safety.
$cases = @(
  @("Ultra",   4096, 55, 4),
  @("Highest", 4096, 50, 3),
  @("High",    2048, 50, 1),
  @("Medium",  2048, 40, 1),
  @("Low",     1024, 30, 1),
  @("Lowest",     0,  0, 0)
)
foreach($c in $cases){
  $lvl=$c[0]; $expSize=[int]$c[1]; $expBloom=[int]$c[2]; $expCasc=[int]$c[3]
  Check "settings_set QualityLevel=$lvl ok" ([E]::acs_editor_settings_set($h,"Rendering","QualityLevel",$lvl) -eq 1)
  Check "$lvl -> shadow size $expSize" ([E]::acs_editor_quality_shadow_size($h) -eq $expSize)
  Check "$lvl -> bloom x100 $expBloom" ([E]::acs_editor_quality_bloom_x100($h) -eq $expBloom)
  Check "$lvl -> shadow cascades $expCasc" ([E]::acs_editor_quality_shadow_cascades($h) -eq $expCasc)
}

Write-Host "`n[fallback] unknown level -> High preset"
# set High first (known-good), then attempt an invalid value; Enum may reject it. Either way shadows stay enabled.
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","High")
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","Bogus")
Check "unknown/invalid level keeps shadows enabled (size>0)" ([E]::acs_editor_quality_shadow_size($h) -gt 0)

Write-Host "`n[per-key overrides] individual Rendering keys take precedence over preset"
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","High")
Check "default exposure = 1.05 (x100=105)" ([E]::acs_editor_quality_exposure_x100($h) -eq 105)
Check "set Exposure=2.0 -> x100=200" (([E]::acs_editor_settings_set($h,"Rendering","Exposure","2.0") -eq 1) -and ([E]::acs_editor_quality_exposure_x100($h) -eq 200))
Check "High preset bloom = 50 (before override)" ([E]::acs_editor_quality_bloom_x100($h) -eq 50)
Check "BloomIntensity=0.25 overrides preset -> 25" (([E]::acs_editor_settings_set($h,"Rendering","BloomIntensity","0.25") -eq 1) -and ([E]::acs_editor_quality_bloom_x100($h) -eq 25))
Check "BloomIntensity=0 turns bloom off -> 0" (([E]::acs_editor_settings_set($h,"Rendering","BloomIntensity","0") -eq 1) -and ([E]::acs_editor_quality_bloom_x100($h) -eq 0))
Check "BloomIntensity=-1 falls back to preset -> 50" (([E]::acs_editor_settings_set($h,"Rendering","BloomIntensity","-1") -eq 1) -and ([E]::acs_editor_quality_bloom_x100($h) -eq 50))

Write-Host "`n[sun direction] azimuth/elevation -> unit light vector (drives shadow/shading/sky)"
function SunDir(){ $o=New-Object 'single[]' 3; [E]::acs_editor_sun_direction($h,$o); return $o }
[void][E]::acs_editor_settings_load_text($h,"")   # defaults: az=-41, el=58 -> ~ (0.40, 0.85, -0.35)
$d = SunDir
Check "default sun ~ (0.40,0.85,-0.35)" (([math]::Abs($d[0]-0.40) -lt 0.03) -and ([math]::Abs($d[1]-0.85) -lt 0.03) -and ([math]::Abs($d[2]+0.35) -lt 0.03))
[void][E]::acs_editor_settings_set($h,"Rendering","SunElevation","90")
$d = SunDir
Check "elevation 90 -> straight up (0,1,0)" (([math]::Abs($d[0]) -lt 0.02) -and ([math]::Abs($d[1]-1.0) -lt 0.02) -and ([math]::Abs($d[2]) -lt 0.02))
[void][E]::acs_editor_settings_set($h,"Rendering","SunElevation","0")
[void][E]::acs_editor_settings_set($h,"Rendering","SunAzimuth","0")
$d = SunDir
Check "elevation 0 / azimuth 0 -> (1,0,0)" (([math]::Abs($d[0]-1.0) -lt 0.02) -and ([math]::Abs($d[1]) -lt 0.02) -and ([math]::Abs($d[2]) -lt 0.02))

Write-Host "`n[sun color/intensity] effective light color = SunColor * SunIntensity"
function SunCol(){ $o=New-Object 'single[]' 3; [E]::acs_editor_sun_light_color($h,$o); return $o }
[void][E]::acs_editor_settings_load_text($h,"")   # defaults: color (1,0.95,0.85) * 2.35
$c = SunCol
Check "default light color ~ (2.35,2.23,2.00)" (([math]::Abs($c[0]-2.35) -lt 0.03) -and ([math]::Abs($c[1]-2.23) -lt 0.03) -and ([math]::Abs($c[2]-2.00) -lt 0.03))
[void][E]::acs_editor_settings_set($h,"Rendering","SunIntensity","4.0")
$c = SunCol
Check "intensity 4.0 -> (4.0, 3.8, 3.4)" (([math]::Abs($c[0]-4.0) -lt 0.03) -and ([math]::Abs($c[1]-3.8) -lt 0.03) -and ([math]::Abs($c[2]-3.4) -lt 0.03))
[void][E]::acs_editor_settings_set($h,"Rendering","SunColor","1,0,0")
[void][E]::acs_editor_settings_set($h,"Rendering","SunIntensity","2")
$c = SunCol
Check "pure red x2 -> (2,0,0)" (([math]::Abs($c[0]-2.0) -lt 0.03) -and ([math]::Abs($c[1]) -lt 0.03) -and ([math]::Abs($c[2]) -lt 0.03))

Write-Host "`n[sky colors] zenith/horizon/ground drive sky background + IBL gradient"
function Sky(){ $o=New-Object 'single[]' 9; [E]::acs_editor_sky_colors($h,$o); return $o }
[void][E]::acs_editor_settings_load_text($h,"")   # defaults
$s = Sky
Check "default zenith (0.16,0.33,0.62)" (([math]::Abs($s[0]-0.16) -lt 0.01) -and ([math]::Abs($s[1]-0.33) -lt 0.01) -and ([math]::Abs($s[2]-0.62) -lt 0.01))
Check "default ground (0.20,0.19,0.21)"  (([math]::Abs($s[6]-0.20) -lt 0.01) -and ([math]::Abs($s[7]-0.19) -lt 0.01) -and ([math]::Abs($s[8]-0.21) -lt 0.01))
[void][E]::acs_editor_settings_set($h,"Rendering","SkyZenith","1,0.5,0.2")   # sunset-ish orange
$s = Sky
Check "SkyZenith=1,0.5,0.2 applied" (([math]::Abs($s[0]-1.0) -lt 0.01) -and ([math]::Abs($s[1]-0.5) -lt 0.01) -and ([math]::Abs($s[2]-0.2) -lt 0.01))

Write-Host "`n[lighting presets] one-click time-of-day (sun + sky + exposure bundles)"
Check "apply Sunset ok" ([E]::acs_editor_apply_lighting_preset($h,"Sunset") -eq 1)
$d = SunDir; $c = SunCol; $s = Sky
Check "Sunset: sun low (y < 0.25)" ($d[1] -lt 0.25)
Check "Sunset: warm light (R > G > B)" (($c[0] -gt $c[1]) -and ($c[1] -gt $c[2]))
Check "Sunset: warm horizon (R high ~0.95)" ([math]::Abs($s[3]-0.95) -lt 0.02)
Check "Sunset: exposure 1.15" ([E]::acs_editor_quality_exposure_x100($h) -eq 115)
Check "apply Night ok" ([E]::acs_editor_apply_lighting_preset($h,"Night") -eq 1)
$c = SunCol; $s = Sky
Check "Night: cool dim light (B > R, dim)" (($c[2] -gt $c[0]) -and ($c[2] -lt 1.0))
Check "Night: dark sky zenith (all < 0.2)" (($s[0] -lt 0.2) -and ($s[1] -lt 0.2) -and ($s[2] -lt 0.2))
Check "apply Noon restores default sun ~0.85 up" ((& { [void][E]::acs_editor_apply_lighting_preset($h,"Noon"); (SunDir)[1] -gt 0.8 }))
Check "unknown preset -> 0" ([E]::acs_editor_apply_lighting_preset($h,"Bogus") -eq 0)

Write-Host "`n[grid toggle] 3D viewport grid show/hide"
Check "grid shown by default" ([E]::acs_editor_get_show_grid3d($h) -eq 1)
[E]::acs_editor_set_show_grid3d($h,0)
Check "grid hidden after set 0" ([E]::acs_editor_get_show_grid3d($h) -eq 0)
[E]::acs_editor_set_show_grid3d($h,1)
Check "grid shown after set 1" ([E]::acs_editor_get_show_grid3d($h) -eq 1)

Write-Host "`n[SSAO intensity override] verified working in-engine (gfx_ao_compare.png); knob wiring here"
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","High")
[void][E]::acs_editor_settings_set($h,"Rendering","SsaoIntensity","-1")
Check "High preset SSAO on (~1.0 => x100=100)" ([E]::acs_editor_quality_ssao_x100($h) -eq 100)
Check "SsaoIntensity=0 turns SSAO off" (([E]::acs_editor_settings_set($h,"Rendering","SsaoIntensity","0") -eq 1) -and ([E]::acs_editor_quality_ssao_x100($h) -eq 0))
Check "SsaoIntensity=2.5 override -> 250" (([E]::acs_editor_settings_set($h,"Rendering","SsaoIntensity","2.5") -eq 1) -and ([E]::acs_editor_quality_ssao_x100($h) -eq 250))
Check "SsaoIntensity=-1 back to preset -> 100" (([E]::acs_editor_settings_set($h,"Rendering","SsaoIntensity","-1") -eq 1) -and ([E]::acs_editor_quality_ssao_x100($h) -eq 100))

Write-Host "`n[SSR intensity override] (engine FSsr; reflection visible only on glossy surfaces)"
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","High")
[void][E]::acs_editor_settings_set($h,"Rendering","SsrIntensity","-1")
Check "High preset SSR on (~0.8 => x100=80)" ([E]::acs_editor_quality_ssr_x100($h) -eq 80)
Check "SsrIntensity=0 turns SSR off" (([E]::acs_editor_settings_set($h,"Rendering","SsrIntensity","0") -eq 1) -and ([E]::acs_editor_quality_ssr_x100($h) -eq 0))
Check "SsrIntensity=1.5 override -> 150" (([E]::acs_editor_settings_set($h,"Rendering","SsrIntensity","1.5") -eq 1) -and ([E]::acs_editor_quality_ssr_x100($h) -eq 150))

Write-Host "`n[SSGI intensity override] (engine FSsgi; 1-bounce indirect, Diligent backend)"
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","Highest")
[void][E]::acs_editor_settings_set($h,"Rendering","SsgiIntensity","-1")
Check "Highest preset SSGI on (~1.0 => x100=100)" ([E]::acs_editor_quality_ssgi_x100($h) -eq 100)
Check "SsgiIntensity=0 turns SSGI off" (([E]::acs_editor_settings_set($h,"Rendering","SsgiIntensity","0") -eq 1) -and ([E]::acs_editor_quality_ssgi_x100($h) -eq 0))
Check "SsgiIntensity=2 override -> 200" (([E]::acs_editor_settings_set($h,"Rendering","SsgiIntensity","2") -eq 1) -and ([E]::acs_editor_quality_ssgi_x100($h) -eq 200))

Write-Host "`n[TAA temporal anti-aliasing] (Halton jitter + history blend)"
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","Highest")
[void][E]::acs_editor_settings_set($h,"Rendering","Taa","-1")
Check "Highest preset TAA on" ([E]::acs_editor_quality_taa($h) -eq 1)
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","High")
Check "High preset TAA off" ([E]::acs_editor_quality_taa($h) -eq 0)
Check "Taa=1 override forces on" (([E]::acs_editor_settings_set($h,"Rendering","Taa","1") -eq 1) -and ([E]::acs_editor_quality_taa($h) -eq 1))
Check "Taa=0 override forces off" (([E]::acs_editor_settings_set($h,"Rendering","Taa","0") -eq 1) -and ([E]::acs_editor_quality_taa($h) -eq 0))

Write-Host "`n[Tonemap + auto-exposure]"
Check "default tonemap = ACES (0)" ([E]::acs_editor_quality_tonemap($h) -eq 0)
Check "Tonemap=1 -> AgX" (([E]::acs_editor_settings_set($h,"Rendering","Tonemap","1") -eq 1) -and ([E]::acs_editor_quality_tonemap($h) -eq 1))
Check "Tonemap=2 -> Reinhard" (([E]::acs_editor_settings_set($h,"Rendering","Tonemap","2") -eq 1) -and ([E]::acs_editor_quality_tonemap($h) -eq 2))
Check "AutoExposure=1 -> on" (([E]::acs_editor_settings_set($h,"Rendering","AutoExposure","1") -eq 1) -and ([E]::acs_editor_quality_auto_exposure($h) -eq 1))
Check "AutoExposure=0 -> off" (([E]::acs_editor_settings_set($h,"Rendering","AutoExposure","0") -eq 1) -and ([E]::acs_editor_quality_auto_exposure($h) -eq 0))

Write-Host "`n[Volumetric height fog]"
Check "default fog off" ([E]::acs_editor_quality_fog_x1000($h) -eq 0)
Check "FogDensity=0.04 -> 40" (([E]::acs_editor_settings_set($h,"Rendering","FogDensity","0.04") -eq 1) -and ([E]::acs_editor_quality_fog_x1000($h) -eq 40))
Check "FogDensity=0 -> off" (([E]::acs_editor_settings_set($h,"Rendering","FogDensity","0") -eq 1) -and ([E]::acs_editor_quality_fog_x1000($h) -eq 0))

[E]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
if($fail -gt 0){ exit 1 }
