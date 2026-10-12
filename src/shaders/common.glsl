// Shared by all compute shaders. Must match struct PushConstants in framegen.cpp.
layout(push_constant) uniform PushConstants {
    mat4 reproj;   // previous projView * current projViewInverse (GL clip space)
    vec2 size;     // render extent in pixels
    vec2 invSize;
    int flipY;     // 1: framebuffer row 0 is GL NDC y = +1
    int depthMode; // 0: depth = (z_ndc + 1) / 2, 1: depth = z_ndc
    int step;      // pixel stride for statistics
    int pad;
} pc;

// Where the surface seen at `uv` (with depth `d`) was on screen in the previous frame.
vec2 Reproject(vec2 uv, float d, int flipY, int depthMode) {
    float ndcY = flipY != 0 ? 1.0 - 2.0 * uv.y : 2.0 * uv.y - 1.0;
    float ndcZ = depthMode != 0 ? d : 2.0 * d - 1.0;
    vec4 prev = pc.reproj * vec4(2.0 * uv.x - 1.0, ndcY, ndcZ, 1.0);
    vec2 p = prev.xy / prev.w;
    return vec2(0.5 * p.x + 0.5, flipY != 0 ? 0.5 - 0.5 * p.y : 0.5 * p.y + 0.5);
}

float Luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}
