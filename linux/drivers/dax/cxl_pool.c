// SPDX-License-Identifier: GPL-2.0
/* Experimental, privileged CXL-backed logical RAM. One execution owner per fd.
 * Canonical data never becomes writable. Host process writes take the
 * same PFN write fault and receive a private CXL frame before they may proceed.
 * Private allocation credits are assigned by the controller; no distributed
 * allocator metadata or cross-kernel locks are introduced here.
 */
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/bitmap.h>
#include <linux/random.h>
#include <uapi/linux/cxl_pool.h>

#define CPM_MAX_PAGES (1UL << 20)
#define CPM_MAGIC 0x43584c504d415032ULL
#define CPM_ACTIVE 1
#define CPM_SEALED 2
#define CPM_RETIRED 3
#define CPM_LINEAGE 2

struct cpm_header {
	u64 magic;
	struct cpm_handle handle;
	struct cpm_config config;
};

struct cpm {
	struct mutex lock;
	struct cpm_config cfg;
	struct cpm_entry *map;
	u64 *credits;
	struct cpm_handle local, *state;
	struct cpm_header *header;
	unsigned long *private_bitmap;
	u64 epoch, cookie_lo, cookie_hi;
	void *base;
	bool ready, mapped;
	struct vm_area_struct *owner_vma;
};

static bool owns(struct cpm *c)
{
	return c->epoch == READ_ONCE(c->state->owner_epoch) &&
	c->cookie_lo == READ_ONCE(c->state->cookie_lo) &&
	c->cookie_hi == READ_ONCE(c->state->cookie_hi);
}

static bool cpm_in_range(u64 offset, u64 start, u64 bytes)
{
	return offset >= start && offset - start < bytes;
}

static bool valid_offset(struct cpm *c, u64 offset, bool shared)
{
	if (offset & (PAGE_SIZE - 1) || offset >= c->cfg.device_bytes)
		return false;
	if (c->header && cpm_in_range(offset, c->state->metadata_offset, c->state->metadata_bytes))
		return false;
	return cpm_in_range(offset, c->cfg.canonical_start, c->cfg.canonical_bytes) == shared;
}

static int cmp_u64(const void *a, const void *b)
{
	u64 x = *(const u64 *)a, y = *(const u64 *)b;

	return (x > y) - (x < y);
}

static int validate_private(struct cpm *c, const u64 *more, u64 nmore)
{
	u64 i, n = 0, *all;
	int ret = 0;

	if (nmore > CPM_MAX_PAGES || c->cfg.count + c->state->credit_count + nmore > 3 * CPM_MAX_PAGES)
		return -E2BIG;
	all = kvmalloc_array(c->cfg.count + c->state->credit_count + nmore, sizeof(*all), GFP_KERNEL);
	if (!all)
		return -ENOMEM;
	for (i = 0; i < c->cfg.count; i++) {
		if (c->map[i].flags & ~CPM_SHARED ||
		    !valid_offset(c, c->map[i].offset, !!(c->map[i].flags & CPM_SHARED))) {
			ret = -EINVAL;
			goto out;
		}
		if (!(c->map[i].flags & CPM_SHARED))
			all[n++] = c->map[i].offset;
	}
	for (i = 0; i < c->state->credit_count + nmore; i++) {
		u64 offset = i < c->state->credit_count ? c->credits[i] : more[i - c->state->credit_count];

		if (!valid_offset(c, offset, false)) {
			ret = -EINVAL;
			goto out;
		}
		all[n++] = offset;
	}
	sort(all, n, sizeof(*all), cmp_u64, NULL);
	for (i = 1; i < n; i++)
		if (all[i] == all[i - 1]) {
			ret = -EINVAL;
			break;
		}
out:
	kvfree(all);
	return ret;
}

/* The mmap lock is held by the fault caller. The context lock serializes
 * competing host process faults. The fault path performs no user copy. */
