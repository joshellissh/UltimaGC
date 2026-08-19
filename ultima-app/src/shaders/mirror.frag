// Fisheye source -> mirror-replacement rectilinear view, single camera, no
// blending. Companion to blit.frag/blit.vert (reuses blit.vert unchanged —
// vTexCoord is just this view's own output UV, [0,1] over the quad), but
// replaces blit.frag's raw passthrough with a per-pixel reprojection.
//
// Same equidistant fisheye model warpmesh.cpp uses (theta = angle off the
// optical axis, r = f*theta), just run in reverse: warpmesh forward-projects
// world ground points into a source camera's pixels on the CPU; this instead
// starts from an output pixel in a virtual, level, narrow-FOV "mirror"
// camera and finds the fisheye source pixel that ray would have come from,
// per fragment.
//
// uRight/uUp/uForward are the virtual mirror camera's own axes, expressed in
// the physical fisheye camera's own coordinate frame (right=+X, up=+Y,
// forward=+Z/optical axis) — computed once per frame on the CPU from the
// yaw/pitch/roll placeholder constants in cameraview.cpp, not re-derived
// per pixel.
uniform sampler2D uTexture;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec3 uForward;
uniform float uTanHalfHFov;
uniform float uTanHalfVFov;
uniform float uFocalPx;
uniform float uThetaMaxRad;
uniform float uPrincipalXFrac; // [0,1] of image width, usually 0.5 (center)
uniform float uPrincipalYFrac; // [0,1] of image height, usually 0.5 (center)

in vec2 vTexCoord;

out vec4 fragColor;

void main() {
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec3 rayVirtual = vec3(ndc.x * uTanHalfHFov, ndc.y * uTanHalfVFov, 1.0);
    vec3 rayPhysical = rayVirtual.x * uRight + rayVirtual.y * uUp + rayVirtual.z * uForward;

    float radial = length(rayPhysical.xy);
    float theta = atan(radial, rayPhysical.z);
    if (rayPhysical.z <= 0.0001 || theta > uThetaMaxRad) {
        fragColor = vec4(0.0);
        return;
    }

    float phi = atan(rayPhysical.y, rayPhysical.x);
    float r = uFocalPx * theta;
    vec2 imageSize = vec2(textureSize(uTexture, 0));
    vec2 srcUV = vec2(uPrincipalXFrac, uPrincipalYFrac) + (r * vec2(cos(phi), sin(phi))) / imageSize;

    if (srcUV.x < 0.0 || srcUV.x > 1.0 || srcUV.y < 0.0 || srcUV.y > 1.0) {
        fragColor = vec4(0.0);
        return;
    }
    fragColor = vec4(texture(uTexture, srcUV).rgb, 1.0);
}
