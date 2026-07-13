/*
 * mk2-set-tonemap — MK.2 hardware HDR→SDR control.
 *
 * Preferred path (Windows KS prop 722):
 *   sudo mk2-set-tonemap on|off
 *   → writes /sys/module/sc0710/parameters/hw_tonemap (MCU 0x32 sub 0x11)
 *
 * Optional custom curve upload (MCU fn 0x63 / KS prop 723):
 *   sudo mk2-set-tonemap passthrough|pq|…
 *
 * build: cc -O2 -lm -o scripts/mk2-set-tonemap scripts/mk2-set-tonemap.c
 */
#include <glob.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define LUT_SAMPLES 512
#define LUT_BYTES   (LUT_SAMPLES * 2)

static void put_u16le(uint8_t *buf, int i, uint16_t v)
{
	buf[i * 2] = (uint8_t)(v & 0xff);
	buf[i * 2 + 1] = (uint8_t)(v >> 8);
}

static void fill_u16_flat(uint8_t *buf, uint16_t v)
{
	int i;

	for (i = 0; i < LUT_SAMPLES; i++)
		put_u16le(buf, i, v);
}

static void fill_passthrough(uint8_t *buf)
{
	memset(buf, 0x01, LUT_BYTES);
}

static void fill_identity(uint8_t *buf)
{
	int i;

	for (i = 0; i < LUT_SAMPLES; i++)
		put_u16le(buf, i, (uint16_t)((i * 65535) / (LUT_SAMPLES - 1)));
}

static double pq_eotf(double N)
{
	const double m1 = 2610.0 / 4096.0 / 4.0;
	const double m2 = 2523.0 / 4096.0 * 128.0;
	const double c1 = 3424.0 / 4096.0;
	const double c2 = 2413.0 / 4096.0 * 32.0;
	const double c3 = 2392.0 / 4096.0 * 32.0;
	double Np, num, den;

	if (N <= 0.0)
		return 0.0;
	Np = pow(N, 1.0 / m2);
	num = Np - c1;
	if (num < 0.0)
		num = 0.0;
	den = c2 - c3 * Np;
	if (den <= 0.0)
		return 0.0;
	return pow(num / den, 1.0 / m1);
}

static double soft_shoulder(double y, double shoulder)
{
	if (y <= 1.0)
		return y;
	return 1.0 + (y - 1.0) / (1.0 + (y - 1.0) / shoulder);
}

static void fill_pq(uint8_t *buf, double paper_nits, double shoulder)
{
	int i;

	for (i = 0; i < LUT_SAMPLES; i++) {
		double N = (double)i / (double)(LUT_SAMPLES - 1);
		double nits = pq_eotf(N) * 10000.0;
		double y = soft_shoulder(nits / paper_nits, shoulder);
		double sdr;
		int v;

		if (y < 0.0)
			y = 0.0;
		if (y > 1.0)
			y = 1.0;
		sdr = pow(y, 1.0 / 2.2);
		v = (int)lround(sdr * 65535.0);
		if (v < 0)
			v = 0;
		if (v > 65535)
			v = 65535;
		put_u16le(buf, i, (uint16_t)v);
	}
}

static void fill_pq_dim(uint8_t *buf)
{
	const double ref_nits = 100.0;
	int i;

	for (i = 0; i < LUT_SAMPLES; i++) {
		double N = (double)i / (double)(LUT_SAMPLES - 1);
		double nits = pq_eotf(N) * 10000.0;
		double y = nits / ref_nits;
		double sdr;
		int v;

		y = y / (1.0 + y);
		if (y < 0.0)
			y = 0.0;
		if (y > 1.0)
			y = 1.0;
		sdr = pow(y, 1.0 / 2.4);
		v = (int)lround(sdr * 65535.0);
		if (v < 0)
			v = 0;
		if (v > 65535)
			v = 65535;
		put_u16le(buf, i, (uint16_t)v);
	}
}

static int find_sysfs(char *out, size_t out_sz)
{
	glob_t g;
	int rc;

	rc = glob("/sys/bus/pci/drivers/sc0710/*/hdr_tonemap", 0, NULL, &g);
	if (rc != 0 || g.gl_pathc < 1) {
		globfree(&g);
		return -1;
	}
	snprintf(out, out_sz, "%s", g.gl_pathv[0]);
	globfree(&g);
	return 0;
}

static int upload(const char *path, const uint8_t *buf)
{
	FILE *f = fopen(path, "wb");
	size_t n;

	if (!f) {
		perror(path);
		return 1;
	}
	n = fwrite(buf, 1, LUT_BYTES, f);
	fclose(f);
	if (n != LUT_BYTES) {
		fprintf(stderr, "short write (%zu of %d)\n", n, LUT_BYTES);
		return 1;
	}
	printf("uploaded %d bytes to %s\n", LUT_BYTES, path);
	return 0;
}

