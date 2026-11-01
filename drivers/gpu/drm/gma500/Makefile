# SPDX-License-Identifier: GPL-2.0
#
#	KMS driver for the GMA500
#

gma500_gfx-y += \
	  backlight.o \
	  cdv_device.o \
	  cdv_intel_crt.o \
	  cdv_intel_display.o \
	  cdv_intel_dp.o \
	  cdv_intel_hdmi.o \
	  cdv_intel_lvds.o \
	  framebuffer.o \
	  gem.o \
	  gma_device.o \
	  gma_display.o \
	  gtt.o \
	  intel_bios.o \
	  intel_gmbus.o \
	  intel_i2c.o \
	  mid_bios.o \
	  mmu.o \
	  oaktrail_device.o \
	  oaktrail_crtc.o \
	  oaktrail_hdmi.o \
	  oaktrail_hdmi_i2c.o \
	  oaktrail_lvds.o \
	  oaktrail_lvds_i2c.o \
	  power.o \
	  psb_device.o \
	  psb_drv.o \
	  psb_intel_display.o \
	  psb_intel_lvds.o \
	  psb_intel_modes.o \
	  psb_intel_sdvo.o \
	  psb_irq.o

gma500_gfx-$(CONFIG_ACPI) +=  opregion.o

# SGX545 GPU (Cedarview)
gma500_gfx-y += gma_sgx.o
gma500_gfx-y += sgxfive/sgx_fw.o
gma500_gfx-y += sgxfive/sgx_mmu.o
gma500_gfx-y += sgxfive/sgx_seq.o
gma500_gfx-y += sgxfive/sgx_xfer.o
gma500_gfx-y += sgxfive/sgxfive.o
gma500_gfx-y += sgxfive/sgxfive_drv.o
gma500_gfx-y += sgxfive/sgxfive_vm.o
gma500_gfx-$(CONFIG_DRM_GMA500_SGXFIVE_DEBUG) += sgxfive/sgxfive_regdump.o
gma500_gfx-$(CONFIG_DRM_GMA500_SGXFIVE_DEBUG) += sgxfive/sgxfive_debug.o

gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgx_fw_test.o
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgx_mmu_test.o
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgx_seq_test.o
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgx_xfer_test.o
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgxfive_test.o
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgxfive_drv_test.o
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgxfive_vm_test.o
ifdef CONFIG_DRM_GMA500_SGXFIVE_DEBUG
gma500_gfx-$(CONFIG_DRM_GMA500_KUNIT_TEST) += sgxfive/sgxfive_regdump_test.o
endif

gma500_gfx-$(CONFIG_DRM_FBDEV_EMULATION) +=  fbdev.o

obj-$(CONFIG_DRM_GMA500) += gma500_gfx.o
