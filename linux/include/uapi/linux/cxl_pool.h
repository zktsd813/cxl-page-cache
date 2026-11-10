/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_CXL_POOL_H
#define _UAPI_LINUX_CXL_POOL_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define CPM_ABI 1
#define CPM_SHARED 1
/* Logical page index is the array index; offsets are relative to the DAX device. */
struct cpm_entry { __u64 offset, flags; };
struct cpm_config {
    __u64 abi, phys_base, device_bytes, canonical_start, canonical_bytes;
    __u64 count, entries, credit_count, credits;
};
struct cpm_query {
    __u64 count, entries, credit_count, credits;
    __u64 generation, cow_pages, discard_pages, faults, exhausted;
};
struct cpm_range { __u64 address, first, count; };
struct cpm_credits { __u64 count, offsets; };
/* Read-only, no-prefault observation of actual native PTEs (not the map table).
 * flags: bit0 present, bit1 writable. A missing PTE has pfn=flags=0. */
struct cpm_pte { __u64 pfn, flags; };
struct cpm_audit { __u64 address, count, ptes; };
#define CPM_CONFIG _IOW('P', 0x70, struct cpm_config)
#define CPM_QUERY _IOWR('P', 0x71, struct cpm_query)
#define CPM_DISCARD _IOW('P', 0x72, struct cpm_range)
#define CPM_ADD_CREDITS _IOW('P', 0x73, struct cpm_credits)
#define CPM_AUDIT _IOWR('P', 0x74, struct cpm_audit)
/* ABI 2 keeps this owner's authoritative mapper in a reserved CXL extent.
 * Handles contain no per-page arrays. Import consumes a sealed owner epoch;
 * callers must fence the source process and drain every writer first. */
struct cpm_handle {
    __u64 abi, phys_base, device_bytes, metadata_offset, metadata_bytes;
    __u64 cookie_lo, cookie_hi, owner_epoch, generation, allocation_generation;
    __u64 count, credit_count, shared_pages, cow_pages, discard_pages;
    __u64 faults, exhausted, full_scan_pages, state;
};
struct cpm_create {
    struct cpm_config config;
    __u64 metadata_offset, metadata_bytes, owner_epoch, allocation_generation;
};
struct cpm_select { __u64 first, count, selected; };
#define CPM_CREATE _IOW('P', 0x75, struct cpm_create)
#define CPM_STATUS _IOWR('P', 0x76, struct cpm_handle)
#define CPM_SEAL _IOWR('P', 0x77, struct cpm_handle)
#define CPM_IMPORT _IOWR('P', 0x78, struct cpm_handle)
#define CPM_SELECT _IOWR('P', 0x79, struct cpm_select)
#define CPM_RETIRE _IOWR('P', 0x7a, struct cpm_handle)
#endif
