// -------------------------------------------------------------
// SkyPS.hlsl
// UnityのProcedural Skybox風 (グラデーションベース)
// -------------------------------------------------------------

cbuffer SkyParams : register(b0)
{
    float3 g_SunDir;
    float g_Intensity;
    float g_Turbidity; // 今回は「地平線の霞み具合」として使用
    float g_Exposure;
    float g_Time;
    float g_Padding;
};

struct PS_INPUT
{
    float4 Position : SV_POSITION;
    float3 Normal : NORMAL;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
};

static const float PI = 3.14159265359;

// --- Unityっぽい色パレット ---
static const float3 kDayZenithColor = float3(0.22, 0.45, 0.78); // 真上の青
static const float3 kDayHorizonColor = float3(0.75, 0.85, 0.95); // 地平線の白っぽい青

static const float3 kSunsetZenith = float3(0.3, 0.3, 0.5); // 夕方の真上
static const float3 kSunsetHorizon = float3(0.95, 0.55, 0.2); // 夕焼けオレンジ

static const float3 kNightZenith = float3(0.02, 0.02, 0.05); // 夜の真上
static const float3 kNightHorizon = float3(0.05, 0.05, 0.08); // 夜の地平線

static const float3 kGroundColor = float3(0.35, 0.33, 0.31); // 地面の色（グレーブラウン）

// 簡易ノイズ（雲用）
float Hash(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}
float Noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
}
float CloudFBM(float2 uv)
{
    float f = 0.0;
    float w = 0.5;
    for (int i = 0; i < 4; i++)
    {
        f += w * Noise(uv);
        uv *= 2.5;
        w *= 0.5;
    }
    return f;
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float3 viewDir = normalize(input.Normal);
    float3 sunDir = normalize(g_SunDir);
    
    // 太陽の高さ (-1.0 ～ 1.0)
    float sunHeight = sunDir.y;
    
    // --- 1. 背景グラデーションの作成 ---
    // 昼、夕方、夜のブレンド率を計算
    float dayFactor = smoothstep(-0.1, 0.2, sunHeight); // 昼間度
    float sunsetFactor = 1.0 - abs(sunHeight * 2.0 - 0.1); // 夕方度（簡易）
    sunsetFactor = saturate(sunsetFactor * 2.0 - 1.0); // 鋭くする

    // 空の色（真上と地平線）を決定
    float3 currZenith = lerp(kNightZenith, kDayZenithColor, dayFactor);
    float3 currHorizon = lerp(kNightHorizon, kDayHorizonColor, dayFactor);

    // 夕焼けを混ぜる
    currZenith = lerp(currZenith, kSunsetZenith, sunsetFactor * 0.5);
    currHorizon = lerp(currHorizon, kSunsetHorizon, sunsetFactor);

    // 画面上の高さ (0.0=地平線, 1.0=真上)
    float horizonBlend = saturate(viewDir.y * 2.5 + 0.05); // グラデーションのきつさ調整
    horizonBlend = pow(horizonBlend, 0.7); // カーブを少し滑らかに

    float3 skyColor = lerp(currHorizon, currZenith, horizonBlend);

    // --- 2. 地面の描画 ---
    // Unityっぽく、Yが0以下は地面色にする
    if (viewDir.y < 0.0)
    {
        // 地平線付近を少しぼかす
        float groundBlend = smoothstep(-0.05, 0.0, viewDir.y);
        skyColor = lerp(kGroundColor * g_Intensity * 0.5, skyColor, groundBlend);
    }

    // --- 3. 太陽の描画 (Unity風のクッキリ円 + ハロー) ---
    float cosTheta = dot(viewDir, sunDir);
    
    // A. 太陽の周りのボワーッとした光 (Mie Scattering風)
    float sunHaloSize = 150.0; // 小さいほど広い
    float sunHalo = exp(sunHaloSize * (cosTheta - 1.0));
    skyColor += float3(1.0, 0.9, 0.8) * sunHalo * 0.5 * dayFactor; // 強すぎないように

    // B. 太陽本体 (クッキリした円)
    float sunRadius = 0.9992; // 太陽の大きさ
    float sunDisk = smoothstep(sunRadius, sunRadius + 0.0002, cosTheta);
    skyColor += float3(1.0, 1.0, 0.9) * sunDisk * 5.0; // 太陽本体は明るく

    // --- 4. 雲 (控えめに追加) ---
    if (viewDir.y > 0.05)
    {
        float speed = 0.01;
        float2 cloudUV = (viewDir.xz / (viewDir.y + 0.1)) * 0.6 + (g_Time * speed);
        float noiseVal = CloudFBM(cloudUV);
        
        // 雲の形を整える
        float cloudAlpha = smoothstep(0.45, 0.75, noiseVal);
        
        // 地平線際で消す
        cloudAlpha *= smoothstep(0.05, 0.3, viewDir.y);
        
        // 雲の色（白）に太陽光を少し乗せる
        float3 cloudColor = float3(1.0, 1.0, 1.0);
        cloudColor = lerp(cloudColor, float3(1.0, 0.8, 0.6), sunsetFactor); // 夕方はオレンジに
        
        // 空と合成
        skyColor = lerp(skyColor, cloudColor * 1.5, cloudAlpha * 0.7); // 0.7は不透明度
    }

    // --- 5. 最終出力 ---
    return float4(skyColor, 1.0);
}