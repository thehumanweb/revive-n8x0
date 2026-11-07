################################################################################
#
# mbx-demo
#
################################################################################

MBX_DEMO_VERSION = 1
MBX_DEMO_SITE = $(BR2_EXTERNAL_N810_PATH)/build/mbx-demo-src
MBX_DEMO_SITE_METHOD = local
MBX_DEMO_LICENSE = MIT
MBX_DEMO_DEPENDENCIES = mesa3d libdrm

define MBX_DEMO_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -std=gnu11 -o $(@D)/gbm_tri $(@D)/gbm_tri.c \
		-I$(STAGING_DIR)/usr/include/libdrm $(TARGET_LDFLAGS) -lgbm -lEGL -lGLESv2 -ldrm -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -std=gnu11 -o $(@D)/gbm_damage $(@D)/gbm_damage.c \
		-I$(STAGING_DIR)/usr/include/libdrm $(TARGET_LDFLAGS) -lgbm -lEGL -lGLESv2 -ldrm -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -std=gnu11 -o $(@D)/gles2_tri $(@D)/gles2_tri.c \
		$(TARGET_LDFLAGS) -lEGL -lGLESv2 -lm
	$(TARGET_CC) $(TARGET_CFLAGS) -O2 -std=gnu11 -o $(@D)/gles2_feat $(@D)/gles2_feat.c \
		$(TARGET_LDFLAGS) -lEGL -lGLESv2 -lm
endef

define MBX_DEMO_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/gbm_tri $(TARGET_DIR)/usr/bin/gbm_tri
	$(INSTALL) -D -m 0755 $(@D)/gbm_damage $(TARGET_DIR)/usr/bin/gbm_damage
	$(INSTALL) -D -m 0755 $(@D)/gles2_tri $(TARGET_DIR)/usr/bin/gles2_tri
	$(INSTALL) -D -m 0755 $(@D)/gles2_feat $(TARGET_DIR)/usr/bin/gles2_feat
endef

$(eval $(generic-package))