static vm_fault_t cpm_fault(struct vm_fault *vmf)
{
	struct cpm *c = vmf->vma->vm_private_data;
	u64 index = vmf->pgoff, offset;
	pgprot_t prot = vmf->vma->vm_page_prot;
	vm_fault_t ret;
	bool write = vmf->flags & FAULT_FLAG_WRITE;

	mutex_lock(&c->lock);
	if (c->header && (!owns(c) ||
	    (write && READ_ONCE(c->state->state) != CPM_ACTIVE) ||
	    READ_ONCE(c->state->state) == CPM_RETIRED)) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}
	if (index >= c->cfg.count) {
		ret = VM_FAULT_SIGBUS;
		goto out;
	}
	c->state->faults++;
	if (write && c->map[index].flags & CPM_SHARED) {
		if (!c->state->credit_count) {
			c->state->exhausted++;
			ret = VM_FAULT_SIGBUS;
			goto out;
		}
		offset = c->credits[--c->state->credit_count];
		memcpy(c->base + offset, c->base + c->map[index].offset, PAGE_SIZE);
		/* A read-only canonical PTE cannot authorize any writer. The context
		 * lock holds competing faults until the old PTE is invalidated below. */
		c->map[index].offset = offset;
		c->map[index].flags &= ~CPM_SHARED;
		c->state->shared_pages--;
		c->state->generation++;
		c->state->cow_pages++;
	}
	if (write)
		zap_vma_ptes(vmf->vma, vmf->address & PAGE_MASK, PAGE_SIZE);
	if (c->map[index].flags & CPM_SHARED)
		prot = __pgprot(pgprot_val(prot) & ~_PAGE_RW);
	else if (vmf->vma->vm_flags & VM_WRITE)
		/* pfn_mkwrite triggers write-notify, which clears RW in vm_page_prot.
		 * Our private CXL frame is exclusively owned, so install writable. */
		prot = __pgprot(pgprot_val(prot) | _PAGE_RW);
	ret = vmf_insert_pfn_prot(vmf->vma, vmf->address & PAGE_MASK,
	    PHYS_PFN(c->cfg.phys_base + c->map[index].offset), prot);
out:
	mutex_unlock(&c->lock);
	return ret;
}

static void cpm_vma_close(struct vm_area_struct *vma)
{
	struct cpm *c = vma->vm_private_data;

	mutex_lock(&c->lock);
	if (c->owner_vma == vma) {
		c->mapped = false;
		c->owner_vma = NULL;
	}
	mutex_unlock(&c->lock);
}

static int cpm_may_split(struct vm_area_struct *vma, unsigned long address)
{
	return -EINVAL;
}

static int cpm_mremap(struct vm_area_struct *vma)
{
	return -EINVAL;
}

static const struct vm_operations_struct cpm_vm_ops = {
	.fault = cpm_fault,
	.pfn_mkwrite = cpm_fault,
	.close = cpm_vma_close,
	.may_split = cpm_may_split,
	.mremap = cpm_mremap,
};

static int cpm_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct cpm *c = file->private_data;
	u64 first = vma->vm_pgoff, count = vma_pages(vma);

	mutex_lock(&c->lock);
	/* A single whole-RAM mapping per context. Otherwise another alias could
	 * retain a stale canonical PTE after this VMA takes CoW. */
	if (!c->ready || c->mapped || !(vma->vm_flags & VM_SHARED) ||
	    first != 0 || count != c->cfg.count) {
		mutex_unlock(&c->lock);
		return -EINVAL;
	}
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	vma->vm_ops = &cpm_vm_ops;
	vma->vm_private_data = c;
	c->mapped = true;
	c->owner_vma = vma;
	mutex_unlock(&c->lock);
	return 0;
}

