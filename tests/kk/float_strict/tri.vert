#version 460
#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], 4461, 32);
layout(push_constant) uniform PC { vec4 k; } pc;
layout(location = 0) out vec2 v_uv;

// One triangle over the whole target, its position through the arithmetic
// a vertex shader does (multiply-adds, min/max, square roots), which keeps
// every bit of its SignedZeroInfNanPreserve in any KK_FLOAT_STRICT mode.
void main()
{
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    float s = sqrt(pc.k.z * pc.k.z + 1.0) * inversesqrt(pc.k.z * pc.k.z + 1.0);
    vec2 p = fma(uv, vec2(2.0), vec2(-1.0)) * max(min(s, 1.0), 0.5);
    gl_Position = vec4(p, 0.5 * s, 1.0);
    v_uv = uv;
}
