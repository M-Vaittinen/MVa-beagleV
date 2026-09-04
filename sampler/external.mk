################################################################################
#
# sampler
#
################################################################################

SAMPLER_VERSION = 1.0
SAMPLER_SITE = ../sampler/src
SAMPLER_SITE_METHOD = local

define SAMPLER_BUILD_CMDS
	$(MAKE) -C $(@D)
endef

# LIBIIO_WRAPPER = libiio_wrapper_but_python_shouldnt_import_this.so

define SAMPLER_INSTALL_TARGET_CMDS
	$(INSTALL) -m 0755 -D $(@D)/setupiio $(TARGET_DIR)/bin/setupiio

	# Install local network and sshd configurations
	$(INSTALL) -m 0644 -D $(@D)/interfaces $(TARGET_DIR)/etc/network/interfaces
	$(INSTALL) -m 0644 -D $(@D)/sshd_config $(TARGET_DIR)/etc/ssh/sshd_config

	# Install LibIIO wrapper
	$(INSTALL) -m 0755 -D $(@D)/*.so $(TOPDIR)/../ui/
endef

$(eval $(generic-package))
