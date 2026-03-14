################################################################################
#
# ultima-app
#
################################################################################

ULTIMA_APP_VERSION = 1.0
ULTIMA_APP_SITE = $(BR2_EXTERNAL_ULTIMA_PATH)/package/ultima-app/src
ULTIMA_APP_SITE_METHOD = local
ULTIMA_APP_DEPENDENCIES = qt5base qt5declarative
ULTIMA_APP_LICENSE = Proprietary

define ULTIMA_APP_CONFIGURE_CMDS
	cd $(@D) && $(QT5_QMAKE) $(ULTIMA_APP_SITE)/ultima-app.pro
endef

define ULTIMA_APP_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D)
endef

define ULTIMA_APP_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/ultima-app $(TARGET_DIR)/root/app/ultima-app
	$(INSTALL) -D -m 0644 $(ULTIMA_APP_SITE)/main.qml $(TARGET_DIR)/root/app/main.qml
endef

$(eval $(generic-package))
