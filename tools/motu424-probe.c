// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * motu424-probe - userspace helper for reverse engineering the MOTU
 * PCI-324/424 register map.
 *
 * It scans /sys/bus/pci for Mark of the Unicorn devices (vendor 0x137A),
 * prints their identity, and - when run as root with a matching card - mmaps
 * BAR0 (sysfs resource0) and dumps its contents. Run it repeatedly while the
 * card is idle vs. streaming (under the vendor OS) and diff the dumps to locate
 * status/position registers.
 *
 * NB: the card uses a windowed MMIO model (see docs/register-map.md), so the
 * registers of interest (e.g. bank ctrl/status at window-B card offset
 * 0xC0024) are NOT in the first bytes of a BAR - a real diffing run needs a
 * larger, targeted dump once the BAR<->window mapping is confirmed on hardware.
 *
 * NOTE: this reads MMIO from userspace via /sys/.../resource0. The in-tree
 * kernel driver must NOT be bound to the device at the same time.
 *
 *   sudo ./motu424-probe            # scan + dump BAR0 of the first MOTU card
 *   sudo ./motu424-probe 0000:01:00.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define MOTU_VENDOR 0x137A	/* Mark of the Unicorn (confirmed, MOTUAW.inf) */
#define SYS_PCI     "/sys/bus/pci/devices"
#define DUMP_BYTES  0x100	/* BAR0 is small; adjust once its size is known */

static unsigned read_hex_file(const char *dir, const char *file)
{
	char path[512];
	unsigned val = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%x", &val) != 1)
		val = 0;
	fclose(f);
	return val;
}

static int dump_bar0(const char *bdf)
{
	char path[512];
	int fd;
	struct stat st;
	volatile uint32_t *bar;
	size_t map_len;

	snprintf(path, sizeof(path), "%s/%s/resource0", SYS_PCI, bdf);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open resource0 (need root, and driver unbound)");
		return -1;
	}
	if (fstat(fd, &st) < 0) {
		perror("fstat");
		close(fd);
		return -1;
	}

	map_len = (size_t)st.st_size < DUMP_BYTES ? (size_t)st.st_size
						  : DUMP_BYTES;
	if (map_len == 0)
		map_len = DUMP_BYTES;

	bar = mmap(NULL, map_len, PROT_READ, MAP_SHARED, fd, 0);
	if (bar == MAP_FAILED) {
		perror("mmap BAR0");
		close(fd);
		return -1;
	}

	printf("  BAR0 dump (%zu bytes):\n", map_len);
	for (size_t off = 0; off + 4 <= map_len; off += 16) {
		printf("    %04zx:", off);
		for (size_t w = 0; w < 16 && off + w + 4 <= map_len; w += 4)
			printf(" %08x", bar[(off + w) / 4]);
		printf("\n");
	}

	munmap((void *)bar, map_len);
	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	DIR *d;
	struct dirent *e;
	const char *want = argc > 1 ? argv[1] : NULL;
	int found = 0;

	d = opendir(SYS_PCI);
	if (!d) {
		perror("opendir " SYS_PCI);
		return 1;
	}

	while ((e = readdir(d))) {
		char dir[512];
		unsigned vendor, device;

		if (e->d_name[0] == '.')
			continue;
		if (want && strcmp(e->d_name, want))
			continue;

		snprintf(dir, sizeof(dir), "%s/%s", SYS_PCI, e->d_name);
		vendor = read_hex_file(dir, "vendor");
		if (vendor != MOTU_VENDOR && !want)
			continue;

		device = read_hex_file(dir, "device");
		printf("MOTU PCI device %s: vendor=%04x device=%04x (%s)\n",
		       e->d_name, vendor, device,
		       device == 0x0003 ? "PCI-324" :
		       device == 0x0004 ? "PCI-424" :
		       device == 0x0005 ? "PCIe-424 (HD Express)" : "unknown/other");
		found++;
		dump_bar0(e->d_name);
	}
	closedir(d);

	if (!found) {
		fprintf(stderr,
			"No MOTU (vendor %04x) PCI device found.\n"
			"If the card is present, check `lspci -nn | grep %04x`.\n",
			MOTU_VENDOR, MOTU_VENDOR);
		return 2;
	}
	return 0;
}
