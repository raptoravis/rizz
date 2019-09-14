#version 450

layout(binding=0) uniform sampler2D tex0smp0;

layout(location = 0) in vec3 in_var_COLOR0;
layout(location = 1) in vec2 in_var_TEXCOORD0;
layout(location = 0) out vec4 out_var_SV_Target0;

void main()
{
    vec4 diffuse = texture(tex0smp0, in_var_TEXCOORD0);
    vec3 diffuseColor;
    vec3 diff = mix(diffuseColor, vec3(dot(diffuseColor, vec3(0.333000004291534423828125))), vec3(0.800000011920928955078125)).xyz * in_var_COLOR0;
    out_var_SV_Target0 = vec4(diff.x, diff.y, diff.z, diffuse.w);
}
