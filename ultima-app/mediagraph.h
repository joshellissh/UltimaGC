#ifndef MEDIAGRAPH_H
#define MEDIAGRAPH_H

#include <QString>

// Resolve the V4L2 capture node (/dev/videoN) that carries a given camera
// virtual channel, by walking the kernel media graph rather than trusting a
// fixed device-node number.
//
// Why not just open /dev/video2: on the J722S CSI2RX stack the capture nodes
// (/dev/video2..7 = the TI CSI2RX SHIM's DMA contexts) are numbered by driver
// probe order, and /dev/video0..1 are the Wave5 VPU codec nodes — nothing pins
// videoN to a channel. Adding/removing a video driver, or a probe-order change,
// renumbers them. The stable identity is the media-graph topology: the camera
// source (nvp6324) links through the Cadence CSI2RX bridge to the SHIM, whose
// per-VC "context N" entity owns the capture node. This resolves by that
// structure, so a renumber can't cross a feed onto the wrong node.
//
// vc is the virtual-channel index (0..3) — VC0 is the first AHD channel. On
// success returns e.g. "/dev/video2" and clears *diag; on failure returns an
// empty string and, if diag is non-null, sets a short human-readable reason
// (logged by the caller). Linux-only; a stub returns empty elsewhere.
namespace MediaGraph {
QString resolveCsiCaptureNode(int vc, QString *diag = nullptr);
}

#endif
