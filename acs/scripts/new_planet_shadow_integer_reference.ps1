#Requires -Version 7.2
using namespace System.Numerics
using namespace System.Globalization

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$OutputDirectory,
    [switch]$VerifyOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# 出力先は必ず呼出し側が指定する。省略時にrepoやスクリプト横へ生成しない。
$referenceRoot = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($OutputDirectory, $PWD.ProviderPath))
$scriptDirectory = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath($PSScriptRoot))
$acsDirectory = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath((Join-Path $scriptDirectory '..')))
$repositoryDirectory = [IO.Path]::TrimEndingDirectorySeparator([IO.Path]::GetFullPath((Join-Path $acsDirectory '..')))
$fixturePath = Join-Path $acsDirectory 'tests/planet_shadow_reference_cases.inl'
foreach ($protectedDirectory in @($scriptDirectory, $acsDirectory, $repositoryDirectory)) {
    if ([StringComparer]::OrdinalIgnoreCase.Equals($referenceRoot, $protectedDirectory)) {
        throw 'OutputDirectory must be an explicit validation directory, not the repository root, acs root or scripts directory.'
    }
}
if ([IO.File]::Exists($referenceRoot)) { throw 'OutputDirectory names a file instead of a directory.' }
# 独立参照の内容を変更した場合、既存の正本を黙って置き換えず停止する。
$canonicalJsonSha256 = '72156E6B48A35455EBAF056E77F3C76A4E093C9DD9D0036D5D5BD77A511CB549'
$jsonPath = Join-Path $referenceRoot 'planet-shadow-reference.json'
$readmePath = Join-Path $referenceRoot 'README.md'
$utf8 = [Text.UTF8Encoding]::new($false)
$radiusBits = [uint32]0x45C6C000u
$checks = [Collections.Generic.List[string]]::new()

# 条件が偽なら成果を書き出さない。成功した検査名は成果に記録する。
function Assert-ReferenceCondition([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "CPU self-test failed: $Name" }
    [void]$checks.Add($Name)
}

# 2の整数乗を.NETの任意精度整数で作る。GPUの桁処理は使用しない。
function Get-PowerOfTwo([int]$Exponent) {
    return [BigInteger]::Pow([BigInteger]2, $Exponent)
}

# ケース記述用の定数だけをbinary32にする。参照多項式には浮動演算を使わない。
function Get-FloatBits([single]$Value) {
    return [BitConverter]::ToUInt32([BitConverter]::GetBytes($Value), 0)
}

# IEEE binary32を2^-149単位の符号付き整数へ写す。NaN/Infには値を与えない。
function ConvertFrom-FloatBitsToInteger([uint32]$Bits) {
    $exponent = [int](($Bits -shr 23) -band 255)
    if ($exponent -eq 255) {
        throw [ArgumentOutOfRangeException]::new('Bits', 'exponent=255: NaN/Infinity is outside this reference domain.')
    }
    $fraction = [uint32]($Bits -band 0x007FFFFFu)
    if ($exponent -eq 0) {
        $magnitude = [BigInteger]$fraction
    }
    else {
        $mantissa = [BigInteger]($fraction -bor 0x00800000u)
        $magnitude = $mantissa * (Get-PowerOfTwo ($exponent - 1))
    }
    if (($Bits -band 0x80000000u) -ne 0) { return -$magnitude }
    return $magnitude
}

# 多項式の全項を組込みBigIntegerで評価する。距離計算や方向正規化は行わない。
function Get-PlanetReference([uint32[]]$PBits, [uint32[]]$DirBits) {
    if ($PBits.Count -ne 3 -or $DirBits.Count -ne 3) { throw 'Each vector must contain exactly three uint32 bit patterns.' }
    $p = @($PBits | ForEach-Object { ConvertFrom-FloatBitsToInteger $_ })
    $direction = @($DirBits | ForEach-Object { ConvertFrom-FloatBitsToInteger $_ })
    if ($direction[0].IsZero -and $direction[1].IsZero -and $direction[2].IsZero) {
        throw [ArgumentException]::new('zero-direction: the nonzero ray-direction contract is required.')
    }
    $radius = ConvertFrom-FloatBitsToInteger $radiusBits
    $a = [BigInteger]::Zero
    $b = [BigInteger]::Zero
    $c = -($radius * $radius)
    for ($axis = 0; $axis -lt 3; $axis++) {
        $a += $direction[$axis] * $direction[$axis]
        $b += $p[$axis] * $direction[$axis]
        $c += $p[$axis] * $p[$axis]
    }
    $discriminant = $b * $b - $a * $c
    $blocked = ($c.Sign -lt 0) -or (($b.Sign -lt 0) -and ($discriminant.Sign -gt 0))
    return [pscustomobject]@{ A = $a; B = $b; C = $c; D = $discriminant; Blocked = [bool]$blocked }
}

