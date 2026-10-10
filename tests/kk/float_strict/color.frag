#version 460
#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], 4461, 32);
layout(push_constant) uniform PC { vec4 k; } pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;

void main()
{
    float a = v_uv.x * pc.k.x, b = v_uv.y * pc.k.y;
    o = vec4(min(a, b), max(sqrt(a), b * 0.5), a < b ? 1.0 : 0.25, fract(a + b));
}