static long configure(struct cpm *c, const void *arg, bool user)
{
	struct cpm_config cfg;
	int ret;

	if (c->map)
		return -EBUSY;
	if (user) {
		if (copy_from_user(&cfg, (void __user *)arg, sizeof(cfg)))
			return -EFAULT;
	} else {
		memcpy(&cfg, arg, sizeof(cfg));
	}
	if (cfg.abi != CPM_ABI || !cfg.count || cfg.count > CPM_MAX_PAGES ||
	    cfg.credit_count > CPM_MAX_PAGES || !cfg.device_bytes ||
	    (cfg.phys_base | cfg.device_bytes | cfg.canonical_start | cfg.canonical_bytes) & (PAGE_SIZE - 1) ||
	    cfg.phys_base + cfg.device_bytes < cfg.phys_base ||
	    cfg.canonical_start > cfg.device_bytes || cfg.canonical_bytes > cfg.device_bytes - cfg.canonical_start)
		return -EINVAL;
	c->cfg = cfg;
	c->map = kvmalloc_array(cfg.count, sizeof(*c->map), GFP_KERNEL);
	c->credits = kvmalloc_array(CPM_MAX_PAGES, sizeof(*c->credits), GFP_KERNEL);
	if (!c->map || !c->credits) {
		ret = -ENOMEM;
		goto fail;
	}
	if (copy_from_user(c->map, u64_to_user_ptr(cfg.entries), cfg.count * sizeof(*c->map)) ||
	    copy_from_user(c->credits, u64_to_user_ptr(cfg.credits), cfg.credit_count * sizeof(*c->credits))) {
		ret = -EFAULT;
		goto fail;
	}
	c->state->credit_count = cfg.credit_count;
	ret = validate_private(c, NULL, 0);
	if (ret)
		goto fail;
	c->base = memremap(cfg.phys_base, cfg.device_bytes, MEMREMAP_WB);
	if (!c->base) {
		ret = -ENXIO;
		goto fail;
	}
	c->state->generation = 1;
	c->state->count = cfg.count;
	c->state->full_scan_pages = cfg.count;
	c->private_bitmap = bitmap_zalloc(cfg.device_bytes >> PAGE_SHIFT, GFP_KERNEL);
	if (!c->private_bitmap) {
		ret = -ENOMEM;
		goto fail;
	}
	{
		u64 i;

		for (i = 0; i < cfg.count; i++) {
			if (c->map[i].flags & CPM_SHARED)
				c->state->shared_pages++;
			else
				__set_bit(c->map[i].offset >> PAGE_SHIFT, c->private_bitmap);
		}
		for (i = 0; i < c->state->credit_count; i++)
			__set_bit(c->credits[i] >> PAGE_SHIFT, c->private_bitmap);
	}
	smp_store_release(&c->ready, true);
	return 0;
fail:
	if (c->base) {
		memunmap(c->base);
		c->base = NULL;
	}
	bitmap_free(c->private_bitmap);
	c->private_bitmap = NULL;
	kvfree(c->credits);
	c->credits = NULL;
	kvfree(c->map);
	c->map = NULL;
	c->state->credit_count = 0;
	return ret;
}

/* Owner-private control state in CXL. No process data or per-page PTE inspection is
 * needed when a fenced owner hands this exact mapper to the next owner. */
static u64 metadata_need(const struct cpm_config *cfg)
{
	return PAGE_ALIGN(PAGE_SIZE + cfg->count * sizeof(struct cpm_entry) +
	    CPM_MAX_PAGES * sizeof(u64) + BITS_TO_LONGS(cfg->device_bytes >> PAGE_SHIFT) * sizeof(long));
}

static void persistent_pointers(struct cpm *c, struct cpm_header *h)
{
	c->header = h;
	c->state = &h->handle;
	c->cfg = h->config;
	c->map = (void *)h + PAGE_SIZE;
	c->credits = (void *)(c->map + c->cfg.count);
	c->private_bitmap = (void *)(c->credits + CPM_MAX_PAGES);
	c->epoch = c->state->owner_epoch;
	c->cookie_lo = c->state->cookie_lo;
	c->cookie_hi = c->state->cookie_hi;
}

