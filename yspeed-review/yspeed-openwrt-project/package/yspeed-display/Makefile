include $(TOPDIR)/rules.mk

PKG_NAME:=yspeed-display
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/yspeed-display
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=YSPEED ST7789 GNSS status display
  DEPENDS:=+gpsd-clients +kmod-fb-tft-st7789v-yspeed
endef

define Package/yspeed-display/description
  Small framebuffer status display for the YS-GPS-12832 ribbon-connected ST7789V.
  Shows local time, eth0.2 DHCP address, and GPSD fix status.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -O2 \
		-o $(PKG_BUILD_DIR)/yspeed-display $(PKG_BUILD_DIR)/yspeed-display.c
endef

define Package/yspeed-display/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/yspeed-display $(1)/usr/sbin/yspeed-display
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/yspeed-display.init $(1)/etc/init.d/yspeed-display
endef

define Package/yspeed-display/postinst
#!/bin/sh
if [ -n "${IPKG_INSTROOT}" ]; then
	mkdir -p "${IPKG_INSTROOT}/etc/rc.d"
	ln -sf ../init.d/yspeed-display "${IPKG_INSTROOT}/etc/rc.d/S95yspeed-display"
else
	/etc/init.d/yspeed-display enable
fi
exit 0
endef

$(eval $(call BuildPackage,yspeed-display))
