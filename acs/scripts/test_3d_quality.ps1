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
}
"@
Add-Type -TypeDefinition $src

$pass=0;$fail=0
function Check($n,$c){ if($c){$script:pass++;Write-Host "  PASS  $n"} else {$script:fail++;Write-Host "  FAIL  $n" -ForegroundColor Red} }

$h=[E]::acs_editor_create()
[E]::acs_editor_settings_load_text($h,"")   # register builtin settings (ResetToDefaults) — headless has no attach

# expected: level -> (shadow size, bloom x100). Must match the ApplyQualityPreset table.
$cases = @(
  @("Ultra",   4096, 55),
  @("Highest", 4096, 50),
  @("High",    2048, 50),
  @("Medium",  2048, 40),
  @("Low",     1024, 30),
  @("Lowest",     0,  0)
)
foreach($c in $cases){
  $lvl=$c[0]; $expSize=[int]$c[1]; $expBloom=[int]$c[2]
  Check "settings_set QualityLevel=$lvl ok" ([E]::acs_editor_settings_set($h,"Rendering","QualityLevel",$lvl) -eq 1)
  Check "$lvl -> shadow size $expSize" ([E]::acs_editor_quality_shadow_size($h) -eq $expSize)
  Check "$lvl -> bloom x100 $expBloom" ([E]::acs_editor_quality_bloom_x100($h) -eq $expBloom)
}

Write-Host "`n[fallback] unknown level -> High preset"
# set High first (known-good), then attempt an invalid value; Enum may reject it. Either way shadows stay enabled.
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","High")
[void][E]::acs_editor_settings_set($h,"Rendering","QualityLevel","Bogus")
Check "unknown/invalid level keeps shadows enabled (size>0)" ([E]::acs_editor_quality_shadow_size($h) -gt 0)

[E]::acs_editor_destroy($h)
Write-Host "`n==== $pass passed, $fail failed ===="
if($fail -gt 0){ exit 1 }
