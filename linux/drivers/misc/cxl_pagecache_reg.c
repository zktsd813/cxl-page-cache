// SPDX-License-Identifier: GPL-2.0
/*
 * Host kernel registration authority for shared page pooling.
 *
 * No file data is read or faulted in. Only resident, clean, uptodate, order-0
 * folios of plain files on a read-only ext4/block device are exported. A folio
 * reference and its lock remain held until explicit release or FD close.
 * The lock excludes migration/removal; the reference excludes physical reuse.
 * A permanent status page invalidates exported records before cleanup begins.
 *
 * The controller trusts the registering host kernel. It must match image
 * identity and inode/index against frozen extents, and validate ACTIVE status
 * and the digest while coordinating registration lifetime with that kernel.
 */
#include <crypto/sha2.h>
#include <linux/blkdev.h>
#include <linux/capability.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <uapi/linux/cxl_pagecache_reg.h>

struct pcreg_session {
	struct mutex mutex;
	struct pcreg_prepare *prepared;
	struct file *files[PCREG_MAX_FILES];
	struct folio **folios;
	struct pcreg_record *records;
	unsigned int held;
	bool active;
};

static atomic_t pcreg_opened = ATOMIC_INIT(0);
static struct pcreg_status *pcreg_status_page;

static void pcreg_drop(struct pcreg_session *s)
{
	unsigned int i;

	/* Publish invalidity before permitting migration, reclaim or frame reuse. */
	smp_store_release(&pcreg_status_page->state, PCREG_INACTIVE);
	smp_mb();
	pcreg_status_page->release_mono_ns = ktime_get_ns();
	pcreg_status_page->transition_sequence++;
	for (i = 0; i < s->held; i++) {
		folio_unlock(s->folios[i]);
		folio_put(s->folios[i]);
	}
	for (i = 0; i < PCREG_MAX_FILES; i++) {
		if (s->files[i])
			fput(s->files[i]);
		s->files[i] = NULL;
	}
	kvfree(s->folios);
	kvfree(s->records);
	kfree(s->prepared);
	s->folios = NULL;
	s->records = NULL;
	s->prepared = NULL;
	s->held = 0;
	s->active = false;
}

static int pcreg_open(struct inode *inode, struct file *file)
{
	struct pcreg_session *s;

	if (!ns_capable(&init_user_ns, CAP_SYS_ADMIN))
		return -EPERM;
	if (atomic_cmpxchg(&pcreg_opened, 0, 1))
		return -EBUSY;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		atomic_set(&pcreg_opened, 0);
		return -ENOMEM;
	}
	mutex_init(&s->mutex);
	file->private_data = s;
	return nonseekable_open(inode, file);
}

static int pcreg_close(struct inode *inode, struct file *file)
{
	struct pcreg_session *s = file->private_data;

	mutex_lock(&s->mutex);
	pcreg_drop(s);
	mutex_unlock(&s->mutex);
	kfree(s);
	atomic_set(&pcreg_opened, 0);
	return 0;
}

static int pcreg_validate_file(struct pcreg_session *s, unsigned int slot)
{
	struct pcreg_file *request = &s->prepared->files[slot];
	struct pcreg_file_identity *id = &request->identity;
	struct inode *inode;
	struct super_block *sb;
	unsigned int i;

	if (request->reserved || request->fd < 0 || !request->page_count ||
	    request->start_page > ULONG_MAX - request->page_count)
		return -EINVAL;
	s->files[slot] = fget(request->fd);
	if (!s->files[slot])
		return -EBADF;
	inode = file_inode(s->files[slot]);
	sb = inode->i_sb;
	if (!(s->files[slot]->f_mode & FMODE_READ) ||
	    (s->files[slot]->f_mode & FMODE_WRITE) || !S_ISREG(inode->i_mode) ||
	    strcmp(sb->s_type->name, "ext4") || !sb_rdonly(sb) ||
	    !sb->s_bdev || !bdev_read_only(sb->s_bdev) || sb->s_blocksize != PAGE_SIZE ||
	    IS_DAX(inode) || IS_ENCRYPTED(inode) || IS_VERITY(inode))
		return -EOPNOTSUPP;
	for (i = 0; i < slot; i++)
		if (file_inode(s->files[i]) == inode)
			return -EINVAL;
	memset(id, 0, sizeof(*id));
	id->inode = inode->i_ino;
	id->file_bytes = i_size_read(inode);
	id->start_page = request->start_page;
	id->page_count = request->page_count;
	id->inode_generation = inode->i_generation;
	id->file_slot = slot;
	memcpy(id->fs_uuid, sb->s_uuid.b, sizeof(id->fs_uuid));
	id->dev_major = MAJOR(sb->s_dev);
	id->dev_minor = MINOR(sb->s_dev);
	return 0;
}

