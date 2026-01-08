cbuffer SkyVSParams : register(b0)
{
    float4x4 ViewProj;
    float3 CameraPos;
    float Padding0;
};

struct VS_IN
{
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float2 uv : TEXCOORD0;
};

struct VS_OUT
{
    float4 pos : SV_POSITION;
    float3 viewDir : TEXCOORD0;
};

VS_OUT main(VS_IN IN)
{
    VS_OUT OUT;

    float3 worldPos = IN.pos;

    OUT.pos = mul(float4(worldPos, 1.0f), ViewProj);

    float3 dir = worldPos - CameraPos;
    OUT.viewDir = normalize(dir);

    return OUT;
}
