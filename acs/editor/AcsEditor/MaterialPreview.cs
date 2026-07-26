using System;
using System.Collections.Generic;
using System.Threading;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AcsEditor;

/// <summary>
/// マテリアル効果プリセットを「サンプル絵」に CPU 適用してサムネイル画像を作る。
///
/// 用途: マテリアルエディタのライブプレビューと、アセットブラウザの .acsmat サムネイル。
///       実描画は GPU シェーダ (kSpriteEffectHLSL) が行うが、RHI にテクスチャ読み戻しが
///       無いため、プレビュー専用にここで同じ数式を CPU で再現する。
///
/// 重要: 各効果の数式は src/render/SpriteBatch.cpp の kSpriteEffectHLSL (PSEffect) と
///       «対» で保たねばならない。シェーダ側を変えたらここも合わせること。
/// </summary>
internal static class MaterialPreview
{
    // アニメ効果(Wave/HueShift)が静止プレビューでも見えるよう、代表的な時刻を使う。
    private const float PreviewTime = 1.0f;

    private const int BasePatternCacheCapacity = 4;
    private static readonly object BasePatternGate = new();
    private static readonly Dictionary<int, float[]> BasePatterns = new();
    private static readonly Dictionary<int, LinkedListNode<int>> BasePatternNodes = new();
    private static readonly LinkedList<int> BasePatternLru = new();

    /// <summary>効果プリセットを適用したサムネイル画像を返す。</summary>
    public static BitmapSource Render(int effect, float strength, float p0, float p1, float p2,
                                      float[] color4, bool animated, int size)
        => Render(effect, strength, p0, p1, p2, color4, animated, size, CancellationToken.None);

    internal static BitmapSource Render(
        MaterialPreviewRequest request,
        CancellationToken cancellationToken)
    {
        ArgumentNullException.ThrowIfNull(request);
        if (request.Kind == MaterialPreviewKind.Pbr)
        {
            return RenderPbr(
                new[] { request.ColorR, request.ColorG, request.ColorB, request.ColorA },
                request.Metallic,
                request.Roughness,
                new[] { request.EmissiveR, request.EmissiveG, request.EmissiveB },
                request.EmissiveStrength,
                request.NormalStrength,
                request.AmbientOcclusion,
                request.PixelSize,
                cancellationToken);
        }

        return Render(
            request.Effect,
            request.Strength,
            request.Param0,
            request.Param1,
            request.Param2,
            new[] { request.ColorR, request.ColorG, request.ColorB, request.ColorA },
            request.Animated,
            request.PixelSize,
            cancellationToken);
    }