static int create_persistent(struct cpm *c, void __user *user)
{
	struct cpm_create p;
	struct cpm_header *h;
	struct cpm_entry *old_map;
	unsigned long *old_bitmap;
	u64 *old_credits, i;
	int ret;

	if (copy_from_user(&p, user, sizeof(p)))
		return -EFAULT;
	if (!p.allocation_generation || p.metadata_offset & (PAGE_SIZE - 1) ||
	    !p.metadata_bytes || p.metadata_bytes & (PAGE_SIZE - 1) ||
	    p.metadata_offset > p.config.device_bytes ||
	    p.metadata_bytes > p.config.device_bytes - p.metadata_offset ||
	    p.config.count > CPM_MAX_PAGES || metadata_need(&p.config) > p.metadata_bytes ||
	    (p.metadata_offset < p.config.canonical_start + p.config.canonical_bytes &&
	    p.config.canonical_start < p.metadata_offset + p.metadata_bytes))
		return -EINVAL;
	/* configure has the one required baseline admission/uniqueness check. */
	ret = configure(c, &p.config, false);
	if (ret)
		return ret;
	h = c->base + p.metadata_offset;
	if (READ_ONCE(h->magic) == CPM_MAGIC && READ_ONCE(h->handle.state) != CPM_RETIRED)
		return -EBUSY;
	for (i = 0; i < c->cfg.count; i++)
		if (cpm_in_range(c->map[i].offset, p.metadata_offset, p.metadata_bytes))
			return -EINVAL;
	for (i = 0; i < c->state->credit_count; i++)
		if (cpm_in_range(c->credits[i], p.metadata_offset, p.metadata_bytes))
			return -EINVAL;
	old_map = c->map;
	old_credits = c->credits;
	old_bitmap = c->private_bitmap;
	memset(h, 0, PAGE_SIZE);
	h->config = c->cfg;
	h->config.entries = h->config.credits = 0;
	h->handle = c->local;
	h->handle.abi = 2;
	h->handle.phys_base = p.config.phys_base;
	h->handle.device_bytes = p.config.device_bytes;
	h->handle.metadata_offset = p.metadata_offset;
	h->handle.metadata_bytes = p.metadata_bytes;
	get_random_bytes(&h->handle.cookie_lo, 2 * sizeof(u64));
	h->handle.owner_epoch = p.owner_epoch;
	h->handle.allocation_generation = p.allocation_generation;
	h->handle.state = CPM_ACTIVE;
	persistent_pointers(c, h);
	memcpy(c->map, old_map, c->cfg.count * sizeof(*c->map));
	memcpy(c->credits, old_credits, c->state->credit_count * sizeof(u64));
	memcpy(c->private_bitmap, old_bitmap,
	    BITS_TO_LONGS(c->cfg.device_bytes >> PAGE_SHIFT) * sizeof(long));
	for (i = 0; i < c->cfg.count; i++)
		if (c->map[i].flags & CPM_SHARED)
			c->map[i].flags |= CPM_LINEAGE;
	kvfree(old_map);
	kvfree(old_credits);
	bitmap_free(old_bitmap);
	smp_store_release(&h->magic, CPM_MAGIC);
	return 0;
}

