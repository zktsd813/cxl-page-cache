/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/cxl_pagecache_reg.h>

_Static_assert(sizeof(struct pcreg_record) == 32, "record ABI");
_Static_assert(sizeof(struct pcreg_file_identity) == 64, "identity ABI");
_Static_assert(sizeof(struct pcreg_digest_header) == 72, "digest header ABI");
_Static_assert(sizeof(struct pcreg_status) == 208, "status ABI");
_Static_assert(offsetof(struct pcreg_status, records_sha256) == 88, "digest offset");

static void fail(const char *why)
{
	perror(why);
	exit(1); /* Closing the owner FD invalidates the kernel authority first. */
}

static uint64_t number(const char *text, int allow_zero)
{
	char *end;
	unsigned long long value;
	errno = 0;
	value = strtoull(text, &end, 10);
	if (errno || !*text || *text == '-' || *end || (!value && !allow_zero)) {
		fprintf(stderr, "invalid integer: %s\n", text);
		exit(2);
	}
	return value;
}

static void hex(char *out, const unsigned char *in, size_t bytes)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < bytes; i++) {
		out[2*i] = digits[in[i] >> 4];
		out[2*i+1] = digits[in[i] & 15];
	}
	out[2*bytes] = 0;
}

static void image_id(const char *text, unsigned char out[32])
{
	if (strlen(text) != 64) {
		fprintf(stderr, "image digest must contain 64 hex digits\n");
		exit(2);
	}
	for (unsigned int i = 0; i < 32; i++) {
		char byte[3] = {text[2*i], text[2*i+1], 0}, *end;
		unsigned long value;
		if (!strchr("0123456789abcdefABCDEF", byte[0]) ||
		    !strchr("0123456789abcdefABCDEF", byte[1])) {
			fprintf(stderr, "invalid image digest\n");
			exit(2);
		}
		value = strtoul(byte, &end, 16);
		if (*end || value > 255)
			exit(2);
		out[i] = value;
	}
}