# 診断用ビット長は二進文字列やGPUの16bit桁に依存せず、整数除算で数える。
function Get-IntegerBitLength([BigInteger]$Value) {
    $magnitude = [BigInteger]::Abs($Value)
    $length = 0
    while (-not $magnitude.IsZero) {
        $magnitude = [BigInteger]::Divide($magnitude, [BigInteger]2)
        $length++
    }
    return $length
}

# 指数掃引用のビット列を直接作る。指数255は呼出し側で生成しない。
function New-FiniteBits([int]$Exponent, [uint32]$Fraction, [bool]$Negative) {
    if ($Exponent -lt 0 -or $Exponent -gt 254 -or $Fraction -gt 0x007FFFFFu) { throw 'Invalid finite bit-pattern parameters.' }
    $bits = [uint32](($Exponent -shl 23) -bor $Fraction)
    if ($Negative) { $bits = [uint32]($bits -bor 0x80000000u) }
    return $bits
}

# JSONはuint32の数値配列を入力の正本とし、16進表記も人の照合用に添える。
function Get-HexVector([uint32[]]$Bits) {
    return @($Bits | ForEach-Object { '0x' + $_.ToString('X8', [CultureInfo]::InvariantCulture) })
}

$finiteCases = [Collections.Generic.List[object]]::new()
function Add-FiniteCase([string]$Id, [uint32[]]$PBits, [uint32[]]$DirBits, [string[]]$Tags, [Nullable[bool]]$AnalyticExpected, [string]$AnalyticReason) {
    $exact = Get-PlanetReference $PBits $DirBits
    if ($null -ne $AnalyticExpected) {
        Assert-ReferenceCondition ($exact.Blocked -eq $AnalyticExpected) ("analytic/$Id")
    }
    $entry = [ordered]@{
        id = $Id
        pBits = @($PBits)
        dirBits = @($DirBits)
        pHex = @(Get-HexVector $PBits)
        dirHex = @(Get-HexVector $DirBits)
        expectedBlocked = $exact.Blocked
        exactSigns = [ordered]@{ a = $exact.A.Sign; b = $exact.B.Sign; c = $exact.C.Sign; discriminant = $exact.D.Sign }
        integerBitLengths = [ordered]@{ a = (Get-IntegerBitLength $exact.A); b = (Get-IntegerBitLength $exact.B); c = (Get-IntegerBitLength $exact.C); discriminant = (Get-IntegerBitLength $exact.D) }
        tags = @($Tags)
        analyticExpected = $AnalyticExpected
        analyticReason = $AnalyticReason
    }
    [void]$finiteCases.Add($entry)
}

# 変換の独立した既知値。有限最大値、normal境界、subnormal、負の0を含む。
$unit = Get-PowerOfTwo 149
$maxInteger = ((Get-PowerOfTwo 24) - [BigInteger]::One) * (Get-PowerOfTwo 253)
$conversionChecks = @(
    @{ Bits = [uint32]0; Value = [BigInteger]::Zero; Name = 'positive-zero' },
    @{ Bits = [uint32]0x80000000u; Value = [BigInteger]::Zero; Name = 'negative-zero' },
    @{ Bits = [uint32]1; Value = [BigInteger]::One; Name = 'minimum-subnormal' },
    @{ Bits = [uint32]0x80000001u; Value = -[BigInteger]::One; Name = 'negative-minimum-subnormal' },
    @{ Bits = [uint32]0x007FFFFFu; Value = [BigInteger]8388607; Name = 'maximum-subnormal' },
    @{ Bits = [uint32]0x00800000u; Value = [BigInteger]8388608; Name = 'minimum-normal' },
    @{ Bits = [uint32]0x3F800000u; Value = $unit; Name = 'one' },
    @{ Bits = $radiusBits; Value = [BigInteger]6360 * $unit; Name = 'radius-6360' },
    @{ Bits = [uint32]0x7F7FFFFFu; Value = $maxInteger; Name = 'maximum-finite' },
    @{ Bits = [uint32]0xFF7FFFFFu; Value = -$maxInteger; Name = 'negative-maximum-finite' }
)
foreach ($check in $conversionChecks) {
    Assert-ReferenceCondition ((ConvertFrom-FloatBitsToInteger $check.Bits) -eq $check.Value) ('decode/' + $check.Name)
}

# binary32のnormal指数を1上げると同じ仮数の値が2倍になることを全指数で検査する。
for ($exponent = 1; $exponent -lt 254; $exponent++) {
    $lower = ConvertFrom-FloatBitsToInteger (New-FiniteBits $exponent 0x00555555u $false)
    $upper = ConvertFrom-FloatBitsToInteger (New-FiniteBits ($exponent + 1) 0x00555555u $false)
    Assert-ReferenceCondition ($upper -eq ([BigInteger]2 * $lower)) ('decode/exponent-step-' + $exponent)
}