static int import_persistent(struct cpm *c, void __user *user)
{
	struct cpm_handle in, *h;
	struct cpm_header *header;
	void *base;
	int ret = -ESTALE;

	if (c->ready || copy_from_user(&in, user, sizeof(in)))
		return -EINVAL;
	if (in.abi != 2 || in.state != CPM_SEALED || !in.allocation_generation ||
	    !in.device_bytes || (in.phys_base | in.device_bytes | in.metadata_offset | in.metadata_bytes) & (PAGE_SIZE - 1) ||
	    in.phys_base + in.device_bytes < in.phys_base ||
	    in.metadata_offset > in.device_bytes || in.metadata_bytes < PAGE_SIZE ||
	    in.metadata_bytes > in.device_bytes - in.metadata_offset || in.owner_epoch == U64_MAX)
		return -EINVAL;
	base = memremap(in.phys_base, in.device_bytes, MEMREMAP_WB);
	if (!base)
		return -ENXIO;
	header = base + in.metadata_offset;
	h = &header->handle;
	if (smp_load_acquire(&header->magic) != CPM_MAGIC ||
	    h->abi != in.abi || h->phys_base != in.phys_base || h->device_bytes != in.device_bytes ||
	    h->metadata_offset != in.metadata_offset || h->metadata_bytes != in.metadata_bytes ||
	    h->cookie_lo != in.cookie_lo || h->cookie_hi != in.cookie_hi ||
	    h->owner_epoch != in.owner_epoch || h->generation != in.generation ||
	    h->allocation_generation != in.allocation_generation || h->count != in.count ||
	    !h->count || h->count > CPM_MAX_PAGES ||
	    header->config.count != h->count || header->config.phys_base != in.phys_base ||
	    header->config.device_bytes != in.device_bytes || metadata_need(&header->config) > in.metadata_bytes)
		goto fail;
	/* Coherent lab ownership transaction. Not a noncoherent CXL lease. */
	if (cmpxchg(&h->state, CPM_SEALED, CPM_ACTIVE) != CPM_SEALED)
		goto fail;
	WRITE_ONCE(h->owner_epoch, in.owner_epoch + 1);
	c->base = base;
	persistent_pointers(c, header);
	smp_store_release(&c->ready, true);
	if (copy_to_user(user, h, sizeof(*h)))
		return -EFAULT;
	return 0;
fail:
	memunmap(base);
	return ret;
}

