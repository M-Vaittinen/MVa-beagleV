################################################################################
#
# sampler
#
################################################################################

SAMPLER_VERSION = 1.0
SAMPLER_SITE = ../sampler/src
SAMPLER_SITE_METHOD = local

# adc_capture_view links against ncurses; make sure buildroot builds/stages
# it for the target before we try to build the sampler package. NOTE: as
# of this writing BR2_PACKAGE_NCURSES is NOT enabled in this tree's
# .config - enable it (e.g. `make ncurses-menuconfig` or set
# BR2_PACKAGE_NCURSES=y) before building, otherwise this dependency will
# fail to resolve.
SAMPLER_DEPENDENCIES = ncurses

include $(sort $(wildcard $(BR2_EXTERNAL_PATH)/package/*/*.mk))

define SAMPLER_BUILD_CMDS
$(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D)
endef

# LIBIIO_WRAPPER = libiio_wrapper_but_python_shouldnt_import_this.so

define SAMPLER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/setupiio $(TARGET_DIR)/bin/setupiio

	# Install local network and sshd configurations
	$(INSTALL) -m 0644 -D $(@D)/interfaces $(TARGET_DIR)/etc/network/interfaces
	$(INSTALL) -m 0644 -D $(@D)/sshd_config $(TARGET_DIR)/etc/ssh/sshd_config

	# Install LibIIO wrapper
	$(INSTALL) -m 0755 -D $(@D)/*.so $(TOPDIR)/../ui/

	# Install standalone ADC capture test/demo tool
	$(INSTALL) -m 0755 -D $(@D)/adc_capture $(TARGET_DIR)/usr/bin/adc_capture

	# Install ncurses-based capture file viewer/analyser
	$(INSTALL) -m 0755 -D $(@D)/adc_capture_view $(TARGET_DIR)/usr/bin/adc_capture_view

	# Install libIIO-free capture tool (sysfs + /dev/iio:deviceN only,
	# no extra runtime dependencies beyond libc)
	$(INSTALL) -m 0755 -D $(@D)/adc_direct_capture $(TARGET_DIR)/usr/bin/adc_direct_capture
endef

$(eval $(generic-package))
