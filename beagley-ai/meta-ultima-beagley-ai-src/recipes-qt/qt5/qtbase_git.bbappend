# meta-ti's j722s.inc + meta-beagle's beagle-bsp.inc wire
# PREFERRED_PROVIDER_virtual/gpudriver to the Rogue stack, so PACKAGECONFIG_GL
# already defaults to "kms gbm gles2 eglfs" for this machine. linuxfb added as
# a same-image fallback: first bring-up on new hardware runs the
# app on linuxfb (see this layer's recipes-ultima/ultima-app/ultima-app/
# beagley-ai/ultima-app.service) to isolate the app/QML layer from the
# unproven BXS-4-64 GPU driver stack, before switching the service back to
# eglfs.
PACKAGECONFIG:append:beagley-ai = " linuxfb"
