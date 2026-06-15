sc0710-objs := \
	lib/sc0710-cards.o lib/sc0710-core.o lib/sc0710-i2c.o \
	lib/sc0710-dma-channel.o lib/sc0710-dma-channels.o \
	lib/sc0710-dma-chains.o lib/sc0710-dma-chain.o \
	lib/sc0710-things-per-second.o lib/sc0710-video.o \
	lib/sc0710-audio.o lib/sc0710-scaler.o

obj-m += sc0710.o

# User/DKMS entry (KBUILD_EXTMOD unset). kbuild sets KBUILD_EXTMOD when it
# includes this Makefile — do not define "all" there or kbuild recurses.
ifeq ($(KBUILD_EXTMOD),)

.DEFAULT_GOAL := all

TARFILES = Makefile lib/*.h lib/*.c *.txt *.md

KVERSION ?= $(shell uname -r)
KERNEL_VER := $(if $(KERNELRELEASE),$(KERNELRELEASE),$(KVERSION))
VERSION_HDR := lib/sc0710-version.h
SYNC_VERSION := scripts/sync-version-header.sh
KBUILD_DIR = /lib/modules/$(KERNEL_VER)/build
MODULE_OUTPUT_DIR := $(CURDIR)/build

# Auto-detect kernel compiler.
# Modern distributions correctly use the LLVM toolchain.
# We must force LLVM if the kernel was built with it, explicitly
# ignoring legacy GCC variables that DKMS might inject.
KERNEL_IS_CLANG := $(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KBUILD_DIR)/.config 2>/dev/null)
ifneq ($(KERNEL_IS_CLANG),)
  $(info Auto-detected Clang-built kernel, forcing LLVM toolchain)
  KBUILD_CC = CC=clang LLVM=1
endif

$(VERSION_HDR): version $(SYNC_VERSION)
	bash "$(SYNC_VERSION)"

all: $(VERSION_HDR)
	@mkdir -p "$(MODULE_OUTPUT_DIR)"
	$(MAKE) -C "$(KBUILD_DIR)" M="$(CURDIR)" MO="$(MODULE_OUTPUT_DIR)" $(KBUILD_CC) modules
	@# Fallback for kernels < 6.13 where MO= is not supported:
	@# the .ko ends up at the source root instead of build/.
	@if [ -f "$(CURDIR)/sc0710.ko" ] && [ ! -f "$(MODULE_OUTPUT_DIR)/sc0710.ko" ]; then \
		cp "$(CURDIR)/sc0710.ko" "$(MODULE_OUTPUT_DIR)/sc0710.ko"; \
	fi

clean:
	rm -rf "$(MODULE_OUTPUT_DIR)"/*.o "$(MODULE_OUTPUT_DIR)"/*.ko "$(MODULE_OUTPUT_DIR)"/*.mod \
		"$(MODULE_OUTPUT_DIR)"/*.mod.c "$(MODULE_OUTPUT_DIR)"/*.mod.o "$(MODULE_OUTPUT_DIR)"/.*.cmd \
		"$(MODULE_OUTPUT_DIR)"/.tmp_versions "$(MODULE_OUTPUT_DIR)"/lib
	rm -f sc0710.ko sc0710.o sc0710.mod sc0710.mod.c sc0710.mod.o .module-common.o \
		Module.symvers modules.order .sc0710*.cmd .module-common*.cmd .modules*.cmd
	rm -rf .tmp_versions
	$(MAKE) -C "$(KBUILD_DIR)" M="$(CURDIR)" MO="$(MODULE_OUTPUT_DIR)" clean 2>/dev/null || true

load: all
	sudo dmesg -c >/dev/null
	sudo cp /dev/null /var/log/debug
	#sudo modprobe videobuf2-core
	sudo modprobe videobuf2-common
	sudo modprobe videodev
	#sudo modprobe videobuf-dma-sg
	sudo modprobe videobuf2-vmalloc
	sudo insmod ./build/sc0710.ko \
		thread_dma_poll_interval_ms=2 \
		dma_status=0

unload:
	# Only real way to remove the module due to the module not dereferencing itself
	# Decent chance this causes kernel issues if unlucky
	sudo rmmod -f sc0710
	sync

tarball:
	tar zcf ../sc0710-dev-$(shell date +%Y%m%d-%H%M%S).tgz $(TARFILES)

deps:
	sudo yum -y install v4l-utils

test:
	dd if=/dev/video0 of=frame.bin bs=1843200 count=20

encode:
	#ffmpeg -f rawvideo -pixel_format uyvy422 -video_size 1280x720 -i /dev/video0 -vcodec libx264 -f mpegts encoder2.ts
	#ffmpeg -f rawvideo -pixel_format yuyv422 -video_size 1280x720 -i /dev/video0 -vcodec libx264 -f mpegts encoder3.ts
	ffmpeg -r 59.94 -f rawvideo -pixel_format yuyv422 -video_size 1280x720 -i /dev/video0 -vcodec libx264 -f mpegts encoder0.ts

stream720p:
	ffmpeg -r 59.94 -f rawvideo -pixel_format yuyv422 -video_size 1280x720 -i /dev/video0 \
		-vcodec libx264 -preset ultrafast -tune zerolatency \
		-f mpegts udp://192.168.0.200:4001?pkt_size=1316

stream720pAudio:
	ffmpeg -r 59.94 -f rawvideo -pixel_format yuyv422 -video_size 1280x720 -i /dev/video0 \
		-f alsa -ac 2 -ar 48000 -i hw:2,0 \
		-vcodec libx264 -preset ultrafast -tune zerolatency \
		-acodec mp2 \
		-f mpegts udp://192.168.0.200:4001?pkt_size=1316


stream720p10:
	ffmpeg -r 59.94 -f rawvideo -pixel_format yuv422p10le -video_size 1280x720 -i /dev/video0 \
		-vcodec libx264 -preset ultrafast -tune zerolatency \
		-f mpegts udp://192.168.0.66:4001?pkt_size=1316

stream1080p:
	ffmpeg -r 59.94 -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -i /dev/video0 \
		-vcodec libx264 -preset ultrafast -tune zerolatency \
		-f mpegts udp://192.168.0.66:4001?pkt_size=1316

stream1080pAudio:
	ffmpeg -r 59.94 -f rawvideo -pixel_format yuyv422 -video_size 1920x1080 -i /dev/video0 \
		-f alsa -ac 2 -ar 48000 -i hw:2,0 \
		-vcodec libx264 -preset ultrafast -tune zerolatency \
		-acodec mp2 \
		-f mpegts udp://192.168.0.66:4001?pkt_size=1316

stream2160p:
	ffmpeg -r 30 -f rawvideo -pixel_format yuyv422 -video_size 3840x2160 -i /dev/video0 \
		-vcodec libx264 -preset ultrafast -tune zerolatency \
		-f mpegts udp://192.168.0.66:4001?pkt_size=1316

dumpaudioparams:
	arecord --dump-hw-params -D hw:2,0

dvtimings:
	v4l2-ctl --get-dv-timings

10bitAVC:
	./ffmpeg -y -r 59.94 -f rawvideo -pixel_format yuv422p10le -video_size 1920x1080 -i /dev/video0 \
		-vcodec libx264 -pix_fmt yuv420p10le -preset ultrafast -tune zerolatency \
		-f mpegts recording.ts

10bitHEVC:
	./ffmpeg-hevc -y -r 59.94 -f rawvideo -pixel_format yuv422p10le -video_size 1920x1080 -i /dev/video0 \
		-vcodec libx265 -pix_fmt yuv422p10le -preset ultrafast -tune zerolatency \
		-f mpegts recording.ts


probe:
	# See https://codecalamity.com/encoding-uhd-4k-hdr10-videos-with-ffmpeg/
	./ffprobe-hevc -hide_banner -loglevel warning -select_streams v -print_format json -show_frames \
		-read_intervals "%+#1" -show_entries "frame=color_space,color_primaries,color_transfer,side_data_list,pix_fmt" -i recording.ts

#yuv422p10le 10bit 4:2:2

endif
