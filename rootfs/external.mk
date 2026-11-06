
# The mbx gallium driver rides along with Mesa's driver list (it is not one of
# Buildroot's own options). The mesa3d package expands this list when it is used.
MESA3D_GALLIUM_DRIVERS-$(BR2_PACKAGE_MESA3D) += mbx

include $(sort $(wildcard $(BR2_EXTERNAL_N810_PATH)/package/*/*.mk))