# 幾何学から答えが決まる35条件。期待値は参照関数から導かない。
$one = [uint32]0x3F800000u
$minusOne = [uint32]0xBF800000u
$negativeZero = [uint32]0x80000000u
$minimum = [uint32]1
$negativeMinimum = [uint32]0x80000001u
$maximum = [uint32]0x7F7FFFFFu
$negativeMaximum = [uint32]0xFF7FFFFFu
$inside = Get-FloatBits 6359
$outside = Get-FloatBits 6361
$tangentX = Get-FloatBits 564.46881103515625
$six = Get-FloatBits 6
$r5088 = Get-FloatBits 5088
$r3816 = Get-FloatBits 3816

Add-FiniteCase 'center-plus-x' @(0,0,0) @($one,0,0) @('analytic','interior') $true 'The starting point is the sphere center, hence it is already inside.'
Add-FiniteCase 'interior-outward' @(0,$inside,0) @(0,$one,0) @('analytic','interior','outward') $true 'Radius 6359 is strictly below B=6360, even though the ray points outward.'
Add-FiniteCase 'interior-negative-axis' @([uint32]($inside -bor 0x80000000u),0,0) @($minusOne,0,0) @('analytic','interior','negative-axis') $true 'The starting radius is 6359; reflecting the axis does not remove the initial interior segment.'
Add-FiniteCase 'surface-outward' @(0,$radiusBits,0) @(0,$one,0) @('analytic','surface','outward') $false 'For t>0 the positive y coordinate increases beyond B.'
Add-FiniteCase 'surface-inward' @(0,$radiusBits,0) @(0,$minusOne,0) @('analytic','surface','inward') $true 'For sufficiently small t>0 the y coordinate is strictly between 0 and B.'
Add-FiniteCase 'surface-tangent' @(0,$radiusBits,0) @($one,0,0) @('analytic','surface','tangent') $false 'The path remains in plane y=B; squared radius is B^2+t^2.'
Add-FiniteCase 'surface-inward-normal-tiny' @(0,$radiusBits,0) @($one,[uint32]0x97800000u,0) @('analytic','normal','intermediate-ftz-counterexample') $true 'The radial derivative at t=0 is negative because dy=-2^-80; a positive interior interval exists.'
Add-FiniteCase 'surface-inward-subnormal' @(0,$radiusBits,0) @($one,$negativeMinimum,0) @('analytic','subnormal','inward') $true 'Even dy=-2^-149 gives a negative radial derivative and a nonempty interior interval.'
Add-FiniteCase 'surface-outward-subnormal' @(0,$radiusBits,0) @($one,$minimum,0) @('analytic','subnormal','outward') $false 'Both the y increment and x^2 increase squared distance from the sphere center.'
Add-FiniteCase 'surface-minus-zero-tangent' @($negativeZero,$radiusBits,0) @($one,$negativeZero,$negativeZero) @('analytic','signed-zero','tangent') $false 'Signed zero does not change the path in plane y=B.'
Add-FiniteCase 'exterior-inward' @(0,$outside,0) @(0,$minusOne,0) @('analytic','exterior','inward') $true 'The positive half-ray runs through the sphere center.'
Add-FiniteCase 'exterior-away' @(0,$outside,0) @(0,$one,0) @('analytic','exterior','outward') $false 'The initial y=6361 increases for every positive t.'
Add-FiniteCase 'exterior-tangent-direction' @(0,$outside,0) @($one,0,0) @('analytic','exterior','perpendicular') $false 'The path stays in plane y=6361, strictly outside the sphere.'
Add-FiniteCase 'tangent-plane' @($tangentX,$radiusBits,0) @($minusOne,0,0) @('analytic','tangent','boundary') $false 'Plane y=B only touches the sphere, regardless of the x coordinate.'
Add-FiniteCase 'tangent-plane-one-ulp-inside' @($tangentX,([uint32]($radiusBits-1)),0) @($minusOne,0,0) @('analytic','boundary','adjacent-float') $true 'At positive t=x the point has x=0 and y immediately below B.'
Add-FiniteCase 'tangent-plane-one-ulp-outside' @($tangentX,([uint32]($radiusBits+1)),0) @($minusOne,0,0) @('analytic','boundary','adjacent-float') $false 'The constant y coordinate is immediately above B.'
Add-FiniteCase 'rotated-exact-tangent' @($tangentX,$r5088,$r3816) @($minusOne,0,0) @('analytic','tangent','prior-gpu-counterexample') $false '5088^2+3816^2=6360^2 exactly; changing x cannot enter the sphere.'
Add-FiniteCase 'rotated-tangent-axis-swap' @($r3816,$r5088,$tangentX) @(0,0,$minusOne) @('analytic','tangent','axis-permutation') $false 'Swap x and z in the preceding exact tangent, including the direction.'
Add-FiniteCase 'tangent-plane-oblique' @($one,$radiusBits,$six) @([uint32]0xBE285835u,0,[uint32]0xBF7C8450u) @('analytic','tangent-plane','prior-gpu-counterexample') $false 'The whole line remains in plane y=B; the rounded direction actually has D=-2^-50.'
Add-FiniteCase 'dot-cancellation-zero' @($radiusBits,$radiusBits,0) @($one,$minusOne,0) @('analytic','signed-dot-cancellation') $false 'The exterior starting point is perpendicular to the direction; distance only increases.'
Add-FiniteCase 'dot-cancellation-positive-residual' @($radiusBits,$radiusBits,$one) @($one,$minusOne,$one) @('analytic','signed-dot-cancellation') $false 'The large dot terms cancel, leaving positive b=1; the exterior half-ray points away.'
Add-FiniteCase 'dot-cancellation-negative-residual' @($radiusBits,$radiusBits,$one) @($one,$minusOne,$minusOne) @('analytic','signed-dot-cancellation') $false 'Here a=3, b=-1 and c=B^2+1, so the positive-time closest point is still outside: D=-3*B^2-2.'
Add-FiniteCase 'direction-scale-one' @($radiusBits,$one,0) @($minusOne,0,0) @('analytic','positive-direction-scaling') $true 'The ray reaches (0,1,0), strictly inside the sphere.'
Add-FiniteCase 'direction-scale-minimum-subnormal' @($radiusBits,$one,0) @($negativeMinimum,0,0) @('analytic','positive-direction-scaling','subnormal') $true 'Multiplying the direction by positive 2^-149 changes only the ray parameter, not its points.'
Add-FiniteCase 'direction-scale-minimum-normal' @($radiusBits,$one,0) @([uint32]0x80800000u,0,0) @('analytic','positive-direction-scaling','normal') $true 'Positive scaling by 2^-126 preserves the same half-ray.'
Add-FiniteCase 'direction-scale-large' @($radiusBits,$one,0) @([uint32]0xFF000000u,0,0) @('analytic','positive-direction-scaling','large-exponent') $true 'Positive scaling by 2^127 preserves the same half-ray.'
Add-FiniteCase 'maximum-toward-center' @($maximum,$maximum,$maximum) @($negativeMaximum,$negativeMaximum,$negativeMaximum) @('analytic','maximum-finite','large-coefficients') $true 'At t=1 the position is exactly the sphere center.'
Add-FiniteCase 'maximum-away' @($maximum,$maximum,$maximum) @($maximum,$maximum,$maximum) @('analytic','maximum-finite','outward') $false 'Every positive t scales the already exterior position by 1+t.'
Add-FiniteCase 'maximum-perpendicular' @($maximum,$maximum,0) @($maximum,$negativeMaximum,0) @('analytic','maximum-finite','signed-dot-cancellation') $false 'The dot product is zero and the squared radius starts above B^2.'
Add-FiniteCase 'maximum-plus-minimum-toward-plane' @($maximum,$minimum,0) @($negativeMaximum,0,0) @('analytic','maximum-finite','subnormal','exponent-spread') $true 'At t=1 only the minimum subnormal y coordinate remains, strictly inside.'
Add-FiniteCase 'subnormal-position-interior' @($minimum,$negativeMinimum,[uint32]0x007FFFFFu) @($one,0,0) @('analytic','subnormal','interior') $true 'All starting coordinates are far smaller than B; the start is interior.'
Add-FiniteCase 'long-borrow-then-carry-dot' @($maximum,$one,$one) @($negativeMaximum,$one,$minusOne) @('analytic','long-borrow','long-carry','signed-dot-cancellation') $true 'At t=1 the point is (0,2,0). Sequential dot magnitudes are M^2, M^2-S^2, M^2, crossing 208 one-bits; S=2^149.'
Add-FiniteCase 'maximum-dot-cancellation-residual' @($maximum,$maximum,$one) @($negativeMaximum,$maximum,$minusOne) @('analytic','maximum-finite','signed-dot-cancellation') $false 'For all t, x^2+y^2=2*maxfloat^2*(1+t^2)>B^2; cancellation leaves b=-1.'
Add-FiniteCase 'maximum-position-minimum-direction' @($maximum,0,0) @($negativeMinimum,0,0) @('analytic','maximum-finite','subnormal','exponent-spread') $true 'A negative nonzero direction along the positive x axis eventually reaches the origin; t need not fit a float.'
Add-FiniteCase 'subnormal-only-direction-away' @(0,$outside,0) @(0,$minimum,0) @('analytic','subnormal','outward') $false 'The nonzero positive y direction increases the initial exterior radius.'
Assert-ReferenceCondition ($finiteCases.Count -eq 35) 'case-count/analytic-35'