static void pcreg_scan_file(struct pcreg_session *s, unsigned int slot)
{
	struct pcreg_file *request = &s->prepared->files[slot];
	struct pcreg_stats *stats = &s->prepared->stats;
	struct inode *inode = file_inode(s->files[slot]);
	struct address_space *mapping = inode->i_mapping;
	u64 end = request->start_page + request->page_count;
	u64 full_pages = request->identity.file_bytes >> PAGE_SHIFT;
	u64 index;

	for (index = request->start_page; index < end; index++) {
		struct pcreg_record *record;
		struct folio *folio;
		sector_t block = index;

		stats->requested++;
		if (index >= full_pages) {
			stats->beyond_full_pages++;
			continue;
		}
		/* No FGP_CREAT/READ: a cache miss never instantiates a data folio. */
		folio = filemap_get_folio(mapping, index);
		if (IS_ERR(folio)) {
			stats->absent++;
			continue;
		}
		if (folio_test_large(folio)) {
			stats->large++;
			goto put;
		}
		/* bmap may fetch filesystem metadata, never the selected file data.
		 * It is done before locking, since ext4_bmap may wait on writeback.
		 * Read-only device + superblock keep the block identity immutable.
		 */
		if (bmap(inode, &block)) {
			stats->unsupported_block++;
			goto put;
		}
		if (!block) {
			stats->hole_or_unwritten++;
			goto put;
		}
		if (!folio_trylock(folio)) {
			stats->busy++;
			goto put;
		}
		if (folio_mapping(folio) != mapping || folio->index != index ||
		    folio_test_large(folio)) {
			stats->mapping_changed++;
			goto unlock;
		}
		if (!folio_test_uptodate(folio)) {
			stats->not_uptodate++;
			goto unlock;
		}
		if (folio_test_dirty(folio)) {
			stats->dirty++;
			goto unlock;
		}
		if (folio_test_writeback(folio)) {
			stats->writeback++;
			goto unlock;
		}
		record = &s->records[s->held];
		record->file_slot = slot;
		record->flags = PCREG_RECORD_FLAGS;
		record->file_page = index;
		record->phys = (u64)folio_pfn(folio) << PAGE_SHIFT;
		record->disk_offset = (u64)block << PAGE_SHIFT;
		s->folios[s->held++] = folio;
		stats->selected++;
		cond_resched();
		continue;
unlock:
		folio_unlock(folio);
put:
		folio_put(folio);
		cond_resched();
	}
}

static void pcreg_digest(struct pcreg_session *s)
{
	struct pcreg_prepare *p = s->prepared;
	struct pcreg_digest_header header = {
		.version = PCREG_ABI, .nr_files = p->nr_files,
		.record_count = s->held, .session_cookie = p->session_cookie,
		.epoch = p->epoch, .plan_generation = p->plan_generation,
	};
	struct sha256_state hash;
	unsigned int i;

	memcpy(header.image_id, p->image_id, sizeof(header.image_id));
	sha256_init(&hash);
	sha256_update(&hash, PCREG_DIGEST_DOMAIN, PCREG_DIGEST_DOMAIN_BYTES);
	sha256_update(&hash, (const u8 *)&header, sizeof(header));
	for (i = 0; i < p->nr_files; i++)
		sha256_update(&hash, (const u8 *)&p->files[i].identity, sizeof(p->files[i].identity));
	sha256_update(&hash, (const u8 *)s->records, s->held * sizeof(*s->records));
	sha256_final(&hash, p->records_sha256);
}

static long pcreg_prepare(struct pcreg_session *s, void __user *user)
{
	struct pcreg_prepare *p;
	u64 requested = 0;
	unsigned int i;
	int error;

	if (s->active)
		return -EBUSY;
	p = memdup_user(user, sizeof(*p));
	if (IS_ERR(p))
		return PTR_ERR(p);
	if (p->version != PCREG_ABI || p->flags ||
	    !p->plan_generation || !p->nr_files || p->nr_files > PCREG_MAX_FILES ||
	    !p->max_records || p->max_records > PCREG_MAX_PAGES ||
	    !memchr_inv(p->image_id, 0, sizeof(p->image_id))) {
		kfree(p);
		return -EINVAL;
	}
	for (i = 0; i < p->nr_files; i++) {
		if (p->files[i].page_count > p->max_records - requested) {
			kfree(p);
			return -E2BIG;
		}
		requested += p->files[i].page_count;
	}
	if (!requested) {
		kfree(p);
		return -EINVAL;
	}
	memset(&p->stats, 0, sizeof(p->stats));
	memset(p->records_sha256, 0, sizeof(p->records_sha256));
	memset(&p->files[p->nr_files], 0,
	       (PCREG_MAX_FILES - p->nr_files) * sizeof(p->files[0]));
	p->session_cookie = get_random_u64();
	if (!p->session_cookie)
		p->session_cookie = 1;
	p->status_phys = virt_to_phys(pcreg_status_page);
	s->prepared = p;
	s->folios = kvcalloc(requested, sizeof(*s->folios), GFP_KERNEL);
	s->records = kvcalloc(requested, sizeof(*s->records), GFP_KERNEL);
	if (!s->folios || !s->records) {
		error = -ENOMEM;
		goto fail;
	}
	smp_store_release(&pcreg_status_page->state, PCREG_PREPARING);
	for (i = 0; i < p->nr_files; i++) {
		error = pcreg_validate_file(s, i);
		if (error)
			goto fail;
	}
	for (i = 0; i < p->nr_files; i++)
		pcreg_scan_file(s, i);
	pcreg_digest(s);
	pcreg_status_page->session_cookie = p->session_cookie;
	pcreg_status_page->epoch = p->epoch;
	pcreg_status_page->plan_generation = p->plan_generation;
	pcreg_status_page->record_count = s->held;
	pcreg_status_page->nr_files = p->nr_files;
	pcreg_status_page->record_bytes = sizeof(*s->records);
	memcpy(pcreg_status_page->image_id, p->image_id, sizeof(p->image_id));
	memcpy(pcreg_status_page->records_sha256, p->records_sha256, sizeof(p->records_sha256));
	pcreg_status_page->prepare_mono_ns = ktime_get_ns();
	pcreg_status_page->release_mono_ns = 0;
	pcreg_status_page->transition_sequence++;
	s->active = true;
	smp_store_release(&pcreg_status_page->state, PCREG_ACTIVE);
	if (copy_to_user(user, p, sizeof(*p))) {
		error = -EFAULT;
		goto fail;
	}
	return 0;
fail:
	pcreg_drop(s);
	return error;
}