    private static BitmapSource Render(
        int effect,
        float strength,
        float p0,
        float p1,
        float p2,
        float[] color4,
        bool animated,
        int size,
        CancellationToken cancellationToken)
    {
        size = Math.Clamp(size, 8, 1024);
        cancellationToken.ThrowIfCancellationRequested();
        float[] src = GetBasePattern(size);
        float t = animated ? PreviewTime : 0.0f;
        float cr = color4.Length > 0 ? color4[0] : 1f;
        float cg = color4.Length > 1 ? color4[1] : 1f;
        float cb = color4.Length > 2 ? color4[2] : 1f;

        var px = new byte[size * size * 4];   // BGRA32
        for (int y = 0; y < size; y++)
        {
            if ((y & 3) == 0)
                cancellationToken.ThrowIfCancellationRequested();
            for (int x = 0; x < size; x++)
            {
                float u = (x + 0.5f) / size;
                float v = (y + 0.5f) / size;

                // --- UV 歪み系 (サンプル前) ---
                float su = u, sv = v;
                if (effect == 4)        // Wave
                {
                    su += MathF.Sin(v * p0 + t * p1) * strength;
                    sv += MathF.Cos(u * p0 + t * p1) * strength * 0.5f;
                }
                else if (effect == 5)   // Pixelate
                {
                    float cx = MathF.Max(p0, 1f), cy = MathF.Max(p1, 1f);
                    su = (MathF.Floor(u * cx) + 0.5f) / cx;
                    sv = (MathF.Floor(v * cy) + 0.5f) / cy;
                }

                float r, g, b, a;
                if (effect == 12)       // Chromatic: R/B を逆方向にずらしてサンプル
                {
                    Sample(src, size, su + p0, sv, out float sr_r, out _, out _, out float sr_a);
                    Sample(src, size, su, sv, out float cg_r, out float cg_g, out float cg_b, out float cg_a);
                    Sample(src, size, su - p0, sv, out _, out _, out float sb_b, out float sb_a);
                    float rr = Lerp(cg_r, sr_r, sr_a);
                    float bb = Lerp(cg_b, sb_b, sb_a);
                    float s = Sat(strength);
                    r = Lerp(cg_r, rr, s); g = cg_g; b = Lerp(cg_b, bb, s); a = cg_a;
                }
                else
                {
                    Sample(src, size, su, sv, out r, out g, out b, out a);
                }

                // --- 色系 (サンプル後) ---
                switch (effect)
                {
                    case 1:   // Grayscale
                    {
                        float gray = r * 0.299f + g * 0.587f + b * 0.114f;
                        float s = Sat(strength);
                        r = Lerp(r, gray, s); g = Lerp(g, gray, s); b = Lerp(b, gray, s);
                        break;
                    }
                    case 2:   // Tint
                    {
                        float s = Sat(strength);
                        r = Lerp(r, r * cr, s); g = Lerp(g, g * cg, s); b = Lerp(b, b * cb, s);
                        break;
                    }
                    case 3:   // Vignette
                    {
                        float dx = u - 0.5f, dy = v - 0.5f;
                        float rad = MathF.Sqrt(dx * dx + dy * dy) * 1.41421356f;
                        float vig = SmoothStep(p0, MathF.Max(p1, p0 + 0.001f), rad);
                        float s = Sat(strength) * vig;
                        r = Lerp(r, cr, s); g = Lerp(g, cg, s); b = Lerp(b, cb, s);
                        break;
                    }
                    case 6:   // HueShift
                        HueShift(ref r, ref g, ref b, (strength + p0 * t) * (MathF.PI / 180f));
                        break;
                    case 7:   // Brightness/Contrast
                        r = Sat(((r - 0.5f) * MathF.Max(p0, 0f) + 0.5f) * strength);
                        g = Sat(((g - 0.5f) * MathF.Max(p0, 0f) + 0.5f) * strength);
                        b = Sat(((b - 0.5f) * MathF.Max(p0, 0f) + 0.5f) * strength);
                        break;
                    case 8:   // Invert
                    {
                        float s = Sat(strength);
                        r = Lerp(r, 1f - r, s); g = Lerp(g, 1f - g, s); b = Lerp(b, 1f - b, s);
                        break;
                    }
                    case 9:   // Sepia
                    {
                        float sr = r * 0.393f + g * 0.769f + b * 0.189f;
                        float sg = r * 0.349f + g * 0.686f + b * 0.168f;
                        float sb = r * 0.272f + g * 0.534f + b * 0.131f;
                        float s = Sat(strength);
                        r = Lerp(r, sr, s); g = Lerp(g, sg, s); b = Lerp(b, sb, s);
                        break;
                    }
                    case 10:  // Posterize
                    {
                        float lv = MathF.Max(p0, 2f); float s = Sat(strength);
                        r = Lerp(r, MathF.Floor(r * lv + 0.5f) / lv, s);
                        g = Lerp(g, MathF.Floor(g * lv + 0.5f) / lv, s);
                        b = Lerp(b, MathF.Floor(b * lv + 0.5f) / lv, s);
                        break;
                    }
                    case 11:  // Scanline
                    {
                        float sc = 0.5f + 0.5f * MathF.Sin(v * p0 * 6.2831853f);
                        float k = 1f - Sat(strength) * sc;
                        r *= k; g *= k; b *= k;
                        break;
                    }
                }

                int o = (y * size + x) * 4;
                px[o + 0] = ToByte(b); px[o + 1] = ToByte(g); px[o + 2] = ToByte(r); px[o + 3] = ToByte(a);
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        var bmp = BitmapSource.Create(size, size, 96, 96, PixelFormats.Bgra32, null, px, size * 4);
        bmp.Freeze();
        return bmp;
    }

    /// <summary>
    /// PBR (metallic-roughness) マテリアルを «ライト付きの球» に Cook-Torrance BRDF で陰影付けし、
    /// サムネイル化する。実シーンの lit スプライト描画は GPU 側で行う予定だが、プレビューは
    /// 同じ BRDF を CPU 再現する (GPU 読み戻し実装後は実シェーダ描画へ置換可能)。
    /// </summary>
    public static BitmapSource RenderPbr(float[] baseColor4, float metallic, float roughness,
                                         float[] emissive3, float emissiveStrength,
                                         float normalStrength, float ao, int size)
        => RenderPbr(
            baseColor4,
            metallic,
            roughness,
            emissive3,
            emissiveStrength,
            normalStrength,
            ao,
            size,
            CancellationToken.None);

    private static BitmapSource RenderPbr(
        float[] baseColor4,
        float metallic,
        float roughness,
        float[] emissive3,
        float emissiveStrength,
        float normalStrength,
        float ao,
        int size,
        CancellationToken cancellationToken)
    {
        size = Math.Clamp(size, 8, 1024);
        cancellationToken.ThrowIfCancellationRequested();
        float bcr = baseColor4[0], bcg = baseColor4[1], bcb = baseColor4[2];
        float emr = emissive3[0] * emissiveStrength, emg = emissive3[1] * emissiveStrength, emb = emissive3[2] * emissiveStrength;
        float rough = Math.Clamp(roughness, 0.04f, 1f);
        float metal = Math.Clamp(metallic, 0f, 1f);

        // ライト (左上手前) と環境光。
        float lx = -0.45f, ly = -0.55f, lz = 0.70f;
        float ll = MathF.Sqrt(lx * lx + ly * ly + lz * lz); lx /= ll; ly /= ll; lz /= ll;
        const float lightI = 2.0f;
        float ambR = 0.05f, ambG = 0.06f, ambB = 0.08f;

        // F0 = 誘電体 0.04 と baseColor を metallic で補間。
        float f0r = Lerp(0.04f, bcr, metal), f0g = Lerp(0.04f, bcg, metal), f0b = Lerp(0.04f, bcb, metal);
        float a2 = rough * rough; a2 *= a2;
        float kg = (rough + 1f); kg = kg * kg / 8f;

        var px = new byte[size * size * 4];
        for (int y = 0; y < size; y++)
        {
            if ((y & 3) == 0)
                cancellationToken.ThrowIfCancellationRequested();
            for (int x = 0; x < size; x++)
            {
                // 画面 -> 単位円。中心 (0,0)、半径 1 の球。
                float u = (x + 0.5f) / size * 2f - 1f;
                float v = (y + 0.5f) / size * 2f - 1f;
                float r2 = u * u + v * v;
                int o = (y * size + x) * 4;
                if (r2 > 1.0f)   // 球の外 = 暗い背景
                {
                    px[o + 0] = 28; px[o + 1] = 25; px[o + 2] = 23; px[o + 3] = 255;
                    continue;
                }
                // 球の法線 (z は手前)。normalStrength で平面(0,0,1)へ寄せる。
                float nz = MathF.Sqrt(MathF.Max(0f, 1f - r2));
                float nx = u, ny = v;
                float ns = Math.Clamp(normalStrength, 0f, 2f);
                nx *= ns; ny *= ns;
                float nl = MathF.Sqrt(nx * nx + ny * ny + nz * nz);
                nx /= nl; ny /= nl; nz /= nl;

                // V=(0,0,1), L=light, H=normalize(V+L)
                float hX = lx, hY = ly, hZ = lz + 1f;
                float hl = MathF.Sqrt(hX * hX + hY * hY + hZ * hZ); hX /= hl; hY /= hl; hZ /= hl;
                float ndl = MathF.Max(nx * lx + ny * ly + nz * lz, 0f);
                float ndv = MathF.Max(nz, 1e-3f);                      // dot(N, +Z)
                float ndh = MathF.Max(nx * hX + ny * hY + nz * hZ, 0f);
                float vdh = MathF.Max(hZ, 0f);                          // dot(V=+Z, H)

                // GGX NDF
                float denom = ndh * ndh * (a2 - 1f) + 1f;
                float D = a2 / MathF.Max(MathF.PI * denom * denom, 1e-6f);
                // Smith geometry
                float gv = ndv / (ndv * (1f - kg) + kg);
                float gl = ndl / (ndl * (1f - kg) + kg);
                float G = gv * gl;
                // Fresnel (Schlick)
                float fc = MathF.Pow(1f - vdh, 5f);
                float fr = f0r + (1f - f0r) * fc, fg = f0g + (1f - f0g) * fc, fb = f0b + (1f - f0b) * fc;

                float specD = D * G / MathF.Max(4f * ndv * ndl, 1e-4f);
                float kdr = (1f - fr) * (1f - metal), kdg = (1f - fg) * (1f - metal), kdb = (1f - fb) * (1f - metal);
                float invPi = 1f / MathF.PI;

                // 直接光: (diffuse + spec) * lightColor * NdotL
                float rr = (kdr * bcr * invPi + specD * fr) * lightI * ndl;
                float gg = (kdg * bcg * invPi + specD * fg) * lightI * ndl;
                float bb = (kdb * bcb * invPi + specD * fb) * lightI * ndl;
                // 環境 + 発光
                rr += bcr * ambR * ao + emr; gg += bcg * ambG * ao + emg; bb += bcb * ambB * ao + emb;
                // Reinhard トーンマップ + ガンマ
                rr = rr / (rr + 1f); gg = gg / (gg + 1f); bb = bb / (bb + 1f);
                rr = MathF.Pow(rr, 1f / 2.2f); gg = MathF.Pow(gg, 1f / 2.2f); bb = MathF.Pow(bb, 1f / 2.2f);

                px[o + 0] = ToByte(bb); px[o + 1] = ToByte(gg); px[o + 2] = ToByte(rr); px[o + 3] = 255;
            }
        }
        cancellationToken.ThrowIfCancellationRequested();
        var bmp = BitmapSource.Create(size, size, 96, 96, PixelFormats.Bgra32, null, px, size * 4);
        bmp.Freeze();
        return bmp;
    }

    // ===== サンプル絵 (ミニ風景: 空グラデ + 太陽 + 丘) — 効果が読みやすい色/形/勾配を含む =====
    private static float[] GetBasePattern(int size)
    {
        lock (BasePatternGate)
        {
            if (BasePatterns.TryGetValue(size, out float[]? cached))
            {
                TouchBasePatternLocked(size);
                return cached;
            }
        }

        // Build outside the lock. A concurrent duplicate is harmless and avoids
        // serializing unrelated preview sizes behind a relatively expensive fill.
        float[] built = BuildBasePattern(size);
        lock (BasePatternGate)
        {
            if (BasePatterns.TryGetValue(size, out float[]? cached))
            {
                TouchBasePatternLocked(size);
                return cached;
            }

            BasePatterns.Add(size, built);
            var node = new LinkedListNode<int>(size);
            BasePatternNodes.Add(size, node);
            BasePatternLru.AddFirst(node);
            while (BasePatterns.Count > BasePatternCacheCapacity)
            {
                LinkedListNode<int>? oldest = BasePatternLru.Last;
                if (oldest is null) break;
                BasePatternLru.RemoveLast();
                BasePatternNodes.Remove(oldest.Value);
                BasePatterns.Remove(oldest.Value);
            }
            return built;
        }
    }

    private static void TouchBasePatternLocked(int size)
    {
        LinkedListNode<int> node = BasePatternNodes[size];
        BasePatternLru.Remove(node);
        BasePatternLru.AddFirst(node);
    }

    private static float[] BuildBasePattern(int size)
    {
        var img = new float[size * size * 4];
        for (int y = 0; y < size; y++)
        {
            for (int x = 0; x < size; x++)
            {
                float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
                float r, g, b;
                if (v < 0.62f)   // 空: 上=水色 → 下=橙
                {
                    float t = v / 0.62f;
                    r = Lerp(0.35f, 0.98f, t); g = Lerp(0.60f, 0.62f, t); b = Lerp(0.95f, 0.40f, t);
                }
                else             // 丘: 緑
                {
                    float t = (v - 0.62f) / 0.38f;
                    r = Lerp(0.20f, 0.10f, t); g = Lerp(0.65f, 0.40f, t); b = Lerp(0.25f, 0.18f, t);
                }
                // 太陽 (赤橙の円)
                float dx = u - 0.70f, dy = v - 0.32f;
                float d = MathF.Sqrt(dx * dx + dy * dy);
                if (d < 0.15f)
                {
                    float t = Sat(d / 0.15f);
                    r = Lerp(1.0f, r, t); g = Lerp(0.85f, g, t); b = Lerp(0.30f, b, t);
                }
                int o = (y * size + x) * 4;
                img[o + 0] = r; img[o + 1] = g; img[o + 2] = b; img[o + 3] = 1.0f;
            }
        }
        return img;
    }

    // clamp サンプル (nearest)。
    private static void Sample(float[] img, int size, float u, float v,
                               out float r, out float g, out float b, out float a)
    {
        int x = (int)MathF.Floor(Sat(u) * (size - 1) + 0.5f);
        int y = (int)MathF.Floor(Sat(v) * (size - 1) + 0.5f);
        if (x < 0) x = 0; else if (x >= size) x = size - 1;
        if (y < 0) y = 0; else if (y >= size) y = size - 1;
        int o = (y * size + x) * 4;
        r = img[o]; g = img[o + 1]; b = img[o + 2]; a = img[o + 3];
    }

    private static float Sat(float v) => v < 0f ? 0f : (v > 1f ? 1f : v);
    private static float Lerp(float a, float b, float t) => a + (b - a) * t;

    private static float SmoothStep(float e0, float e1, float x)
    {
        float t = Sat((x - e0) / (e1 - e0));
        return t * t * (3f - 2f * t);
    }

    // 輝度軸まわりの色相回転 (Rodrigues、HLSL の HueShiftRGB と同式)。
    private static void HueShift(ref float r, ref float g, ref float b, float angle)
    {
        const float k = 0.57735027f;
        float ca = MathF.Cos(angle), sa = MathF.Sin(angle);
        float dotk = k * (r + g + b);
        // cross(k_vec, c) で k_vec=(k,k,k): (k*b - k*g, k*r - k*b, k*g - k*r)
        float cx = k * b - k * g, cy = k * r - k * b, cz = k * g - k * r;
        float nr = r * ca + cx * sa + k * dotk * (1f - ca);
        float ng = g * ca + cy * sa + k * dotk * (1f - ca);
        float nb = b * ca + cz * sa + k * dotk * (1f - ca);
        r = nr; g = ng; b = nb;
    }

    private static byte ToByte(float v) => (byte)Math.Clamp(v * 255f + 0.5f, 0f, 255f);
}
