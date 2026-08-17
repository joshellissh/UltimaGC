// Pairs with blit.vert — samples CameraView's persistent SurroundTexture
// (already RGBA8, uploaded via glTexSubImage2D, no per-pixel work here)
// with no blending: a raw camera feed has no meaningful alpha of its own.
uniform sampler2D uTexture;

in vec2 vTexCoord;

out vec4 fragColor;

void main() {
    fragColor = vec4(texture(uTexture, vTexCoord).rgb, 1.0);
}
