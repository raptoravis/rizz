#version 450

out gl_PerVertex
{
    vec4 gl_Position;
};

layout(binding = 0, std140) uniform type_params
{
    float aspect;
} params;

//in int gl_VertexID;

layout(location = 0) in vec4 in_var_POSSCALE;
layout(location = 1) in vec4 in_var_COLORSPRITE;
layout(location = 0) out vec3 out_var_COLOR0;
layout(location = 1) out vec2 out_var_TEXCOORD0;

void main()
{
    float x = float(uint(gl_VertexID) / 2u);
    float y = float(uint(gl_VertexID) & 1u);

	vec4 pos;

	pos.x = in_var_POSSCALE.x + (x-0.5f) * in_var_POSSCALE.z;
	pos.y = in_var_POSSCALE.y + (y-0.5f) * in_var_POSSCALE.z * params.aspect;
	pos.z = 0.0f;
	pos.w = 1.0f;

    out_var_COLOR0 = in_var_COLORSPRITE.xyz;
    out_var_TEXCOORD0 = vec2((x + in_var_COLORSPRITE.w) * 0.125, 1.0 - y);

    gl_Position = pos;
}