static long cpm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct cpm *c = file->private_data;
	void __user *user = (void __user *)arg;
	long ret = 0;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (cmd == CPM_CONFIG || cmd == CPM_CREATE || cmd == CPM_IMPORT) {
		mutex_lock(&c->lock);
		ret = cmd == CPM_CONFIG ? configure(c, user, true) :
		cmd == CPM_CREATE ? create_persistent(c, user) : import_persistent(c, user);
		mutex_unlock(&c->lock);
		return ret;
	}
	if (!smp_load_acquire(&c->ready))
		return -EINVAL;
	if (c->header && !owns(c))
		return -ESTALE;
	if (cmd == CPM_STATUS || cmd == CPM_SEAL || cmd == CPM_RETIRE) {
		struct cpm_handle h;

		if (!c->header)
			return -EOPNOTSUPP;
		mutex_lock(&c->lock);
		if (!owns(c) || c->state->state == CPM_RETIRED) {
			ret = -ESTALE;
			goto status_done;
		}
		if (cmd == CPM_SEAL && c->state->state == CPM_ACTIVE)
			smp_store_release(&c->state->state, CPM_SEALED);
		if (cmd == CPM_RETIRE) {
			if (c->mapped || c->state->state != CPM_ACTIVE) {
				ret = -EBUSY;
				goto status_done;
			}
			smp_store_release(&c->state->state, CPM_RETIRED);
		}
		h = *c->state;
status_done:
		mutex_unlock(&c->lock);
		if (ret)
			return ret;
		return copy_to_user(user, &h, sizeof(h)) ? -EFAULT : 0;
	}
	if ((cmd == CPM_ADD_CREDITS || cmd == CPM_DISCARD) && c->header &&
	    READ_ONCE(c->state->state) != CPM_ACTIVE)
		return -EPERM;
	if (cmd == CPM_SELECT) {
		struct cpm_select r;
		u8 *out;
		u64 i;

		if (copy_from_user(&r, user, sizeof(r)))
			return -EFAULT;
		if (!r.count || r.first >= c->cfg.count || r.count > c->cfg.count - r.first)
			return -EINVAL;
		out = kvzalloc(r.count, GFP_KERNEL);
		if (!out)
			return -ENOMEM;
		mutex_lock(&c->lock);
		for (i = 0; i < r.count; i++)
			out[i] = !!(c->map[r.first + i].flags & CPM_LINEAGE);
		mutex_unlock(&c->lock);
		ret = copy_to_user(u64_to_user_ptr(r.selected), out, r.count) ? -EFAULT : 0;
		kvfree(out);
		return ret;
	}
	if (cmd == CPM_AUDIT) {
		struct cpm_audit a;
		struct cpm_pte *out;
		struct vm_area_struct *vma;
		u64 i;

		if (copy_from_user(&a, user, sizeof(a)))
			return -EFAULT;
		if (!a.count || a.count > c->cfg.count || a.address & (PAGE_SIZE - 1) ||
		    a.address + a.count * PAGE_SIZE < a.address)
			return -EINVAL;
		out = kvcalloc(a.count, sizeof(*out), GFP_KERNEL);
		if (!out)
			return -ENOMEM;
		mmap_read_lock(current->mm);
		mutex_lock(&c->lock);
		vma = find_vma(current->mm, a.address);
		if (!vma || vma != c->owner_vma || vma->vm_start > a.address ||
		    vma->vm_end < a.address + a.count * PAGE_SIZE) {
			ret = -EINVAL;
		} else
			for (i = 0; i < a.count; i++) {
				c->state->full_scan_pages++;
				pte_t *pte, value;
				spinlock_t *ptl;

				if (follow_pte(current->mm, a.address + i * PAGE_SIZE, &pte, &ptl))
					continue;
				value = ptep_get(pte);
				if (pte_present(value)) {
					out[i].pfn = pte_pfn(value);
					out[i].flags = 1 | (pte_write(value) ? 2 : 0);
				}
				pte_unmap_unlock(pte, ptl);
			}
		mutex_unlock(&c->lock);
		mmap_read_unlock(current->mm);
		if (!ret && copy_to_user(u64_to_user_ptr(a.ptes), out, a.count * sizeof(*out)))
			ret = -EFAULT;
		kvfree(out);
		return ret;
	}
	if (cmd == CPM_QUERY) {
		struct cpm_query q;
		struct cpm_entry *map;
		u64 *credits;

		if (copy_from_user(&q, user, sizeof(q)))
			return -EFAULT;
		if (q.count < c->cfg.count || q.credit_count > CPM_MAX_PAGES)
			return -ENOSPC;
		map = kvmalloc_array(c->cfg.count, sizeof(*map), GFP_KERNEL);
		credits = kvmalloc_array(q.credit_count, sizeof(*credits), GFP_KERNEL);
		if (!map || !credits) {
			kvfree(map);
			kvfree(credits);
			return -ENOMEM;
		}
		mutex_lock(&c->lock);
		if (q.credit_count < c->state->credit_count) {
			ret = -ENOSPC;
		} else {
			c->state->full_scan_pages += c->cfg.count;
			q.count = c->cfg.count;
			q.credit_count = c->state->credit_count;
			q.generation = c->state->generation;
			q.cow_pages = c->state->cow_pages;
			q.discard_pages = c->state->discard_pages;
			q.faults = c->state->faults;
			q.exhausted = c->state->exhausted;
			memcpy(map, c->map, q.count * sizeof(*map));
			{
				u64 i;

				for (i = 0; i < q.count; i++)
					map[i].flags &= CPM_SHARED;
			}
			memcpy(credits, c->credits, q.credit_count * sizeof(*credits));
		}
		mutex_unlock(&c->lock);
		if (!ret && (copy_to_user(u64_to_user_ptr(q.entries), map, q.count * sizeof(*map)) ||
		    copy_to_user(u64_to_user_ptr(q.credits), credits, q.credit_count * sizeof(*credits)) ||
		    copy_to_user(user, &q, sizeof(q))))
			ret = -EFAULT;
		kvfree(map);
		kvfree(credits);
		return ret;
	}
	if (cmd == CPM_ADD_CREDITS) {
		struct cpm_credits add;
		u64 *offsets;

		if (copy_from_user(&add, user, sizeof(add)))
			return -EFAULT;
		if (!add.count || add.count > CPM_MAX_PAGES)
			return -EINVAL;
		offsets = memdup_user(u64_to_user_ptr(add.offsets), add.count * sizeof(*offsets));
		if (IS_ERR(offsets))
			return PTR_ERR(offsets);
		mutex_lock(&c->lock);
		if (c->state->credit_count + add.count > CPM_MAX_PAGES)
			ret = -ENOSPC;
		else {
			u64 i;
			/* Previously admitted frames stay in the membership bitmap even
			 * when a credit becomes mapped. Validate only the new grant. */
			for (i = 0; i < add.count; i++) {
				if (!valid_offset(c, offsets[i], false) ||
				    test_and_set_bit(offsets[i] >> PAGE_SHIFT, c->private_bitmap)) {
					ret = -EINVAL;
					break;
				}
			}
			if (ret)
				while (i)
					__clear_bit(offsets[--i] >> PAGE_SHIFT, c->private_bitmap);
		}
		if (!ret) {
			memcpy(c->credits + c->state->credit_count, offsets, add.count * sizeof(*offsets));
			c->state->credit_count += add.count;
			c->state->generation++;
		}
		mutex_unlock(&c->lock);
		kfree(offsets);
		return ret;
	}
	if (cmd == CPM_DISCARD) {
		struct cpm_range r;
		struct vm_area_struct *vma;
		u64 i, needed = 0;

		if (copy_from_user(&r, user, sizeof(r)))
			return -EFAULT;
		if (!r.count || r.first >= c->cfg.count || r.count > c->cfg.count - r.first ||
		    r.address & (PAGE_SIZE - 1) || r.address + r.count * PAGE_SIZE < r.address)
			return -EINVAL;
		mmap_write_lock(current->mm);
		vma = find_vma(current->mm, r.address);
		if (!vma || vma->vm_start > r.address || vma->vm_end < r.address + r.count * PAGE_SIZE ||
		    vma->vm_private_data != c || vma->vm_ops != &cpm_vm_ops ||
		    vma->vm_pgoff + ((r.address - vma->vm_start) >> PAGE_SHIFT) != r.first) {
			ret = -EINVAL;
			goto unlock_mm;
		}
		mutex_lock(&c->lock);
		for (i = r.first; i < r.first + r.count; i++)
			needed += !!(c->map[i].flags & CPM_SHARED);
		if (needed > c->state->credit_count) {
			ret = -ENOSPC;
			goto unlock_ctx;
		}
		/* The host caller has quarantined the pages and excluded pending
		 * I/O before this call. Invalidate before reuse. */
		zap_vma_ptes(vma, r.address, r.count * PAGE_SIZE);
		for (i = r.first; i < r.first + r.count; i++) {
			if (c->map[i].flags & CPM_SHARED) {
				c->map[i].offset = c->credits[--c->state->credit_count];
				c->state->shared_pages--;
			}
			memset(c->base + c->map[i].offset, 0, PAGE_SIZE);
			c->map[i].flags = 0;
		}
		c->state->generation++;
		c->state->discard_pages += r.count;
unlock_ctx:
		mutex_unlock(&c->lock);
unlock_mm:
		mmap_write_unlock(current->mm);
		return ret;
	}
	return -ENOTTY;
}