# 85条件の各3成分で指数欄0..254を位置と方向のそれぞれに網羅する。
$fractions = @([uint32]0, [uint32]1, [uint32]0x007FFFFFu, [uint32]0x007FFFFEu, [uint32]0x00555555u, [uint32]0x002AAAABu, [uint32]0x00400000u)
for ($group = 0; $group -lt 85; $group++) {
    $pBits = [uint32[]]::new(3)
    $dirBits = [uint32[]]::new(3)
    for ($axis = 0; $axis -lt 3; $axis++) {
        $exponent = 3*$group+$axis
        $pBits[$axis] = New-FiniteBits $exponent $fractions[($group+$axis)%$fractions.Count] (($group+$axis)%2 -eq 1)
        $dirBits[$axis] = New-FiniteBits (($exponent+127)%255) $fractions[($group+2*$axis+1)%$fractions.Count] (($group+2*$axis)%3 -ne 0)
    }
    Add-FiniteCase ('exponent-sweep-' + $group.ToString('D2')) $pBits $dirBits @('all-exponent-fields','deterministic-bit-patterns') $null 'Coverage case only: expectedBlocked is obtained from the independent BigInteger reference, not an analytic acceptance claim.'
}
Assert-ReferenceCondition ($finiteCases.Count -eq 120) 'case-count/finite-120'
Assert-ReferenceCondition (@($finiteCases.id | Select-Object -Unique).Count -eq 120) 'case-ids/unique'

