#version 460
#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], 4461, 32);
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(push_constant) uniform PC { vec4 k; } pc;
layout(location = 0) in vec2 g_uv[];
layout(location = 0) out vec2 v_uv;

// The triangle passed through, its positions through min/max, a compare and
// a square root (the identity for k.z = 0). KosmicKrisp runs the vertex
// shader before it and it as compute kernels: both keep every bit of their
// SignedZeroInfNanPreserve in any KK_FLOAT_STRICT mode.
void main()
{
    for (int i = 0; i < 3; i++) {
        vec4 p = gl_in[i].gl_Position;
        float s = sqrt(pc.k.z * pc.k.z + 1.0);
        gl_Position = vec4(p.xy * max(min(s, 1.0), 0.5), p.z < 2.0 ? p.z : 0.5, p.w);
        v_uv = g_uv[i];
        EmitVertex();
    }
    EndPrimitive();
}
