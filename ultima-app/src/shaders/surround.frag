// Sample an already-decoded RGB camera texture, apply the calibration-time
// blend weight, done. This is deliberately all the per-pixel work the GPU
// does at runtime — no lens/ground-plane math here, see surround.vert.
uniform sampler2D uTexture;
// 0 = hard edge (step the precomputed weight at 0.5, discarding the
//     feather the mesh already carries),
// 1 = feather/improved seam (use the mesh's weight as-is — feather vs.
//     improved differ in how WarpMesh computed the weight, not here).
uniform int uBlendMode;

in vec2 vTexCoord;
in float vWeight;

out vec4 fragColor;

void main() {
    vec4 color = texture(uTexture, vTexCoord);
    float w = (uBlendMode == 0) ? step(0.5, vWeight) : vWeight;
    fragColor = vec4(color.rgb, color.a * w);
}
