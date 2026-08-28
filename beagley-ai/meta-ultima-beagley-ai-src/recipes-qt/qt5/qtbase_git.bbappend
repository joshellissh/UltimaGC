# Same reasoning as meta-ultima-beagleplay's own beagleplay-ti line — meta-ti's
# j722s.inc + meta-beagle's beagle-bsp.inc wire
# PREFERRED_PROVIDER_virtual/gpudriver to the Rogue stack the same way
# beagleplay-ti.conf does, so PACKAGECONFIG_GL already defaults to
# "kms gbm gles2 eglfs" for this machine too. linuxfb added as a same-image
# fallback here for the same reason: first bring-up on new hardware runs the
# app on linuxfb (see this layer's recipes-ultima/ultima-app/ultima-app/
# beagley-ai/ultima-app.service) to isolate the app/QML layer from the
# unproven BXS-4-64 GPU driver stack, before switching the service back to
# eglfs.
PACKAGECONFIG:append:beagley-ai = " linuxfb"