$positionExponents = @($finiteCases | ForEach-Object { $_.pBits | ForEach-Object { [int](($_ -shr 23) -band 255) } } | Sort-Object -Unique)
$directionExponents = @($finiteCases | ForEach-Object { $_.dirBits | ForEach-Object { [int](($_ -shr 23) -band 255) } } | Sort-Object -Unique)
Assert-ReferenceCondition (($positionExponents -join ',') -eq ((0..254) -join ',')) 'coverage/position-exponent-fields-0-through-254'
Assert-ReferenceCondition (($directionExponents -join ',') -eq ((0..254) -join ',')) 'coverage/direction-exponent-fields-0-through-254'

# 固定点の倍率を使った既知の判別式と、長い借り・繰上がり条件を独立に確認する。
$oblique = Get-PlanetReference @($one,$radiusBits,$six) @([uint32]0xBE285835u,0,[uint32]0xBF7C8450u)
Assert-ReferenceCondition ($oblique.D -eq -(Get-PowerOfTwo 546)) 'analytic/oblique-discriminant-minus-2-to-minus-50'
$borrowMagnitude = $maxInteger*$maxInteger-$unit*$unit
$runMask = (Get-PowerOfTwo 208)-[BigInteger]::One
$run = [BigInteger]::Remainder([BigInteger]::Divide($borrowMagnitude, (Get-PowerOfTwo 298)), (Get-PowerOfTwo 208))
Assert-ReferenceCondition ($run -eq $runMask) 'stress/208-consecutive-one-bits-after-borrow'
Assert-ReferenceCondition (($borrowMagnitude+$unit*$unit) -eq $maxInteger*$maxInteger) 'stress/carry-through-the-208-bit-run'
$longDot = Get-PlanetReference @($maximum,$one,$one) @($negativeMaximum,$one,$minusOne)
Assert-ReferenceCondition ($longDot.B -eq -($maxInteger*$maxInteger)) 'stress/signed-dot-cancellation-with-long-carry'
Assert-ReferenceCondition ((Get-IntegerBitLength $maxInteger) -eq 277) 'bounds/input-277-bits'
Assert-ReferenceCondition ((Get-IntegerBitLength ([BigInteger]3*$maxInteger*$maxInteger)) -eq 556) 'bounds/coefficient-556-bits'
Assert-ReferenceCondition ((Get-IntegerBitLength ([BigInteger]18*[BigInteger]::Pow($maxInteger,4))) -eq 1113) 'bounds/conservative-discriminant-1113-bits'

# 軸の同時交換と中心反転は球の幾何を変えない。全120条件で確認する。
$permutations = @(@(0,1,2),@(0,2,1),@(1,0,2),@(1,2,0),@(2,0,1),@(2,1,0))
foreach ($case in $finiteCases) {
    foreach ($permutation in $permutations) {
        $permutedP = @($permutation | ForEach-Object { $case.pBits[$_] })
        $permutedDir = @($permutation | ForEach-Object { $case.dirBits[$_] })
        $permuted = Get-PlanetReference $permutedP $permutedDir
        Assert-ReferenceCondition ($permuted.Blocked -eq $case.expectedBlocked) ('invariant/axis-' + ($permutation -join '') + '/' + $case.id)
    }
    $reflectedP = @($case.pBits | ForEach-Object { [uint32]($_ -bxor 0x80000000u) })
    $reflectedDir = @($case.dirBits | ForEach-Object { [uint32]($_ -bxor 0x80000000u) })
    $reflected = Get-PlanetReference $reflectedP $reflectedDir
    Assert-ReferenceCondition ($reflected.Blocked -eq $case.expectedBlocked) ('invariant/center-reflection/' + $case.id)
    Assert-ReferenceCondition (($case.integerBitLengths.a -le 556) -and ($case.integerBitLengths.b -le 556) -and ($case.integerBitLengths.c -le 556) -and ($case.integerBitLengths.discriminant -le 1113)) ('bounds/case/' + $case.id)
}

