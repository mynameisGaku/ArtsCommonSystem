cbuffer SkyParams : register(b0)
{
    float3 SunDir;
    float SunSize;

    float3 SkyTint;
    float AtmosphereThick;

    float3 GroundColor;
    float Exposure;

    float3 HorizonColor;
    float SunConvergence;
    
    float Time;
    float3 Padding2;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

float hash(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    
    float res = lerp(lerp(hash(i), hash(i + float2(1, 0)), f.x),
                     lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), f.x), f.y);
    return res;
}

float fbm(float2 p)
{
    float total = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; i++)
    {
        total += noise(p) * amp;
        p *= 2.0;
        amp *= 0.5;
    }
    return total;
}


float3 getAtmosphere(float3 viewDir)
{
    float3 sunDirNorm = normalize(SunDir);
    float y = viewDir.y;
    
    float sunDot = dot(viewDir, sunDirNorm);
    
    float zenithFactor = pow(max(0.0, y), 0.5f);
    float3 skyBase = lerp(HorizonColor, SkyTint, zenithFactor);
    
    float sunHaze = pow(max(0.0, sunDot), SunConvergence);
    float3 sunGlow = HorizonColor * sunHaze * 2.0f;
    
    if (y < 0.0)
    {
        skyBase = lerp(HorizonColor, GroundColor, -y * 2.0f);
    }

    return skyBase + sunGlow;
}

float3 getClouds(float3 viewDir, float3 bgCol)
{
    if (viewDir.y < 0.05)
        return bgCol;
    
    float2 cloudUV = viewDir.xz / (viewDir.y + 0.3f);
    
    cloudUV += float2(Time * 0.01f, Time * 0.005f);
    
    float density = fbm(cloudUV * 3.0f);
    
    float cloudMask = smoothstep(0.4f, 0.7f, density);
    
    float sunDot = dot(viewDir, normalize(SunDir));
    float3 cloudColor = float3(1.0, 1.0, 1.0) * (1.0 + sunDot * 0.5);
    
    cloudColor *= lerp(float3(1, 1, 1), HorizonColor, 0.5f);

    return lerp(bgCol, cloudColor, cloudMask * 0.8f);
}

float3 getSunDisk(float3 viewDir)
{
    float dotVal = dot(viewDir, normalize(SunDir));
    if (dotVal < 0.0f)
        return float3(0, 0, 0);
    
    float sunMask = smoothstep(1.0f - SunSize, 1.0f - SunSize + 0.002f, dotVal);
    
    float horizonMask = smoothstep(-0.02f, 0.02f, viewDir.y);

    return float3(1, 1, 1) * sunMask * 10.0f * horizonMask;
}

float4 main(PS_INPUT input) : SV_TARGET
{
    float PI = 3.14159265f;
    float phi = input.uv.y * PI;
    float theta = input.uv.x * 2.0f * PI;

    float y = cos(phi);
    float r = sin(phi);
    float x = r * cos(theta);
    float z = r * sin(theta);
    float3 viewDir = normalize(float3(x, y, z));
    
    float3 col = getAtmosphere(viewDir);
    
    col = getClouds(viewDir, col);
    
    col += getSunDisk(viewDir);
    
    col = 1.0 - exp(-col * Exposure);

    return float4(col, 1.0f);
}