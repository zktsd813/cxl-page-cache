/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CXL_PAGECACHE_REG_H
#define _UAPI_LINUX_CXL_PAGECACHE_REG_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Experimental, x86-64 little-endian ABI. All reserved fields must be zero. */
#define PCREG_ABI 1U
#define PCREG_MAX_FILES 16U
#define PCREG_MAX_PAGES 65536U
#define PCREG_RECORD_FLAGS 0x3fU
#define PCREG_STATUS_MAGIC 0x47504f4f4c535431ULL
#define PCREG_DIGEST_DOMAIN "GPOOL_RECORDS_V1"
#define PCREG_DIGEST_DOMAIN_BYTES 16U

enum pcreg_state {
	PCREG_IDLE = 0,
	PCREG_PREPARING = 1,
	PCREG_ACTIVE = 2,
	PCREG_INACTIVE = 3,
};

enum pcreg_disposition {
	PCREG_COMMIT = 1,
	PCREG_ABORT = 2,
};

/* This exact 64-byte representation participates in the registration digest. */
struct pcreg_file_identity {
	__u64 inode;
	__u64 file_bytes;
	__u64 start_page;
	__u64 page_count;
	__u32 inode_generation;
	__u32 file_slot;
	__u8 fs_uuid[16];
	__u32 dev_major;
	__u32 dev_minor;
};

struct pcreg_file {
	__s32 fd;
	__u32 reserved;
	__u64 start_page;
	__u64 page_count;
	struct pcreg_file_identity identity; /* output */
};

/* Exact 32-byte representation; ordered by file_slot then file_page. */
struct pcreg_record {
	__u32 file_slot;
	__u32 flags; /* locked, referenced, uptodate, clean, !writeback, order-0 */
	__u64 file_page;
	__u64 phys;
	__u64 disk_offset; /* bmap sanity check; controller frozen extent map is authority */
};

struct pcreg_stats {
	__u64 requested;
	__u64 selected;
	__u64 absent;
	__u64 busy;
	__u64 dirty;
	__u64 writeback;
	__u64 not_uptodate;
	__u64 large;
	__u64 mapping_changed;
	__u64 hole_or_unwritten;
	__u64 unsupported_block;
	__u64 beyond_full_pages;
};

/* Digest = SHA256(domain[16] || header[72] || identity[64]*N || record[32]*M).
 * Every integer is little-endian. No implicit padding is hashed. The helper
 * supports x86-64 only and compile-time checks every hashed structure's size.
 */
struct pcreg_digest_header {
	__u32 version;
	__u32 nr_files;
	__u64 record_count;
	__u64 session_cookie;
	__u64 epoch;
	__u64 plan_generation;
	__u8 image_id[32];
};

/* Permanent kernel-owned page, retained for the registering host kernel lifetime.
 * The controller MUST coordinate registration lifetime with that kernel and
 * validate this page and the digest before mapping records.
 * ACTIVE is published last; INACTIVE is published before any folio unlock.
 * This exposes process exit/FD-close invalidation to the controller.
 */
struct pcreg_status {
	__u64 magic;                 /* offset 0 */
	__u32 state;                 /* offset 8 */
	__u32 version;               /* offset 12 */
	__u64 session_cookie;        /* offset 16 */
	__u64 epoch;              /* offset 24 */
	__u64 plan_generation;       /* offset 32 */
	__u64 record_count;          /* offset 40 */
	__u32 nr_files;              /* offset 48 */
	__u32 record_bytes;          /* offset 52 */
	__u8 image_id[32];           /* offset 56 */
	__u8 records_sha256[32];     /* offset 88 */
	__u64 prepare_mono_ns;       /* offset 120 */
	__u64 release_mono_ns;       /* offset 128 */
	__u64 transition_sequence;   /* offset 136 */
	__u64 reserved[8];           /* offset 144, total 208 */
};

struct pcreg_prepare {
	__u32 version;
	__u32 flags;
	__u64 epoch;
	__u64 plan_generation;
	__u8 image_id[32];
	__u32 nr_files;
	__u32 max_records;
	struct pcreg_file files[PCREG_MAX_FILES];
	__u64 session_cookie; /* output */
	__u64 status_phys;     /* output */
	struct pcreg_stats stats;
	__u8 records_sha256[32];
};

struct pcreg_read {
	__u32 version;
	__u32 reserved;
	__u64 session_cookie;
	__u64 epoch;
	__u64 plan_generation;
	__u64 offset;
	__u64 records_ptr;
	__u32 capacity;
	__u32 returned; /* output */
	__u64 total;    /* output */
};

struct pcreg_release {
	__u32 version;
	__u32 disposition;
	__u64 session_cookie;
	__u64 epoch;
	__u64 plan_generation;
	__u64 released; /* output */
};

#define PCREG_PREPARE _IOWR('G', 0x70, struct pcreg_prepare)
#define PCREG_READ _IOWR('G', 0x71, struct pcreg_read)
#define PCREG_RELEASE _IOWR('G', 0x72, struct pcreg_release)
#define PCREG_STATUS _IOR('G', 0x73, struct pcreg_status)

#endif
