// mk2-set-edid: write a raw EDID .bin to an sc0710 MK.2 via VIDIOC_S_EDID.
// Bypasses v4l2-ctl's hex-text file parsing (which rejects raw binaries).
//   build: cc -o mk2-set-edid scripts/mk2-set-edid.c
//   run:   sudo ./mk2-set-edid /dev/video0 mk2_fw/EDID_4K60_Pro_MK2.bin
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s /dev/videoN edid.bin\n", argv[0]);
		return 2;
	}
	unsigned char buf[512];
	FILE *f = fopen(argv[2], "rb");
	if (!f) { perror("open edid file"); return 1; }
	size_t n = fread(buf, 1, sizeof buf, f);
	fclose(f);
	if (n == 0 || (n % 128) != 0) {
		fprintf(stderr, "bad EDID size %zu (must be a non-zero multiple of 128)\n", n);
		return 1;
	}
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) { perror("open video device"); return 1; }

	struct v4l2_edid e;
	memset(&e, 0, sizeof e);
	e.pad = 0;
	e.start_block = 0;
	e.blocks = n / 128;
	e.edid = buf;
	if (ioctl(fd, VIDIOC_S_EDID, &e) < 0) {
		perror("VIDIOC_S_EDID");
		close(fd);
		return 1;
	}
	printf("wrote %zu bytes (%u blocks) to %s\n", n, e.blocks, argv[1]);
	close(fd);
	return 0;
}