# 非有限入力はfalseへ変換せず、別の拒否記録へ入れる。GPU判定の正解には数えない。
$rejectedInputs = [Collections.Generic.List[object]]::new()
$rejectionSpecs = @(
    @{ Id='position-positive-infinity'; P=@([uint32]0x7F800000u,0,0); Dir=@($one,0,0) },
    @{ Id='position-negative-infinity'; P=@(0,0,[uint32]0xFF800000u); Dir=@($one,0,0) },
    @{ Id='position-quiet-nan'; P=@(0,[uint32]0x7FC00001u,0); Dir=@($one,0,0) },
    @{ Id='position-signaling-nan'; P=@([uint32]0x7F800001u,0,0); Dir=@($one,0,0) },
    @{ Id='direction-positive-infinity'; P=@(0,$radiusBits,0); Dir=@([uint32]0x7F800000u,0,0) },
    @{ Id='direction-negative-infinity'; P=@(0,$radiusBits,0); Dir=@(0,[uint32]0xFF800000u,0) },
    @{ Id='direction-quiet-nan'; P=@(0,$radiusBits,0); Dir=@(0,0,[uint32]0xFFC00001u) },
    @{ Id='direction-signaling-nan'; P=@(0,$radiusBits,0); Dir=@([uint32]0x7F800001u,0,0) }
)
foreach ($spec in $rejectionSpecs) {
    $wasRejected = $false
    try { [void](Get-PlanetReference $spec.P $spec.Dir) }
    catch [ArgumentOutOfRangeException] { $wasRejected = $_.Exception.Message.Contains('exponent=255') }
    Assert-ReferenceCondition $wasRejected ('reject/' + $spec.Id)
    [void]$rejectedInputs.Add([ordered]@{ id=$spec.Id; pBits=@($spec.P); dirBits=@($spec.Dir); pHex=@(Get-HexVector $spec.P); dirHex=@(Get-HexVector $spec.Dir); status='rejected'; expectedBlocked=$null; reason='exponent=255: NaN/Infinity is outside the finite-input reference domain.' })
}
foreach ($zeroBits in @([uint32]0,$negativeZero)) {
    $wasRejected = $false
    try { [void](Get-PlanetReference @(0,$radiusBits,0) @($zeroBits,$zeroBits,$zeroBits)) }
    catch [ArgumentException] { $wasRejected = $_.Exception.Message.Contains('zero-direction') }
    Assert-ReferenceCondition $wasRejected ('reject/zero-direction-' + $zeroBits.ToString('X8'))
}
Assert-ReferenceCondition (($finiteCases.Count+$rejectedInputs.Count) -eq 128) 'case-count/total-128'

$document = [ordered]@{
    schemaVersion = 1
    purpose = 'Independent diagnostic CPU oracle for GPU planet-shadow input-bit tests.'
    radius = [ordered]@{ km=6360; bits=$radiusBits; hex=('0x'+$radiusBits.ToString('X8')) }
    arithmetic = [ordered]@{
        implementation = 'PowerShell / System.Numerics.BigInteger; no copied GPU limb arithmetic.'
        inputUnit = '2^-149'
        coefficientUnit = '2^-298'
        discriminantUnit = '2^-596'
        normalMapping = 'sign * (2^23 + fraction) * 2^(exponentField - 1)'
        subnormalMapping = 'sign * fraction'
        predicate = 'c < 0 || (b < 0 && b*b - a*c > 0)'
        tangentPolicy = 'D=0 does not enter the open sphere interior.'
        domain = 'Finite IEEE binary32 input bits, nonzero direction, fixed positive radius B=6360.'
        invalidPolicy = 'Reject exponentField=255 and zero direction; no expectedBlocked boolean is assigned to rejected rows.'
    }
    counts = [ordered]@{ finiteCases=120; analyticCases=35; exponentSweepCases=85; rejectedCases=8; totalCases=128 }
    coverage = [ordered]@{ positionExponentFields=$positionExponents; directionExponentFields=$directionExponents; allFiniteInputsExhaustivelyTested=$false }
    cpuSelfTests = [ordered]@{ passed=$checks.Count; failed=0; checks=@($checks.ToArray()) }
    gpuExecutions = 0
    limitations = @(
        'This finite deterministic corpus is not an exhaustive proof over all finite float inputs.'
        'The reference is exact for the supplied input bits; earlier floating-point rounding, FTZ, normalization and input transport are outside that claim.'
        'GPU shader compilation, execution, readback, performance and product acceptance were not performed.'
        'BigInteger and PowerShell/.NET are diagnostic-only; ACS product no-STL constraints are unchanged.'
    )
    finiteCases = @($finiteCases.ToArray())
    rejectedInputs = @($rejectedInputs.ToArray())
}
$json = (($document | ConvertTo-Json -Depth 16) -replace "`r`n", "`n") + "`n"
$jsonBytes = $utf8.GetBytes($json)
$jsonHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($jsonBytes))

