#ifndef WARPMESH_H
#define WARPMESH_H

#include "cameracalibration.h"
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLFunctions>
#include <vector>
#include <cstdint>
#include <memory>

// Matches shaders/surround.vert's layout(location=...) attributes exactly.
struct WarpVertex {
    float x, y;    // output-space NDC position
    float u, v;    // source camera texture UV
    float weight;  // precomputed blend weight
};

enum class BlendQuality {
    Feather,  // linear falloff across the overlap wedge
    Improved, // same overlap geometry, a gentler S-curve falloff instead
};

// One camera's precomputed ground-plane warp, as a triangle grid mesh —
// this is where all the fisheye/perspective/ground-plane math happens, and
// it happens exactly once (in build()), never per-frame. Runtime rendering
// (SurroundView) only ever binds this mesh's VAO and issues one indexed
// draw call per camera.
//
// Ported from test/avm-benchmark/src/graphics/WarpMesh.{h,cpp} — see that
// project's docs/measurement-notes.md and README.md for the real-hardware
// validation behind this architecture (precomputed mesh + feather blend,
// vs. e.g. per-frame optical flow / AI stitching, which the project brief
// there explicitly ruled out).
class WarpMesh {
public:
    // Builds the CPU-side grid for one camera. Returns false only on
    // degenerate input (e.g. zero grid resolution).
    bool build(const CameraCalibration &cam, const SurroundGeometryConfig &geom, BlendQuality quality);

    // Uploads to GPU buffers and builds a VAO. `f` must be the current
    // context's functions.
    bool upload(QOpenGLFunctions *f);
    void draw(QOpenGLFunctions *f);

    // Releases the VBO/IBO/VAO. Must be called (with the owning context
    // still current) before a WarpMesh is destroyed — QOpenGLBuffer does
    // NOT release its underlying GL buffer object from its own destructor.
    void destroy(QOpenGLFunctions *f);

    int vertexCount() const { return int(m_vertices.size()); }
    int triangleCount() const { return int(m_indices.size() / 3); }

private:
    std::vector<WarpVertex> m_vertices;
    std::vector<uint32_t> m_indices;

    std::unique_ptr<QOpenGLVertexArrayObject> m_vao;
    QOpenGLBuffer m_vbo{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_ibo{QOpenGLBuffer::IndexBuffer};
};

#endif