static int set_hw_enable(int on)
{
	FILE *f;
	const char *path = "/sys/module/sc0710/parameters/hw_tonemap";
	const char *sw = "/sys/module/sc0710/parameters/sw_tonemap";

	f = fopen(path, "w");
	if (!f) {
		perror(path);
		fprintf(stderr, "Is sc0710 loaded with hw_tonemap?\n");
		return 1;
	}
	fprintf(f, "%d\n", on ? 2 : 0);
	fclose(f);

	f = fopen(sw, "w");
	if (f) {
		fprintf(f, "0\n");
		fclose(f);
	}
	printf("hw_tonemap=%s sw_tonemap=0 (MCU 0x32 sub 0x11)\n",
	       on ? "2 (force on)" : "0 (off)");
	printf("Check dmesg for: hw_tonemap MCU 0x11=%d\n", on ? 1 : 0);
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <mode> [--write PATH] [--dump PATH]\n"
		"\n"
		"MCU enable (Windows KS prop 722 — preferred):\n"
		"  on / enable     force hardware tonemap on, disable CPU tonemap\n"
		"  off / disable   force hardware tonemap off\n"
		"\n"
		"Optional custom LUT (MCU fn 0x63):\n"
		"  passthrough     empirical all-0x01 sentinel\n"
		"  pq|pq-bright|pq-dim|identity   EXPERIMENTAL (may scramble)\n"
		"  flat <u16>      debug\n"
		"\n"
		"Default Elgato look needs 'on' only — firmware-baked curve.\n",
		argv0);
}

int main(int argc, char **argv)
{
	uint8_t buf[LUT_BYTES];
	char sysfs[512];
	const char *write_path = NULL;
	const char *dump_path = NULL;
	const char *mode = NULL;
	int flat_val = -1;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--write") && i + 1 < argc) {
			write_path = argv[++i];
		} else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
			dump_path = argv[++i];
		} else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 2;
		} else if (!strcmp(argv[i], "flat") && i + 1 < argc) {
			mode = "flat";
			flat_val = atoi(argv[++i]);
			if (flat_val < 0 || flat_val > 65535) {
				fprintf(stderr, "flat value must be 0..65535\n");
				return 2;
			}
		} else if (!mode) {
			mode = argv[i];
		} else {
			fprintf(stderr, "unexpected arg: %s\n", argv[i]);
			usage(argv[0]);
			return 2;
		}
	}

	if (!mode) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(mode, "on") || !strcmp(mode, "true") ||
	    !strcmp(mode, "enable"))
		return set_hw_enable(1);
	if (!strcmp(mode, "off") || !strcmp(mode, "false") ||
	    !strcmp(mode, "disable"))
		return set_hw_enable(0);

	if (!strcmp(mode, "pq") || !strcmp(mode, "pq-bright") ||
	    !strcmp(mode, "pq-dim") || !strcmp(mode, "identity")) {
		fprintf(stderr,
			"warning: '%s' is a synthetic LUT — prefer:\n"
			"  sudo %s on\n"
			"Continuing upload anyway...\n",
			mode, argv[0]);
	}

	if (!strcmp(mode, "pq")) {
		fill_pq(buf, 203.0, 1.5);
		printf("mode: pq [EXPERIMENTAL]\n");
	} else if (!strcmp(mode, "pq-bright")) {
		fill_pq(buf, 100.0, 2.0);
		printf("mode: pq-bright [EXPERIMENTAL]\n");
	} else if (!strcmp(mode, "pq-dim")) {
		fill_pq_dim(buf);
		printf("mode: pq-dim [EXPERIMENTAL]\n");
	} else if (!strcmp(mode, "identity")) {
		fill_identity(buf);
		printf("mode: identity [EXPERIMENTAL]\n");
	} else if (!strcmp(mode, "passthrough")) {
		fill_passthrough(buf);
		printf("mode: passthrough (LUT sentinel)\n");
	} else if (!strcmp(mode, "flat")) {
		fill_u16_flat(buf, (uint16_t)flat_val);
		printf("mode: flat u16=%d\n", flat_val);
	} else {
		usage(argv[0]);
		return 2;
	}

	if (dump_path) {
		FILE *f = fopen(dump_path, "wb");

		if (!f) {
			perror(dump_path);
			return 1;
		}
		if (fwrite(buf, 1, LUT_BYTES, f) != LUT_BYTES) {
			fprintf(stderr, "short dump write\n");
			fclose(f);
			return 1;
		}
		fclose(f);
		printf("wrote %d bytes to %s\n", LUT_BYTES, dump_path);
	}

	if (!write_path) {
		if (find_sysfs(sysfs, sizeof(sysfs)) != 0) {
			fprintf(stderr,
				"hdr_tonemap sysfs not found (MK.2 + loaded driver?)\n");
			return 1;
		}
		write_path = sysfs;
	}
	return upload(write_path, buf);
}