# C++固定fixtureの120行×7列を照合する。コメントや空白以外の未知の初期化式は拒否する。
# fixtureの検査結果はJSONのCPU検査一覧へ混ぜず、正本JSONのバイト列を維持する。
function Test-PlanetReferenceFixture([string]$FixtureText, [object[]]$Cases) {
    $withoutComments = [regex]::Replace($FixtureText, '(?s)/\*.*?\*/|//[^\r\n]*', '')
    $declarationPattern = '(?s)\bstatic\s+constexpr\s+acs::u32\s+kPlanetShadowReferenceBits\s*\[\s*\]\s*\[\s*7\s*\]\s*=\s*\{(?<body>.*?)\}\s*;'
    $declarations = [regex]::Matches($withoutComments, $declarationPattern)
    if ($declarations.Count -ne 1) { throw 'Fixture must contain exactly one supported kPlanetShadowReferenceBits[][7] declaration.' }
    $body = $declarations[0].Groups['body'].Value
    $rows = [Collections.Generic.List[object]]::new()
    $cursor = 0
    $rowPattern = [regex]::new('\G\s*\{(?<values>[^{}]*)\}\s*(?<separator>,?)')
    while ($cursor -lt $body.Length) {
        if ([string]::IsNullOrWhiteSpace($body.Substring($cursor))) { break }
        $row = $rowPattern.Match($body, $cursor)
        if (-not $row.Success) { throw ('Unsupported fixture initializer near offset ' + $cursor) }
        $columns = $row.Groups['values'].Value.Split(',')
        if ($columns.Count -ne 7) { throw ('Fixture row ' + $rows.Count + ' does not have seven columns.') }
        $numbers = [uint32[]]::new(7)
        for ($column = 0; $column -lt 7; $column++) {
            $literal = [regex]::Match($columns[$column].Trim(), '\A0[xX](?<digits>[0-9a-fA-F]{1,8})[uU]\z')
            if (-not $literal.Success) { throw ('Unsupported uint32 literal in fixture row ' + $rows.Count + ', column ' + $column) }
            $numbers[$column] = [Convert]::ToUInt32($literal.Groups['digits'].Value, 16)
        }
        [void]$rows.Add($numbers)
        $cursor = $row.Index + $row.Length
        if ($row.Groups['separator'].Value.Length -eq 0 -and -not [string]::IsNullOrWhiteSpace($body.Substring($cursor))) {
            throw 'Fixture rows must be separated by commas.'
        }
    }
    if ($Cases.Count -ne 120 -or $rows.Count -ne 120) { throw ('Fixture must match exactly 120 finite cases; found ' + $rows.Count + ' rows.') }
    for ($rowIndex = 0; $rowIndex -lt 120; $rowIndex++) {
        $case = $Cases[$rowIndex]
        $expected = @($case.pBits) + @($case.dirBits) + @([uint32]([bool]$case.expectedBlocked))
        for ($column = 0; $column -lt 7; $column++) {
            if ($rows[$rowIndex][$column] -ne $expected[$column]) {
                throw ('Fixture mismatch: case ' + $rowIndex + ' (' + $case.id + '), column ' + $column + '; actual=0x' + $rows[$rowIndex][$column].ToString('X8') + ', expected=0x' + ([uint32]$expected[$column]).ToString('X8'))
            }
        }
    }
    return [pscustomobject]@{ rows = 120; columns = 7; constants = 840; matched = $true }
}

# 生成と検査の両モードでfixture一致を必須とし、不一致なら出力フォルダーも作らない。
if ($jsonHash -cne $canonicalJsonSha256) { throw 'Deterministic JSON has changed from the pinned independent reference SHA-256.' }
$fixtureBytes = [IO.File]::ReadAllBytes($fixturePath)
$fixtureVerification = Test-PlanetReferenceFixture ([Text.Encoding]::UTF8.GetString($fixtureBytes)) $finiteCases.ToArray()
$fixtureHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($fixtureBytes))
$scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash

# READMEとJSONのみを指定フォルダー内へ生成する。検査モードでは書き込まない。
$readme = @"
# 惑星遮蔽の独立整数参照

有限入力120件と非有限入力の拒否8件、合計128件の決定論的な診断データ。
半径B=6360km。GPU実行数は **0**。build・製品ソース編集・commit/push・cleanupは行わない。

## 再実行

PowerShell 7.2以降 / .NETで、repoの生成器へ出力先を明示する。OutputDirectoryは必須である。

    pwsh -NoLogo -NoProfile -File "$PSCommandPath" -OutputDirectory "$referenceRoot"
    pwsh -NoLogo -NoProfile -File "$PSCommandPath" -OutputDirectory "$referenceRoot" -VerifyOnly

