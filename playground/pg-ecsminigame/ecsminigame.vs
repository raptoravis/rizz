

cbuffer params : register(b0) {
  float aspect;
};
struct vs_in {
  float4 posScale : POSSCALE;
  float4 colorIndex : COLORSPRITE;
  uint vid : SV_VertexID;
};
struct v2f {
  float3 color : COLOR0;
  float2 uv : TEXCOORD0;
  float4 pos : SV_Position;
};
v2f main(vs_in inp) {
  v2f outp;
  float x = inp.vid / 2;
  float y = inp.vid & 1;
  outp.pos.x = inp.posScale.x + (x-0.5f) * inp.posScale.z;
  outp.pos.y = inp.posScale.y + (y-0.5f) * inp.posScale.z * aspect;
  outp.pos.z = 0.0f;
  outp.pos.w = 1.0f;
  outp.uv = float2((x + inp.colorIndex.w)/8,1-y);
  outp.color = inp.colorIndex.rgb;
  return outp;
}