sc0710-objs := \
	lib/sc0710-cards.o lib/sc0710-core.o lib/sc0710-i2c.o \
	lib/sc0710-dma-channel.o lib/sc0710-dma-channels.o \
	lib/sc0710-dma-chains.o lib/sc0710-dma-chain.o \
	lib/sc0710-things-per-second.o lib/sc0710-video.o \
	lib/sc0710-audio.o lib/sc0710-tonemap.o

obj-m += sc0710.o

# User/DKMS entry (KBUILD_EXTMOD unset). kbuild sets KBUILD_EXTMOD when it
# includes this Makefile — do not define "all" there or kbuild recurses.
ifeq ($(KBUILD_EXTMOD),)

.DEFAULT_GOAL := all

TARFILES = Makefile lib/*.h lib/*.c *.txt *.md

KVERSION ?= $(shell uname -r)
KERNEL_VER := $(if $(KERNELRELEASE),$(KERNELRELEASE),$(KVERSION))
VERSION_HDR := lib/sc0710-version.h
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

$(VERSION_HDR): version
	@VERSION="$$(head -n 1 version | tr -d '[:space:]')"; \
	printf '%s\n' \
		'/* Auto-generated from version — do not edit manually. */' \
		'#ifndef SC0710_VERSION_H' \
		'#define SC0710_VERSION_H' \
		'' \
		"#define SC0710_DRV_VERSION_STRING \"$$VERSION\"" \
		'' \
		'#endif /* SC0710_VERSION_H */' \
		> "$(VERSION_HDR)"

all: $(VERSION_HDR)
	@# kbuild writes its .o/.cmd next to the sources, so stage the sources into
	@# build/ and build there; the source tree stays clean and everything
	@# (objects, sc0710.ko, Module.symvers) lands under build/. DKMS reads
	@# build/sc0710.ko (BUILT_MODULE_LOCATION), unchanged.
	@mkdir -p "$(MODULE_OUTPUT_DIR)/lib"
	@# cp -u only adds, so prune staged sources deleted/renamed in lib/ or
	@# they keep satisfying stale #includes.
	@for f in "$(MODULE_OUTPUT_DIR)/lib"/*.c "$(MODULE_OUTPUT_DIR)/lib"/*.h; do \
		[ -e "$$f" ] || continue; \
		[ -e "lib/$${f##*/}" ] || rm -f "$$f"; \
	done
	@cp -u Makefile "$(MODULE_OUTPUT_DIR)/Makefile"
	@cp -u lib/*.c lib/*.h "$(MODULE_OUTPUT_DIR)/lib/"
	@ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || ulimit -n 4096 2>/dev/null || true; \
	$(MAKE) -j1 -C "$(KBUILD_DIR)" M="$(MODULE_OUTPUT_DIR)" $(KBUILD_CC) modules

clean:
	rm -rf "$(MODULE_OUTPUT_DIR)"
	@# Legacy: sweep artifacts left in the tree by older in-place builds.
	rm -f sc0710.ko sc0710.o sc0710.mod sc0710.mod.c sc0710.mod.o .module-common.o \
		Module.symvers modules.order .sc0710*.cmd .module-common*.cmd .modules*.cmd \
		lib/*.o lib/.*.cmd 2>/dev/null || true
	rm -rf .tmp_versions

load: all
	sudo dmesg -c >/dev/null
	sudo cp /dev/null /var/log/debug
	# insmod (unlike modprobe) won't pull dependencies, so a clean environment fails
	# with "Unknown symbol in module". Load exactly the modules modpost recorded in the
	# built .ko's "depends" field (videodev, videobuf2-*, snd, snd-pcm).
	D=$$(modinfo -F depends ./build/sc0710.ko | tr ',' ' '); [ -z "$$D" ] || sudo modprobe -a $$D
	sudo insmod ./build/sc0710.ko $(LOAD_ARGS)

# The low-latency test configuration: DMA straight into the client's buffers,
# completion service woken by the card's interrupt (the default).
load-zc: LOAD_ARGS = zero_copy=1
load-zc: load

unload:
	# Plain rmmod refuses while something holds /dev/video* open - that refusal
	# is the safety net. Never rmmod -f here: force-unloading with an open
	# capture fd is a use-after-free panic. Note PipeWire/wireplumber holds
	# video nodes open persistently even with no capture app running; fuser
	# prints its listing on stderr, so don't silence it.
	sudo rmmod sc0710 || { \
		echo "module busy - holders of the video nodes:"; \
		sudo fuser -v /dev/video* || true; \
		echo "PipeWire holds nodes without any app open: use scripts/sc0710-cli.sh --unload"; \
		echo "(stops PipeWire, unloads, restarts it) or close the processes listed above."; \
		exit 1; }
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
