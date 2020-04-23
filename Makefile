# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_BCM2835_VCHIQ)	+= vchiq.o

vchiq-objs := \
   interface/vchiq_arm/vchiq_core.o  \
   interface/vchiq_arm/vchiq_arm.o \
   interface/vchiq_arm/vchiq_2835_arm.o \
   interface/vchiq_arm/vchiq_debugfs.o \
   interface/vchiq_arm/vchiq_shim.o \
   interface/vchiq_arm/vchiq_util.o \
   interface/vchiq_arm/vchiq_connected.o \

obj-$(CONFIG_SND_BCM2835)		+= bcm2835-audio/
obj-$(CONFIG_VIDEO_BCM2835)		+= bcm2835-camera/
obj-$(CONFIG_BCM2835_VCHIQ_MMAL)	+= vchiq-mmal/
obj-$(CONFIG_BCM_VC_SM_CMA) 		+= vc-sm-cma/
obj-$(CONFIG_VIDEO_CODEC_BCM2835)	+= bcm2835-codec/
obj-$(CONFIG_VIDEO_ISP_BCM2835)		+= bcm2835-isp/

ccflags-y += -Idrivers/staging/vc04_services -D__VCCOREVER__=0x04000000