static bool pcreg_matches(struct pcreg_session *s, u64 cookie, u64 epoch, u64 generation)
{
	return s->active && cookie == s->prepared->session_cookie &&
		epoch == s->prepared->epoch && generation == s->prepared->plan_generation;
}

static long pcreg_read(struct pcreg_session *s, void __user *user)
{
	struct pcreg_read request;
	u64 count;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (request.version != PCREG_ABI || request.reserved ||
	    !request.capacity || request.capacity > PCREG_MAX_PAGES)
		return -EINVAL;
	if (!pcreg_matches(s, request.session_cookie, request.epoch, request.plan_generation))
		return -ESTALE;
	if (request.offset > s->held)
		return -ERANGE;
	count = min_t(u64, request.capacity, s->held - request.offset);
	if (copy_to_user(u64_to_user_ptr(request.records_ptr), s->records + request.offset,
			 count * sizeof(*s->records)))
		return -EFAULT;
	request.returned = count;
	request.total = s->held;
	return copy_to_user(user, &request, sizeof(request)) ? -EFAULT : 0;
}

static long pcreg_release(struct pcreg_session *s, void __user *user)
{
	struct pcreg_release request;

	if (copy_from_user(&request, user, sizeof(request)))
		return -EFAULT;
	if (request.version != PCREG_ABI ||
	    (request.disposition != PCREG_COMMIT && request.disposition != PCREG_ABORT))
		return -EINVAL;
	if (!pcreg_matches(s, request.session_cookie, request.epoch, request.plan_generation))
		return -ESTALE;
	request.released = s->held;
	pcreg_drop(s);
	return copy_to_user(user, &request, sizeof(request)) ? -EFAULT : 0;
}

static long pcreg_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct pcreg_session *s = file->private_data;
	void __user *user = (void __user *)arg;
	long result;

	if (mutex_lock_interruptible(&s->mutex))
		return -ERESTARTSYS;
	switch (command) {
	case PCREG_PREPARE:
		result = pcreg_prepare(s, user);
		break;
	case PCREG_READ:
		result = pcreg_read(s, user);
		break;
	case PCREG_RELEASE:
		result = pcreg_release(s, user);
		break;
	case PCREG_STATUS:
		result = copy_to_user(user, pcreg_status_page,
				      sizeof(*pcreg_status_page)) ? -EFAULT : 0;
		break;
	default:
		result = -ENOTTY;
	}
	mutex_unlock(&s->mutex);
	return result;
}

static const struct file_operations pcreg_operations = {
	.owner = THIS_MODULE,
	.open = pcreg_open,
	.release = pcreg_close,
	.unlocked_ioctl = pcreg_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice pcreg_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cxl_pagecache_reg",
	.fops = &pcreg_operations,
	.mode = 0600,
};

static int __init pcreg_init(void)
{
	int error;

	BUILD_BUG_ON(PAGE_SIZE != 4096);
	BUILD_BUG_ON(sizeof(struct pcreg_record) != 32);
	BUILD_BUG_ON(sizeof(struct pcreg_file_identity) != 64);
	BUILD_BUG_ON(sizeof(struct pcreg_digest_header) != 72);
	BUILD_BUG_ON(sizeof(struct pcreg_status) != 208);
	BUILD_BUG_ON(offsetof(struct pcreg_status, records_sha256) != 88);
	pcreg_status_page = (void *)get_zeroed_page(GFP_KERNEL);
	if (!pcreg_status_page)
		return -ENOMEM;
	pcreg_status_page->magic = PCREG_STATUS_MAGIC;
	pcreg_status_page->version = PCREG_ABI;
	error = misc_register(&pcreg_device);
	if (error) {
		free_page((unsigned long)pcreg_status_page);
		pcreg_status_page = NULL;
	}
	return error;
}
/* No module exit: the controller relies on this permanent status page. */
device_initcall(pcreg_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded read-only ext4 resident-folio registration for host pooling");
