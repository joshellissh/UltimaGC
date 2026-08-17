// Vertex shader shared by every SurroundView fragment-shader variant
// (surround.frag today; a future native-YUV path would reuse this
// unchanged). ShaderManager prepends #version + precision — see
// shadermanager.cpp for why this file has neither.
//
// aPosition/aTexCoord/aWeight come straight out of a calibration-time
// WarpMesh (warpmesh.h): the lens/ground-plane math that produced them
// never runs per-frame, only this trivial passthrough does. Fixed
// locations so WarpMesh's VAO setup never needs to query a specific linked
// program's attribute indices.
layout(location = 0) in vec2 aPosition;  // output-space NDC, [-1,1]
layout(location = 1) in vec2 aTexCoord;  // source camera UV, [0,1]
layout(location = 2) in float aWeight;   // precomputed blend weight, [0,1] (0 = fully masked)

out vec2 vTexCoord;
out float vWeight;

void main() {
    vTexCoord = aTexCoord;
    vWeight = aWeight;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