static void print_status(int device)
{
	struct pcreg_status status;
	char digest[65];
	if (ioctl(device, PCREG_STATUS, &status))
		fail("status");
	hex(digest, status.records_sha256, 32);
	printf("{\"kind\":\"pcreg_status\",\"state\":%u,\"session_cookie\":%llu,"
	       "\"epoch\":%llu,\"plan_generation\":%llu,\"record_count\":%llu,"
	       "\"records_sha256\":\"%s\",\"prepare_mono_ns\":%llu,\"release_mono_ns\":%llu}\n",
	       status.state, status.session_cookie, status.epoch, status.plan_generation,
	       status.record_count, digest, status.prepare_mono_ns, status.release_mono_ns);
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct pcreg_prepare prepare = {.version = PCREG_ABI,
		.max_records = PCREG_MAX_PAGES};
	struct pcreg_record records[1024];
	char digest[65], image[65], command[64];
	int device;

	if (argc == 2 && !strcmp(argv[1], "--abi")) {
		printf("{\"version\":1,\"prepare_bytes\":%zu,\"read_bytes\":%zu,"
		       "\"release_bytes\":%zu,\"status_bytes\":%zu,\"record_bytes\":%zu,"
		       "\"identity_bytes\":%zu,\"header_bytes\":%zu,\"digest_offset\":%zu,"
		       "\"prepare_ioctl\":%lu,\"read_ioctl\":%lu,\"release_ioctl\":%lu,\"status_ioctl\":%lu}\n",
		       sizeof(prepare), sizeof(struct pcreg_read), sizeof(struct pcreg_release),
		       sizeof(struct pcreg_status), sizeof(records[0]),
		       sizeof(struct pcreg_file_identity), sizeof(struct pcreg_digest_header),
		       offsetof(struct pcreg_status, records_sha256), (unsigned long)PCREG_PREPARE,
		       (unsigned long)PCREG_READ, (unsigned long)PCREG_RELEASE, (unsigned long)PCREG_STATUS);
		return 0;
	}
	if (argc < 6 || strcmp(argv[1], "hold") || argc - 5 > (int)PCREG_MAX_FILES) {
		fprintf(stderr, "usage: %s --abi | hold IMAGE_SHA256 EPOCH TOKEN FILE...\n"
			"After export, stdin accepts status, commit, abort; EOF aborts.\n", argv[0]);
		return 2;
	}
	image_id(argv[2], prepare.image_id);
	prepare.epoch = number(argv[3], 1);
	prepare.plan_generation = number(argv[4], 0);
	prepare.nr_files = argc - 5;
	for (unsigned int i = 0; i < prepare.nr_files; i++) {
		struct stat st;
		int fd = open(argv[i+5], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (fd < 0 || fstat(fd, &st))
			fail("open input");
		if (!S_ISREG(st.st_mode) || st.st_size <= 0) {
			fprintf(stderr, "input must be a nonempty regular file\n");
			return 2;
		}
		prepare.files[i].fd = fd;
		prepare.files[i].page_count = ((uint64_t)st.st_size + 4095) / 4096;
	}
	device = open("/dev/cxl_pagecache_reg", O_RDWR | O_CLOEXEC);
	if (device < 0)
		fail("open cxl_pagecache_reg");
	if (ioctl(device, PCREG_PREPARE, &prepare))
		fail("prepare");
	hex(digest, prepare.records_sha256, 32);
	hex(image, prepare.image_id, 32);
	printf("{\"kind\":\"pcreg_registration\",\"version\":1,\"session_cookie\":%llu,"
	       "\"epoch\":%llu,\"plan_generation\":%llu,\"status_phys\":%llu,"
	       "\"image_id\":\"%s\",\"records_sha256\":\"%s\",\"nr_files\":%u,"
	       "\"selected_pages\":%llu,\"requested_pages\":%llu,\"absent\":%llu,"
	       "\"busy\":%llu,\"dirty\":%llu,\"writeback\":%llu,\"not_uptodate\":%llu,"
	       "\"large\":%llu,\"mapping_changed\":%llu,\"hole_or_unwritten\":%llu,"
	       "\"unsupported_block\":%llu,\"beyond_full_pages\":%llu}\n",
	       prepare.session_cookie, prepare.epoch, prepare.plan_generation, prepare.status_phys,
	       image, digest, prepare.nr_files, prepare.stats.selected, prepare.stats.requested,
	       prepare.stats.absent, prepare.stats.busy, prepare.stats.dirty, prepare.stats.writeback,
	       prepare.stats.not_uptodate, prepare.stats.large, prepare.stats.mapping_changed,
	       prepare.stats.hole_or_unwritten, prepare.stats.unsupported_block, prepare.stats.beyond_full_pages);
	for (unsigned int i = 0; i < prepare.nr_files; i++) {
		struct pcreg_file_identity *id = &prepare.files[i].identity;
		char uuid[33];
		hex(uuid, id->fs_uuid, 16);
		printf("{\"kind\":\"pcreg_file\",\"file_slot\":%u,\"inode\":%llu,"
		       "\"inode_generation\":%u,\"file_bytes\":%llu,\"start_page\":%llu,"
		       "\"page_count\":%llu,\"fs_uuid\":\"%s\",\"dev_major\":%u,\"dev_minor\":%u}\n",
		       id->file_slot, id->inode, id->inode_generation, id->file_bytes, id->start_page,
		       id->page_count, uuid, id->dev_major, id->dev_minor);
	}
	for (uint64_t offset = 0; offset < prepare.stats.selected;) {
		struct pcreg_read read = {.version = PCREG_ABI,
			.session_cookie = prepare.session_cookie, .epoch = prepare.epoch,
			.plan_generation = prepare.plan_generation, .offset = offset,
			.records_ptr = (uintptr_t)records, .capacity = 1024};
		if (ioctl(device, PCREG_READ, &read))
			fail("read records");
		if (!read.returned || read.returned > 1024 || read.total != prepare.stats.selected)
			abort();
		for (unsigned int i = 0; i < read.returned; i++)
			printf("{\"kind\":\"pcreg_record\",\"file_slot\":%u,\"flags\":%u,"
			       "\"file_page\":%llu,\"phys\":%llu,\"disk_offset\":%llu}\n",
			       records[i].file_slot, records[i].flags, records[i].file_page,
			       records[i].phys, records[i].disk_offset);
		offset += read.returned;
	}
	print_status(device);
	while (fgets(command, sizeof(command), stdin)) {
		command[strcspn(command, "\r\n")] = 0;
		if (!strcmp(command, "status")) {
			print_status(device);
			continue;
		}
		if (!strcmp(command, "commit") || !strcmp(command, "abort")) {
			struct pcreg_release release = {.version = PCREG_ABI,
				.disposition = !strcmp(command, "commit") ? PCREG_COMMIT : PCREG_ABORT,
				.session_cookie = prepare.session_cookie, .epoch = prepare.epoch,
				.plan_generation = prepare.plan_generation};
			if (ioctl(device, PCREG_RELEASE, &release))
				fail("release");
			printf("{\"kind\":\"pcreg_release\",\"disposition\":%u,\"released\":%llu}\n",
			       release.disposition, release.released);
			print_status(device);
			close(device);
			return 0;
		}
		fprintf(stderr, "unknown command; aborting session\n");
		close(device);
		return 2;
	}
	close(device);
	return 0;
}
