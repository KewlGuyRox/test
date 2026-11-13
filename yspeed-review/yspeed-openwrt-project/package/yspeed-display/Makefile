include $(TOPDIR)/rules.mk

PKG_NAME:=yspeed-display
PKG_VERSION:=2.0
PKG_RELEASE:=1
PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)

include $(INCLUDE_DIR)/package.mk

define Package/yspeed-display
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=YSPEED ST7789 userspace SPI GNSS display
  DEPENDS:=+gpsd
endef

define Package/yspeed-display/description
  English userspace ST7789 display using SPI0 CS1 (/dev/spidev0.1)
  and GPIO sysfs for D/C, reset and backlight. No framebuffer driver.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -Wall -Wextra -O2 \
		-o $(PKG_BUILD_DIR)/yspeed-display $(PKG_BUILD_DIR)/yspeed-display.c
endef

# st7789_display is the vendor's own statically linked MIPS/musl program, the
# one that lights this panel on the stock firmware. Shipping it lets us run a
# known-good driver against our kernel instead of guessing.
define Package/yspeed-display/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/yspeed-display $(1)/usr/sbin/yspeed-display
	$(INSTALL_BIN) ./files/st7789_display $(1)/usr/sbin/st7789_display
	$(INSTALL_BIN) ./files/yspeed-antenna $(1)/usr/sbin/yspeed-antenna
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/yspeed-display.init $(1)/etc/init.d/yspeed-display
	$(INSTALL_BIN) ./files/yspeed-antenna.init $(1)/etc/init.d/yspeed-antenna
endef

define Package/yspeed-display/postinst
#!/bin/sh
if [ -n "$${IPKG_INSTROOT}" ]; then
	mkdir -p "$${IPKG_INSTROOT}/etc/rc.d"
	ln -sf ../init.d/yspeed-display "$${IPKG_INSTROOT}/etc/rc.d/S95yspeed-display"
	ln -sf ../init.d/yspeed-antenna "$${IPKG_INSTROOT}/etc/rc.d/S96yspeed-antenna"
else
	/etc/init.d/yspeed-display enable
	/etc/init.d/yspeed-antenna enable
fi
exit 0
endef

$(eval $(call BuildPackage,yspeed-display))
