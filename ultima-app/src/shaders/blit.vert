// Plain full-viewport textured quad — no calibration mesh, no blend weight.
// Deliberately NOT surround.vert: that shader's aPosition/aTexCoord/aWeight
// layout is WarpMesh-specific (per-camera calibration geometry); CameraView
// draws one un-stitched camera feed per instance with no warp at all.
// Ported from test/avm-benchmark/shaders/blit.vert (TEST 20's stitch-buffer
// blit), which this is functionally identical to.
layout(location = 0) in vec2 aPosition;  // NDC, [-1,1]
layout(location = 1) in vec2 aTexCoord;  // [0,1]

out vec2 vTexCoord;

void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