static int cpm_open(struct inode *inode, struct file *file)
{
	struct cpm *c;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	mutex_init(&c->lock);
	c->state = &c->local;
	file->private_data = c;
	return 0;
}

static int cpm_release(struct inode *inode, struct file *file)
{
	struct cpm *c = file->private_data;

	if (c->header) {
		if (owns(c))
			cmpxchg(&c->state->state, CPM_ACTIVE, CPM_RETIRED);
	} else {
		kvfree(c->map);
		kvfree(c->credits);
		bitmap_free(c->private_bitmap);
	}
	if (c->base)
		memunmap(c->base);
	kfree(c);
	return 0;
}

static const struct file_operations cpm_fops = {
	.owner = THIS_MODULE, .open = cpm_open, .release = cpm_release,
	.unlocked_ioctl = cpm_ioctl, .mmap = cpm_mmap, .llseek = no_llseek,
};

static struct miscdevice cpm_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "cxl_pool", .fops = &cpm_fops, .mode = 0600,
};

static int __init cpm_init(void)
{
	return misc_register(&cpm_device);
}

static void __exit cpm_exit(void)
{
	misc_deregister(&cpm_device);
}
module_init(cpm_init);
module_exit(cpm_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental CXL data-pool logical RAM with CXL private CoW");