通常実行は指定フォルダーのplanet-shadow-reference.jsonとREADME.mdだけを生成する。
生成前に全CPU検査・正本JSON SHA-256・repo fixture一致を確認する。
VerifyOnlyは既存JSONのバイト再現性とrepoのacs/tests/planet_shadow_reference_cases.inlの120×7定数一致を必須確認し、一切書き込まない。
VerifyOnlyはREADMEの生成環境表示の違いでは失敗させず、JSONとfixtureを検査する。
fixtureの対応形式はunsigned16進定数の120×7初期化子で、一般のC++式を評価するものではない。
日時や乱数源をJSONへ入れず、正本SHA-256が変わる出力は拒否する。
今回の実行環境: PowerShell $($PSVersionTable.PSVersion)、.NET $([Environment]::Version)。

## 証拠

- スクリプトSHA-256: $scriptHash
- JSON SHA-256: $jsonHash
- CPU自己検査: $($checks.Count)件成功、0件失敗。
- repo fixture: $($fixtureVerification.rows)行×$($fixtureVerification.columns)列、$($fixtureVerification.constants)定数一致。
- fixture SHA-256: $fixtureHash
- GPU実行: 0件。GPU側の24条件測定とは独立したCPU成果である。
- 検査名と各ケースの期待符号・整数ビット長はJSONに記録した。

## 参照の定義

入力のpBits[3]、dirBits[3]はuint32値の数値配列であり、各値をIEEE binary32のビットとして解釈する。
pHex/dirHexは照合用。GPUへはビットを保持した経路で渡し、判定前のfloat演算によるFTZや正規化を混ぜない。
JSONにはfloat座標値を正本として保存しない。expectedBlockedが有限ケースの期待boolである。

normalは符号×(2^23+fraction)×2^(exponentField-1)、subnormalは符号×fractionとし、共通の2^-149単位へ写す。
a=Σdir_i²、b=ΣP_i dir_i、c=ΣP_i²-B²を組込みSystem.Numerics.BigIntegerで計算する。
係数は2^-298単位、D=b²-acは2^-596単位。c<0、またはb<0かつD>0なら遮蔽する。
D=0の接線は非遮蔽。GPUの16bit桁配列、学校式乗算、carry/borrow処理はコピーしていない。

## ケースと自己検査

- 35件は地表内外、接線平面、半直線の向き、方向の正倍率変更などから独立に期待値を定めた。
- 85件の指数掃引により、位置・方向のそれぞれで指数欄0〜254をすべて含む。仮数全域の網羅ではない。
- ±0、最小/最大subnormal、最小normal、有限最大値、隣接floatによる境界、軸交換、異符号dot相殺を含む。
- long-borrow-then-carry-dotでは、M=maxfloat×2^149、S=2^149としてM²-S²に208個連続の1が生じ、S²を戻すとその区間を繰り上がることも確認した。
- 全120件について6軸順序と中心反転の不変性、整数ビット長上限を確認した。
- 既知の斜め接線平面の反例はD=-2^-50を固定点整数でも照合した。
- NaN/Infの指数255は別途8件を拒否し、expectedBlocked=nullとした。±0だけの零方向もCPU自己検査で拒否した。

## 容量と適用限界

有限入力整数の最大値M=(2^24-1)×2^253は277bit。
a、|b|、|c|≤3M²<2^556、|D|≤18M^4<2^1113という保守的上限を確認した。
これは多項式の数学上の容量評価であり、GPU実装全体や有限float全域を検証済みと称するものではない。
この128件の成功だけでは、GPUコンパイラー、配列境界、入出力ビット保持、実時間性能、製品全体の受入は保証しない。
判定入口以前に失われた情報は回復できない。非有限入力は未対応であり、正しい非遮蔽として数えない。

ACS製品はno-STL方針を維持する。本スクリプトのPowerShell/.NET BigIntegerは独立診断にだけ使用し、製品依存へ追加しない。
"@
$readme = ($readme -replace "`r`n", "`n") + "`n"
if ($VerifyOnly) {
    if (-not [IO.File]::Exists($jsonPath)) { throw 'VerifyOnly requires an existing planet-shadow-reference.json; no files or directories were created.' }
    if ((Get-FileHash -LiteralPath $jsonPath -Algorithm SHA256).Hash -cne $jsonHash) { throw 'JSON does not match deterministic regeneration.' }
}
else {
    [void][IO.Directory]::CreateDirectory($referenceRoot)
    [IO.File]::WriteAllText($jsonPath, $json, $utf8)
    [IO.File]::WriteAllText($readmePath, $readme, $utf8)
}
[pscustomobject]@{
    mode = $(if ($VerifyOnly) { 'verify-only' } else { 'generate' })
    finiteCases = $finiteCases.Count
    analyticCases = 35
    rejectedCases = $rejectedInputs.Count
    cpuPassed = $checks.Count
    cpuFailed = 0
    gpuExecutions = 0
    scriptSha256 = $scriptHash
    jsonSha256 = $jsonHash
    fixturePath = $fixturePath
    fixtureSha256 = $fixtureHash
    fixtureRows = $fixtureVerification.rows
    fixtureConstants = $fixtureVerification.constants
    fixtureMatched = $fixtureVerification.matched
    outputDirectory = $referenceRoot
} | ConvertTo-Json
