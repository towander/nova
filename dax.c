/*
 * BRIEF DESCRIPTION
 *
 * DAX file operations.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/module.h>
#include <linux/buffer_head.h>
#include <asm/cpufeature.h>
#include <asm/pgtable.h>
#include <linux/version.h>
#include "nova.h"

int inplace_data_updates = 0;

module_param(inplace_data_updates, int, S_IRUGO);
MODULE_PARM_DESC(inplace_data_updates, "In-place Write Data Updates");

static ssize_t
do_dax_mapping_read(struct file *filp, char __user *buf,
	size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
	pgoff_t index, end_index;
	unsigned long offset;
	loff_t isize, pos;
	size_t copied = 0, error = 0;
	timing_t memcpy_time;

	pos = *ppos;
	index = pos >> PAGE_SHIFT;
	offset = pos & ~PAGE_MASK;

	if (!access_ok(VERIFY_WRITE, buf, len)) {
		error = -EFAULT;
		goto out;
	}

	isize = i_size_read(inode);
	if (!isize)
		goto out;

	nova_dbgv("%s: inode %lu, offset %lld, count %lu, size %lld\n",
		__func__, inode->i_ino,	pos, len, isize);

	if (len > isize - pos)
		len = isize - pos;

	if (len <= 0)
		goto out;

	end_index = (isize - 1) >> PAGE_SHIFT;
	do {
		unsigned long nr, left;
		unsigned long nvmm;
		void *dax_mem = NULL;
		int zero = 0;

		/* nr is the maximum number of bytes to copy from this page */
		if (index >= end_index) {
			if (index > end_index)
				goto out;
			nr = ((isize - 1) & ~PAGE_MASK) + 1;
			if (nr <= offset) {
				goto out;
			}
		}

		entry = nova_get_write_entry(sb, sih, index);
		if (unlikely(entry == NULL)) {
			nova_dbgv("Required extent not found: pgoff %lu, "
				"inode size %lld\n", index, isize);
			nr = PAGE_SIZE;
			zero = 1;
			goto memcpy;
		}

		/* Find contiguous blocks */
		if (index < entry->pgoff ||
			index - entry->pgoff >= entry->num_pages) {
			nova_err(sb, "%s ERROR: %lu, entry pgoff %llu, num %u, "
				"blocknr %llu\n", __func__, index, entry->pgoff,
				entry->num_pages, entry->block >> PAGE_SHIFT);
			return -EINVAL;
		}
		if (entry->reassigned == 0) {
			nr = (entry->num_pages - (index - entry->pgoff))
				* PAGE_SIZE;
		} else {
			nr = PAGE_SIZE;
		}

		nvmm = get_nvmm(sb, sih, entry, index);
		dax_mem = nova_get_block(sb, (nvmm << PAGE_SHIFT));

memcpy:
		nr = nr - offset;
		if (nr > len - copied)
			nr = len - copied;

		if ( (!zero) && (data_csum > 0) ) {
			if (nova_find_pgoff_in_vma(inode, index))
				goto skip_verify;

			if (!nova_verify_data_csum(sb, sih, nvmm, offset, nr)) {
				nova_err(sb, "%s: nova data checksum and "
					"recovery fail! "
					"inode %lu, offset %lu, "
					"entry pgoff %lu, %u pages, "
					"pgoff %lu\n", __func__,
					inode->i_ino, offset, entry->pgoff,
					entry->num_pages, index);
				error = -EIO;
				goto out;
			}
		}
skip_verify:
		NOVA_START_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (!zero)
			left = __copy_to_user(buf + copied,
						dax_mem + offset, nr);
		else
			left = __clear_user(buf + copied, nr);

		NOVA_END_TIMING(memcpy_r_nvmm_t, memcpy_time);

		if (left) {
			nova_dbg("%s ERROR!: bytes %lu, left %lu\n",
				__func__, nr, left);
			error = -EFAULT;
			goto out;
		}

		copied += (nr - left);
		offset += (nr - left);
		index += offset >> PAGE_SHIFT;
		offset &= ~PAGE_MASK;
	} while (copied < len);

out:
	*ppos = pos + copied;
	if (filp)
		file_accessed(filp);

	NOVA_STATS_ADD(read_bytes, copied);

	nova_dbgv("%s returned %zu\n", __func__, copied);
	return (copied ? copied : error);
}

/*
 * Wrappers. We need to use the rcu read lock to avoid
 * concurrent truncate operation. No problem for write because we held
 * lock.
 */
ssize_t nova_dax_file_read(struct file *filp, char __user *buf,
			    size_t len, loff_t *ppos)
{
	struct inode *inode = filp->f_mapping->host;
	ssize_t res;
	timing_t dax_read_time;

	NOVA_START_TIMING(dax_read_t, dax_read_time);
	inode_lock_shared(inode);
	res = do_dax_mapping_read(filp, buf, len, ppos);
	inode_unlock_shared(inode);
	NOVA_END_TIMING(dax_read_t, dax_read_time);
	return res;
}

static inline int nova_copy_partial_block(struct super_block *sb,
	struct nova_inode_info_header *sih,
	struct nova_file_write_entry *entry, unsigned long index,
	size_t offset, size_t length, void* kmem)
{
	void *ptr;
	int rc = 0;
	unsigned long nvmm;

	nvmm = get_nvmm(sb, sih, entry, index);
	ptr = nova_get_block(sb, (nvmm << PAGE_SHIFT));

	if (ptr != NULL) {
		rc = memcpy_to_pmem_nocache(kmem + offset, ptr + offset,
						length);
	}

	/* TODO: If rc < 0, go to MCE data recovery. */
	return rc;
}

static inline int nova_handle_partial_block(struct super_block *sb,
	struct nova_inode_info_header *sih,
	struct nova_file_write_entry *entry, unsigned long index,
	size_t offset, size_t length, void* kmem)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	nova_memunlock_block(sb, kmem);
	if (entry == NULL)
		/* Fill zero */
		memcpy_to_pmem_nocache(kmem + offset, sbi->zeroed_page, length);
	else
		/* Copy from original block */
		nova_copy_partial_block(sb, sih, entry, index,
					offset, length, kmem);
	nova_memlock_block(sb, kmem);

	return 0;
}

/*
 * Fill the new start/end block from original blocks.
 * Do nothing if fully covered; copy if original blocks present;
 * Fill zero otherwise.
 */
static void nova_handle_head_tail_blocks(struct super_block *sb,
	struct inode *inode, loff_t pos, size_t count, void *kmem)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	size_t offset, eblk_offset;
	unsigned long start_blk, end_blk, num_blocks;
	struct nova_file_write_entry *entry;
	timing_t partial_time;

	NOVA_START_TIMING(partial_block_t, partial_time);
	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	/* offset in the actual block size block */
	offset = pos & (nova_inode_blk_size(sih) - 1);
	start_blk = pos >> sb->s_blocksize_bits;
	end_blk = start_blk + num_blocks - 1;

	nova_dbg_verbose("%s: %lu blocks\n", __func__, num_blocks);
	/* We avoid zeroing the alloc'd range, which is going to be overwritten
	 * by this system call anyway */
	nova_dbg_verbose("%s: start offset %lu start blk %lu %p\n", __func__,
				offset, start_blk, kmem);
	if (offset != 0) {
		entry = nova_get_write_entry(sb, sih, start_blk);
		nova_handle_partial_block(sb, sih, entry, start_blk,
					0, offset, kmem);
	}

	kmem = (void *)((char *)kmem +
			((num_blocks - 1) << sb->s_blocksize_bits));
	eblk_offset = (pos + count) & (nova_inode_blk_size(sih) - 1);
	nova_dbg_verbose("%s: end offset %lu, end blk %lu %p\n", __func__,
				eblk_offset, end_blk, kmem);
	if (eblk_offset != 0) {
		entry = nova_get_write_entry(sb, sih, end_blk);
		nova_handle_partial_block(sb, sih, entry, end_blk,
					eblk_offset,
					sb->s_blocksize - eblk_offset,
					kmem);

	}
	NOVA_END_TIMING(partial_block_t, partial_time);
}

int nova_reassign_file_tree(struct super_block *sb,
	struct nova_inode_info_header *sih, u64 begin_tail)
{
	struct nova_file_write_entry *entry_data;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	while (curr_p && curr_p != sih->log_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %lu log is NULL!\n",
				__func__, sih->ino);
			return -EINVAL;
		}

		entry_data = (struct nova_file_write_entry *)
					nova_get_block(sb, curr_p);

		if (nova_get_entry_type(entry_data) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n",
				__func__, nova_get_entry_type(entry_data));
			curr_p += entry_size;
			continue;
		}

		nova_assign_write_entry(sb, sih, entry_data, true);
		curr_p += entry_size;
	}

	return 0;
}

int nova_cleanup_incomplete_write(struct super_block *sb,
	struct nova_inode_info_header *sih, unsigned long blocknr,
	int allocated, u64 begin_tail, u64 end_tail)
{
	struct nova_file_write_entry *entry;
	u64 curr_p = begin_tail;
	size_t entry_size = sizeof(struct nova_file_write_entry);

	if (blocknr > 0 && allocated > 0)
		nova_free_data_blocks(sb, sih, blocknr, allocated);

	if (begin_tail == 0 || end_tail == 0)
		return 0;

	while (curr_p != end_tail) {
		if (is_last_entry(curr_p, entry_size))
			curr_p = next_log_page(sb, curr_p);

		if (curr_p == 0) {
			nova_err(sb, "%s: File inode %lu log is NULL!\n",
				__func__, sih->ino);
			return -EINVAL;
		}

		entry = (struct nova_file_write_entry *)
					nova_get_block(sb, curr_p);

		if (nova_get_entry_type(entry) != FILE_WRITE) {
			nova_dbg("%s: entry type is not write? %d\n",
				__func__, nova_get_entry_type(entry));
			curr_p += entry_size;
			continue;
		}

		blocknr = entry->block >> PAGE_SHIFT;
		nova_free_data_blocks(sb, sih, blocknr, entry->num_pages);
		curr_p += entry_size;
	}

	return 0;
}

void nova_init_file_write_entry(struct super_block *sb,
	struct nova_inode_info_header *sih, struct nova_file_write_entry *entry,
	u64 epoch_id, u64 pgoff, int num_pages, u64 blocknr, u32 time,
	u64 file_size)
{
	memset(entry, 0, sizeof(struct nova_file_write_entry));
	entry->entry_type = FILE_WRITE;
	entry->reassigned = 0;
	entry->updating = 0;
	entry->epoch_id = epoch_id;
	entry->pgoff = cpu_to_le64(pgoff);
	entry->num_pages = cpu_to_le32(num_pages);
	entry->invalid_pages = 0;
	entry->block = cpu_to_le64(nova_get_block_off(sb, blocknr,
							sih->i_blk_type));
	entry->mtime = cpu_to_le32(time);

	entry->size = file_size;
}

static int nova_protect_file_data(struct super_block *sb, struct inode *inode,
	loff_t pos, size_t count, const char __user *buf, unsigned long blocknr,
	bool inplace)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	size_t offset, eblk_offset, bytes, left;
	unsigned long start_blk, end_blk, num_blocks, nvmm, nvmmoff;
	unsigned long blocksize = sb->s_blocksize;
	unsigned int blocksize_bits = sb->s_blocksize_bits;
	u8 *blockbuf, *blockptr;
	struct nova_file_write_entry *entry;
	bool mapped, nvmm_ok;
	int ret = 0;
	timing_t protect_file_data_time;

	NOVA_START_TIMING(protect_file_data_t, protect_file_data_time);

	offset = pos & (blocksize - 1);
	num_blocks = ((offset + count - 1) >> blocksize_bits) + 1;
	start_blk = pos >> blocksize_bits;
	end_blk = start_blk + num_blocks - 1;

	blockbuf = (u8 *) kzalloc(blocksize, GFP_KERNEL);
	if (blockbuf == NULL) {
		nova_err(sb, "%s: block buffer allocation error\n", __func__);
		return -ENOMEM;
	}

	bytes = blocksize - offset;
	if (bytes > count) bytes = count;

	left = copy_from_user(blockbuf + offset, buf, bytes);
	if (unlikely(left != 0)) {
		nova_err(sb, "%s: not all data is copied from user! "
				"expect to copy %zu bytes, actually "
				"copied %zu bytes\n", __func__,
				bytes, bytes - left);
		ret = -EFAULT;
		goto out;
	}

	if (offset != 0) {
		entry = nova_get_write_entry(sb, sih, start_blk);
		if (entry != NULL) {
			/* make sure data in the partial block head is good */
			nvmm = get_nvmm(sb, sih, entry, start_blk);
			nvmmoff = nova_get_block_off(sb, nvmm, sih->i_blk_type);
			blockptr = (u8 *) nova_get_block(sb, nvmmoff);

			mapped = nova_find_pgoff_in_vma(inode, start_blk);
			if (data_csum > 0 && !mapped && !inplace) {
				nvmm_ok = nova_verify_data_csum(sb, sih, nvmm,
								0, offset);
				if (!nvmm_ok) {
					ret = -EIO;
					goto out;
				}
			}

			ret = memcpy_from_pmem(blockbuf, blockptr, offset);
			if (ret < 0) goto out;
		}

		/* copying existing checksums from nvmm can be even slower than
		 * re-computing checksums of a whole block.
		if (data_csum > 0)
			nova_copy_partial_block_csum(sb, sih, entry, start_blk,
							offset, blocknr, false);
		*/
	}

	if (num_blocks == 1) goto eblk;

	do {
		nova_update_block_csum_parity(sb, sih, blockbuf, blocknr,
							0, blocksize);

		blocknr++;
		pos += bytes;
		buf += bytes;
		count -= bytes;
		offset = pos & (blocksize - 1);

		bytes = count < blocksize ? count : blocksize;
		left = copy_from_user(blockbuf, buf, bytes);
		if (unlikely(left != 0)) {
			nova_err(sb, "%s: not all data is copied from user! "
					"expect to copy %zu bytes, actually "
					"copied %zu bytes\n", __func__,
					bytes, bytes - left);
			ret = -EFAULT;
			goto out;
		}
	} while (count > blocksize);

eblk:
	eblk_offset = (pos + count) & (blocksize - 1);

	if (eblk_offset != 0) {
		entry = nova_get_write_entry(sb, sih, end_blk);
		if (entry != NULL) {
			/* make sure data in the partial block tail is good */
			nvmm = get_nvmm(sb, sih, entry, end_blk);
			nvmmoff = nova_get_block_off(sb, nvmm, sih->i_blk_type);
			blockptr = (u8 *) nova_get_block(sb, nvmmoff);

			mapped = nova_find_pgoff_in_vma(inode, end_blk);
			if (data_csum > 0 && !mapped && !inplace) {
				nvmm_ok = nova_verify_data_csum(sb, sih, nvmm,
					eblk_offset, blocksize - eblk_offset);
				if (!nvmm_ok) {
					ret = -EIO;
					goto out;
				}
			}

			ret = memcpy_from_pmem(blockbuf + eblk_offset,
						blockptr + eblk_offset,
						blocksize - eblk_offset);
			if (ret < 0) goto out;
		}

		if (entry == NULL && num_blocks > 1)
			memset(blockbuf + eblk_offset, 0,
				blocksize - eblk_offset);

		/* copying existing checksums from nvmm can be even slower than
		 * re-computing checksums of a whole block.
		if (data_csum > 0)
			nova_copy_partial_block_csum(sb, sih, entry, end_blk,
						eblk_offset, blocknr, true);
		*/
	}

	nova_update_block_csum_parity(sb, sih, blockbuf, blocknr, 0, blocksize);

out:
	if (blockbuf != NULL) kfree(blockbuf);

	NOVA_END_TIMING(protect_file_data_t, protect_file_data_time);

	return ret;
}

static ssize_t nova_cow_file_write(struct file *filp,
	const char __user *buf,	size_t len, loff_t *ppos, bool need_lock)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode    *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	ssize_t     written = 0;
	loff_t pos;
	size_t count, offset, copied;
	unsigned long start_blk, num_blocks;
	unsigned long total_blocks;
	unsigned long blocknr = 0;
	unsigned int data_bits;
	int allocated = 0;
	void *kmem;
	u64 file_size;
	size_t bytes;
	long status = 0;
	timing_t cow_write_time, memcpy_time;
	unsigned long step = 0;
	ssize_t ret;
	u64 begin_tail = 0;
	u64 epoch_id;
	u32 time;

	if (len == 0)
		return 0;

	/*
	 * We disallow writing to a mmaped file,
	 * since write is copy-on-write while mmap is DAX (in-place).
	 */
	if (mapping_mapped(mapping))
		return -EACCES;

	NOVA_START_TIMING(cow_write_t, cow_write_time);

	sb_start_write(inode->i_sb);
	if (need_lock)
		inode_lock(inode);

	if (!access_ok(VERIFY_READ, buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	pos = *ppos;

	if (filp->f_flags & O_APPEND)
		pos = i_size_read(inode);

	count = len;

	pi = nova_get_inode(sb, inode);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	total_blocks = num_blocks;

	/* offset in the actual block size block */

	ret = file_remove_privs(filp);
	if (ret) {
		goto out;
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;

	nova_dbgv("%s: inode %lu, offset %lld, count %lu\n",
			__func__, inode->i_ino,	pos, count);

	epoch_id = nova_get_epoch_id(sb);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;
	while (num_blocks > 0) {
		offset = pos & (nova_inode_blk_size(sih) - 1);
		start_blk = pos >> sb->s_blocksize_bits;

		/* don't zero-out the allocated blocks */
		allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
						num_blocks, 0, ANY_CPU, 0);
		nova_dbg_verbose("%s: alloc %d blocks @ %lu\n", __func__,
						allocated, blocknr);

		if (allocated <= 0) {
			nova_dbg("%s alloc blocks failed %d\n", __func__,
								allocated);
			ret = allocated;
			goto out;
		}

		step++;
		bytes = sb->s_blocksize * allocated - offset;
		if (bytes > count)
			bytes = count;

		kmem = nova_get_block(inode->i_sb,
			nova_get_block_off(sb, blocknr,	sih->i_blk_type));

		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)) != 0)
			nova_handle_head_tail_blocks(sb, inode, pos, bytes, kmem);

		/* Now copy from user buf */
//		nova_dbg("Write: %p\n", kmem);
		NOVA_START_TIMING(memcpy_w_nvmm_t, memcpy_time);
		nova_memunlock_range(sb, kmem + offset, bytes);
		copied = bytes - memcpy_to_pmem_nocache(kmem + offset,
						buf, bytes);
		nova_memlock_range(sb, kmem + offset, bytes);
		NOVA_END_TIMING(memcpy_w_nvmm_t, memcpy_time);

		if (data_csum > 0 || data_parity > 0) {
			ret = nova_protect_file_data(sb, inode, pos, bytes,
							buf, blocknr, false);
			if (ret) goto out;
		}

		if (pos + copied > inode->i_size)
			file_size = cpu_to_le64(pos + copied);
		else
			file_size = cpu_to_le64(inode->i_size);

		nova_init_file_write_entry(sb, sih, &entry_data, epoch_id,
					start_blk, allocated, blocknr, time,
					file_size);

		ret = nova_append_file_write_entry(sb, pi, inode,
					&entry_data, &update);
		if (ret) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		nova_dbgv("Write: %p, %lu\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			buf += copied;
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0)
			break;

		if (begin_tail == 0)
			begin_tail = update.curr_entry;
	}

	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));

	nova_memunlock_inode(sb, pi);
	nova_update_inode(sb, inode, pi, &update, 1);
	nova_memlock_inode(sb, pi);

	/* Free the overlap blocks after the write is committed */
	ret = nova_reassign_file_tree(sb, sih, begin_tail);
	if (ret)
		goto out;

	inode->i_blocks = sih->i_blocks;

	ret = written;
	NOVA_STATS_ADD(cow_write_breaks, step);
	nova_dbgv("blocks: %lu, %lu\n", inode->i_blocks, sih->i_blocks);

	*ppos = pos;
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		sih->i_size = pos;
	}

out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
						begin_tail, update.tail);

	if (need_lock)
		inode_unlock(inode);
	sb_end_write(inode->i_sb);
	NOVA_END_TIMING(cow_write_t, cow_write_time);
	NOVA_STATS_ADD(cow_write_bytes, written);
	return ret;
}

/*
 * Check if there is an existing entry for target page offset.
 * Used for inplace write, direct IO, DAX-mmap and fallocate.
 */
unsigned long nova_check_existing_entry(struct super_block *sb,
	struct inode *inode, unsigned long num_blocks, unsigned long start_blk,
	struct nova_file_write_entry **ret_entry, int check_next, u64 epoch_id,
	int *inplace)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry;
	unsigned long next_pgoff;
	unsigned long ent_blks = 0;
	timing_t check_time;

	NOVA_START_TIMING(check_entry_t, check_time);

	*ret_entry = NULL;
	*inplace = 0;
	entry = nova_get_write_entry(sb, sih, start_blk);
	if (entry) {
		/* We can do inplace write. Find contiguous blocks */
		if (entry->reassigned == 0)
			ent_blks = entry->num_pages -
					(start_blk - entry->pgoff);
		else
			ent_blks = 1;

		if (ent_blks > num_blocks)
			ent_blks = num_blocks;

		*ret_entry = entry;

		if (entry->epoch_id == epoch_id)
			*inplace = 1;

	} else if (check_next) {
		/* Possible Hole */
		entry = nova_find_next_entry(sb, sih, start_blk);
		if (entry) {
			next_pgoff = entry->pgoff;
			if (next_pgoff <= start_blk) {
				nova_err(sb, "iblock %lu, entry pgoff %lu, "
						" num pages %lu\n", start_blk,
						next_pgoff, entry->num_pages);
				nova_print_inode_log(sb, inode);
				BUG();
				ent_blks = num_blocks;
				goto out;
			}
			ent_blks = next_pgoff - start_blk;
			if (ent_blks > num_blocks)
				ent_blks = num_blocks;
		} else {
			/* File grow */
			ent_blks = num_blocks;
		}
	}

	if (entry && ent_blks == 0) {
		nova_dbg("%s: %d\n", __func__, check_next);
		dump_stack();
	}

out:
	NOVA_END_TIMING(check_entry_t, check_time);
	return ent_blks;
}

ssize_t nova_inplace_file_write(struct file *filp,
	const char __user *buf,	size_t len, loff_t *ppos, bool need_mutex)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode    *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi;
	struct nova_file_write_entry *entry;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	ssize_t     written = 0;
	loff_t pos;
	size_t count, offset, copied;
	unsigned long start_blk, num_blocks, ent_blks = 0;
	unsigned long total_blocks;
	unsigned long blocknr = 0;
	unsigned int data_bits;
	int allocated = 0;
	int inplace = 0;
	bool hole_fill = false;
	bool update_log = false;
	void *kmem;
	u64 blk_off;
	size_t bytes;
	long status = 0;
	timing_t inplace_write_time, memcpy_time;
	unsigned long step = 0;
	u64 begin_tail = 0;
	u64 epoch_id;
	u64 file_size;
	u32 time;
	ssize_t ret;

	if (len == 0)
		return 0;

	NOVA_START_TIMING(inplace_write_t, inplace_write_time);

	sb_start_write(inode->i_sb);
	if (need_mutex)
		inode_lock(inode);

	if (!access_ok(VERIFY_READ, buf, len)) {
		ret = -EFAULT;
		goto out;
	}
	pos = *ppos;

	if (filp->f_flags & O_APPEND)
		pos = i_size_read(inode);

	count = len;

	pi = nova_get_inode(sb, inode);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	total_blocks = num_blocks;

	/* offset in the actual block size block */

	ret = file_remove_privs(filp);
	if (ret) {
		goto out;
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;

	epoch_id = nova_get_epoch_id(sb);

	nova_dbgv("%s: epoch_id %llu, inode %lu, offset %lld, count %lu\n",
			__func__, epoch_id, inode->i_ino, pos, count);
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;
	while (num_blocks > 0) {
		hole_fill = false;
		offset = pos & (nova_inode_blk_size(sih) - 1);
		start_blk = pos >> sb->s_blocksize_bits;

		ent_blks = nova_check_existing_entry(sb, inode, num_blocks,
						start_blk, &entry, 1, epoch_id,
						&inplace);

		if (entry && inplace) {
			/* We can do inplace write. Find contiguous blocks */
			blocknr = get_nvmm(sb, sih, entry, start_blk);
			blk_off = blocknr << PAGE_SHIFT;
			allocated = ent_blks;
			if (data_csum || data_parity)
				nova_set_write_entry_updating(sb, entry, 1);
		} else {
			/* Allocate blocks to fill hole */
			allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
							ent_blks, 0, ANY_CPU, 0);
			nova_dbg_verbose("%s: alloc %d blocks @ %lu\n", __func__,
							allocated, blocknr);

			if (allocated <= 0) {
				nova_dbg("%s alloc blocks failed!, %d\n", __func__,
								allocated);
				ret = allocated;
				goto out;
			}

			hole_fill = true;
			blk_off = nova_get_block_off(sb, blocknr, sih->i_blk_type);
		}

		step++;
		bytes = sb->s_blocksize * allocated - offset;
		if (bytes > count)
			bytes = count;

		kmem = nova_get_block(inode->i_sb, blk_off);

		if (hole_fill && (offset || ((offset + bytes) & (PAGE_SIZE - 1)) != 0))
			nova_handle_head_tail_blocks(sb, inode, pos, bytes, kmem);

		/* Now copy from user buf */
//		nova_dbg("Write: %p\n", kmem);
		NOVA_START_TIMING(memcpy_w_nvmm_t, memcpy_time);
		nova_memunlock_range(sb, kmem + offset, bytes);
		copied = bytes - memcpy_to_pmem_nocache(kmem + offset,
						buf, bytes);
		nova_memlock_range(sb, kmem + offset, bytes);
		NOVA_END_TIMING(memcpy_w_nvmm_t, memcpy_time);

		if (data_csum > 0 || data_parity > 0) {
			ret = nova_protect_file_data(sb, inode, pos, bytes,
						buf, blocknr, !hole_fill);
			if (ret) goto out;
		}

		if (pos + copied > inode->i_size)
			file_size = cpu_to_le64(pos + copied);
		else
			file_size = cpu_to_le64(inode->i_size);

		/* Handle hole fill write */
		if (hole_fill) {
			nova_init_file_write_entry(sb, sih, &entry_data,
						epoch_id, start_blk, allocated,
						blocknr, time, file_size);

			ret = nova_append_file_write_entry(sb, pi, inode,
						&entry_data, &update);
			if (ret) {
				nova_dbg("%s: append inode entry failed\n", __func__);
				ret = -ENOSPC;
				goto out;
			}
		} else {
			/* Update existing entry */
			struct nova_log_entry_info entry_info;

			entry_info.type = FILE_WRITE;
			entry_info.epoch_id = epoch_id;
			entry_info.time = time;
			entry_info.file_size = file_size;
			entry_info.inplace = 1;

			nova_inplace_update_write_entry(sb, inode, entry,
							&entry_info);
		}

		nova_dbgv("Write: %p, %lu\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			buf += copied;
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0)
			break;

		if (hole_fill) {
			update_log = true;
			if (begin_tail == 0)
				begin_tail = update.curr_entry;
		}
	}

	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));

	inode->i_blocks = sih->i_blocks;

	if (update_log) {
		nova_memunlock_inode(sb, pi);
		nova_update_inode(sb, inode, pi, &update, 1);
		nova_memlock_inode(sb, pi);

		/* Update file tree */
		ret = nova_reassign_file_tree(sb, sih, begin_tail);
		if (ret) {
			goto out;
		}
	}

	ret = written;
	NOVA_STATS_ADD(inplace_write_breaks, step);
	nova_dbgv("blocks: %lu, %lu\n", inode->i_blocks, sih->i_blocks);

	*ppos = pos;
	if (pos > inode->i_size) {
		i_size_write(inode, pos);
		sih->i_size = pos;
	}

out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
						begin_tail, update.tail);

	if (need_mutex)
		inode_unlock(inode);
	sb_end_write(inode->i_sb);
	NOVA_END_TIMING(inplace_write_t, inplace_write_time);
	NOVA_STATS_ADD(inplace_write_bytes, written);
	return ret;
}

ssize_t nova_dax_file_write(struct file *filp, const char __user *buf,
	size_t len, loff_t *ppos)
{
	if (inplace_data_updates) {
		return nova_inplace_file_write(filp, buf, len, ppos, true);
	} else {
		return nova_cow_file_write(filp, buf, len, ppos, true);
	}
}

/*
 * return > 0, # of blocks mapped or allocated.
 * return = 0, if plain lookup failed.
 * return < 0, error case.
 */
int nova_dax_get_blocks(struct inode *inode, sector_t iblock,
	unsigned long max_blocks, u32 *bno, bool *new, bool *boundary,
	int create, bool taking_lock)
{
	struct super_block *sb = inode->i_sb;
	struct nova_inode *pi;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_file_write_entry *entry = NULL;
	struct nova_file_write_entry entry_data;
	struct nova_inode_update update;
	u32 time;
	unsigned int data_bits;
	unsigned long nvmm = 0;
	unsigned long blocknr = 0;
	u64 epoch_id;
	int num_blocks = 0;
	int inplace = 0;
	int allocated = 0;
	int locked = 0;
	int check_next = 1;
	int ret = 0;
	timing_t get_block_time;


	if (max_blocks == 0)
		return 0;

	NOVA_START_TIMING(dax_get_block_t, get_block_time);

	nova_dbgv("%s: pgoff %lu, num %lu, create %d\n",
				__func__, iblock, max_blocks, create);

	epoch_id = nova_get_epoch_id(sb);

	if (taking_lock)
		check_next = 0;

again:
	num_blocks = nova_check_existing_entry(sb, inode, max_blocks,
					iblock, &entry, check_next, epoch_id,
					&inplace);

	if (entry) {
		if (create == 0 || inplace) {
			nvmm = get_nvmm(sb, sih, entry, iblock);
			nova_dbgv("%s: found pgoff %lu, block %lu\n",
					__func__, iblock, nvmm);
			goto out;
		}
	}

	if (create == 0) {
		num_blocks = 0;
		goto out1;
	}

	if (taking_lock && locked == 0) {
		inode_lock(inode);
		locked = 1;
		/* Check again incase someone has done it for us */
		check_next = 1;
		goto again;
	}

	pi = nova_get_inode(sb, inode);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;
	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

	/* Return initialized blocks to the user */
	allocated = nova_new_data_blocks(sb, sih, &blocknr, iblock,
						num_blocks, 1, ANY_CPU, 0);
	if (allocated <= 0) {
		nova_dbgv("%s alloc blocks failed %d\n", __func__,
							allocated);
		ret = allocated;
		goto out;
	}

	num_blocks = allocated;
	/* Do not extend file size */
	nova_init_file_write_entry(sb, sih, &entry_data,
					epoch_id, iblock, num_blocks,
					blocknr, time, inode->i_size);

	ret = nova_append_file_write_entry(sb, pi, inode,
				&entry_data, &update);
	if (ret) {
		nova_dbg("%s: append inode entry failed\n", __func__);
		ret = -ENOSPC;
		goto out;
	}

	nvmm = blocknr;
	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (num_blocks << (data_bits - sb->s_blocksize_bits));

	nova_memunlock_inode(sb, pi);
	nova_update_inode(sb, inode, pi, &update, 1);
	nova_memlock_inode(sb, pi);

	ret = nova_reassign_file_tree(sb, sih, update.curr_entry);
	if (ret)
		goto out;

	inode->i_blocks = sih->i_blocks;

//	set_buffer_new(bh);
out:
	if (ret < 0) {
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
						0, update.tail);
		num_blocks = ret;
		goto out1;
	}

	*bno = nvmm;
//	if (num_blocks > 1)
//		bh->b_size = sb->s_blocksize * num_blocks;

out1:
	if (taking_lock && locked)
		inode_unlock(inode);

	NOVA_END_TIMING(dax_get_block_t, get_block_time);
	return num_blocks;
}

#if 0
int nova_dax_get_block_nolock(struct inode *inode, sector_t iblock,
	struct buffer_head *bh, int create)
{
	unsigned long max_blocks = bh->b_size >> inode->i_blkbits;
	int ret;

	ret = nova_dax_get_blocks(inode, iblock, max_blocks,
						bh, create, false);
	if (ret > 0) {
		bh->b_size = ret << inode->i_blkbits;
		ret = 0;
	}
	return ret;
}

int nova_dax_get_block_lock(struct inode *inode, sector_t iblock,
	struct buffer_head *bh, int create)
{
	unsigned long max_blocks = bh->b_size >> inode->i_blkbits;
	int ret;

	ret = nova_dax_get_blocks(inode, iblock, max_blocks,
						bh, create, true);
	if (ret > 0) {
		bh->b_size = ret << inode->i_blkbits;
		ret = 0;
	}
	return ret;
}

static ssize_t nova_flush_mmap_to_nvmm(struct super_block *sb,
	struct inode *inode, struct nova_inode *pi, loff_t pos,
	size_t count, void *kmem, unsigned long blocknr)
{
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	unsigned long start_blk;
	unsigned long cache_addr;
	u64 nvmm_block;
	void *nvmm_addr;
	loff_t offset;
	size_t bytes, copied, csummed;
	ssize_t written = 0;
	int status = 0;
	ssize_t ret;

	while (count) {
		start_blk = pos >> sb->s_blocksize_bits;
		offset = pos & (sb->s_blocksize - 1);
		bytes = sb->s_blocksize - offset;
		if (bytes > count)
			bytes = count;

		cache_addr = nova_get_cache_addr(sb, si, start_blk);
		if (cache_addr == 0) {
			nova_dbg("%s: ino %lu %lu mmap page %lu not found!\n",
					__func__, inode->i_ino, sih->ino, start_blk);
			nova_dbg("mmap pages %lu\n", sih->mmap_pages);
			ret = -EINVAL;
			goto out;
		}

		nvmm_block = MMAP_ADDR(cache_addr);
		nvmm_addr = nova_get_block(sb, nvmm_block);
		copied = bytes - memcpy_to_pmem_nocache(kmem + offset,
				nvmm_addr + offset, bytes);

		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			count -= copied;
			blocknr += (offset + copied) >> sb->s_blocksize_bits;
			kmem += offset + copied;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0) {
			ret = status;
			goto out;
		}
	}
	ret = written;
out:
	return ret;
}

ssize_t nova_copy_to_nvmm(struct super_block *sb, struct inode *inode,
	struct nova_inode *pi, loff_t pos, size_t count, u64 *begin,
	u64 *end)
{
	struct nova_file_write_entry entry_data;
	unsigned long start_blk, num_blocks;
	unsigned long blocknr = 0;
	unsigned long total_blocks;
	unsigned int data_bits;
	int allocated = 0;
	u64 curr_entry;
	ssize_t written = 0;
	int ret;
	void *kmem;
	size_t bytes, copied;
	loff_t offset;
	int status = 0;
	u64 temp_tail = 0, begin_tail = 0;
	u64 epoch_id;
	u32 time;
	timing_t memcpy_time, copy_to_nvmm_time;

	NOVA_START_TIMING(copy_to_nvmm_t, copy_to_nvmm_time);
	sb_start_write(inode->i_sb);

	offset = pos & (sb->s_blocksize - 1);
	num_blocks = ((count + offset - 1) >> sb->s_blocksize_bits) + 1;
	total_blocks = num_blocks;
	inode->i_ctime = inode->i_mtime = CURRENT_TIME_SEC;
	time = CURRENT_TIME_SEC.tv_sec;

	nova_dbgv("%s: ino %lu, block %llu, offset %lu, count %lu\n",
		__func__, inode->i_ino, pos >> sb->s_blocksize_bits,
		(unsigned long)offset, count);

	epoch_id = nova_get_epoch_id(sb);
	temp_tail = *end;
	while (num_blocks > 0) {
		offset = pos & (nova_inode_blk_size(sih) - 1);
		start_blk = pos >> sb->s_blocksize_bits;
		allocated = nova_new_data_blocks(sb, sih, &blocknr, start_blk,
						num_blocks, 0, ANY_CPU, 0);
		if (allocated <= 0) {
			nova_dbg("%s alloc blocks failed %d\n", __func__,
								allocated);
			ret = allocated;
			goto out;
		}

		bytes = sb->s_blocksize * allocated - offset;
		if (bytes > count)
			bytes = count;

		kmem = nova_get_block(inode->i_sb,
			nova_get_block_off(sb, blocknr,	sih->i_blk_type));

		if (offset || ((offset + bytes) & (PAGE_SIZE - 1)))
			nova_handle_head_tail_blocks(sb, inode, pos, bytes, kmem);

		NOVA_START_TIMING(memcpy_w_wb_t, memcpy_time);
		copied = nova_flush_mmap_to_nvmm(sb, inode, pi, pos, bytes,
							kmem, blocknr);
		NOVA_END_TIMING(memcpy_w_wb_t, memcpy_time);

		memset(&entry_data, 0, sizeof(struct nova_file_write_entry));
		entry_data.entry_type = FILE_WRITE;
		entry_data.reassigned = 0;
		entry_data.epoch_id = epoch_id;
		entry_data.pgoff = cpu_to_le64(start_blk);
		entry_data.num_pages = cpu_to_le32(allocated);
		entry_data.invalid_pages = 0;
		entry_data.block = cpu_to_le64(nova_get_block_off(sb, blocknr,
							sih->i_blk_type));
		/* FIXME: should we use the page cache write time? */
		entry_data.mtime = cpu_to_le32(time);

		entry_data.size = cpu_to_le64(inode->i_size);

		curr_entry = nova_append_file_write_entry(sb, pi, inode,
						&entry_data, temp_tail);
		if (curr_entry == 0) {
			nova_dbg("%s: append inode entry failed\n", __func__);
			ret = -ENOSPC;
			goto out;
		}

		nova_dbgv("Write: %p, %ld\n", kmem, copied);
		if (copied > 0) {
			status = copied;
			written += copied;
			pos += copied;
			count -= copied;
			num_blocks -= allocated;
		}
		if (unlikely(copied != bytes)) {
			nova_dbg("%s ERROR!: %p, bytes %lu, copied %lu\n",
				__func__, kmem, bytes, copied);
			if (status >= 0)
				status = -EFAULT;
		}
		if (status < 0) {
			ret = status;
			goto out;
		}

		if (begin_tail == 0)
			begin_tail = curr_entry;
		temp_tail = curr_entry + sizeof(struct nova_file_write_entry);
	}

	nova_memunlock_inode(sb, pi);
	data_bits = blk_type_to_shift[sih->i_blk_type];
	sih->i_blocks += (total_blocks << (data_bits - sb->s_blocksize_bits));
	nova_memlock_inode(sb, pi);
	inode->i_blocks = sih->i_blocks;

	*begin = begin_tail;
	*end = temp_tail;

	ret = written;
out:
	if (ret < 0)
		nova_cleanup_incomplete_write(sb, sih, blocknr, allocated,
						begin_tail, temp_tail);

	sb_end_write(inode->i_sb);
	NOVA_END_TIMING(copy_to_nvmm_t, copy_to_nvmm_time);
	return ret;
}

static int nova_get_nvmm_pfn(struct super_block *sb, struct nova_inode *pi,
	struct nova_inode_info *si, u64 nvmm, pgoff_t pgoff,
	vm_flags_t vm_flags, void **kmem, unsigned long *pfn)
{
	struct nova_inode_info_header *sih = &si->header;
	u64 mmap_block;
	unsigned long cache_addr = 0;
	unsigned long blocknr = 0;
	void *mmap_addr;
	void *nvmm_addr;
	int ret;

	cache_addr = nova_get_cache_addr(sb, si, pgoff);

	if (cache_addr) {
		mmap_block = MMAP_ADDR(cache_addr);
		mmap_addr = nova_get_block(sb, mmap_block);
	} else {
		ret = nova_new_data_blocks(sb, sih, &blocknr, pgoff, 1, 0,
							ANY_CPU, 0);

		if (ret <= 0) {
			nova_dbg("%s alloc blocks failed %d\n",
					__func__, ret);
			return ret;
		}

		mmap_block = blocknr << PAGE_SHIFT;
		mmap_addr = nova_get_block(sb, mmap_block);

		if (vm_flags & VM_WRITE)
			mmap_block |= MMAP_WRITE_BIT;

		nova_dbgv("%s: inode %lu, pgoff %lu, mmap block 0x%llx\n",
			__func__, sih->ino, pgoff, mmap_block);

		ret = radix_tree_insert(&sih->cache_tree, pgoff,
					(void *)mmap_block);
		if (ret) {
			nova_dbg("%s: ERROR %d\n", __func__, ret);
			return ret;
		}

		sih->mmap_pages++;
		if (nvmm) {
			/* Copy from NVMM to dram */
			nvmm_addr = nova_get_block(sb, nvmm);
			memcpy(mmap_addr, nvmm_addr, PAGE_SIZE);
		} else {
			memset(mmap_addr, 0, PAGE_SIZE);
		}
	}

	*kmem = mmap_addr;
	*pfn = nova_get_pfn(sb, mmap_block);

	return 0;
}

static int nova_get_mmap_addr(struct inode *inode, struct vm_area_struct *vma,
	pgoff_t pgoff, int create, void **kmem, unsigned long *pfn)
{
	struct super_block *sb = inode->i_sb;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct nova_inode *pi;
	u64 nvmm;
	vm_flags_t vm_flags = vma->vm_flags;
	int ret;

	pi = nova_get_inode(sb, inode);

	nvmm = nova_find_nvmm_block(sb, sih, NULL, pgoff);

	ret = nova_get_nvmm_pfn(sb, pi, si, nvmm, pgoff, vm_flags,
						kmem, pfn);

	if (vm_flags & VM_WRITE) {
		if (pgoff < sih->low_dirty)
			sih->low_dirty = pgoff;
		if (pgoff > sih->high_dirty)
			sih->high_dirty = pgoff;
	}

	return ret;
}

/* OOM err return with dax file fault handlers doesn't mean anything.
 * It would just cause the OS to go an unnecessary killing spree !
 */
static int __nova_dax_file_fault(struct vm_area_struct *vma,
				  struct vm_fault *vmf)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	pgoff_t size;
	void *dax_mem;
	unsigned long dax_pfn = 0;
	int err;
	int ret = VM_FAULT_SIGBUS;

	inode_lock(inode);
	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size) {
		nova_dbg("[%s:%d] pgoff >= size(SIGBUS). vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx), size 0x%lx\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address, size);
		goto out;
	}

	err = nova_get_mmap_addr(inode, vma, vmf->pgoff, 1,
						&dax_mem, &dax_pfn);
	if (unlikely(err)) {
		nova_dbg("[%s:%d] get_mmap_addr failed. vm_start(0x%lx),"
			" vm_end(0x%lx), pgoff(0x%lx), VA(%lx)\n",
			__func__, __LINE__, vma->vm_start, vma->vm_end,
			vmf->pgoff, (unsigned long)vmf->virtual_address);
		goto out;
	}

	nova_dbgv("%s flags: vma 0x%lx, vmf 0x%x\n",
			__func__, vma->vm_flags, vmf->flags);

	nova_dbgv("DAX mmap: inode %lu, vm_start(0x%lx), vm_end(0x%lx), "
			"pgoff(0x%lx), vma pgoff(0x%lx), "
			"VA(0x%lx)->PA(0x%lx)\n",
			inode->i_ino, vma->vm_start, vma->vm_end, vmf->pgoff,
			vma->vm_pgoff, (unsigned long)vmf->virtual_address,
			(unsigned long)dax_pfn << PAGE_SHIFT);

	if (dax_pfn == 0)
		goto out;

	err = vm_insert_mixed(vma, (unsigned long)vmf->virtual_address,
		__pfn_to_pfn_t(dax_pfn, PFN_DEV));

	if (err == -ENOMEM)
		goto out;
	/*
	 * err == -EBUSY is fine, we've raced against another thread
	 * that faulted-in the same page
	 */
	if (err != -EBUSY)
		BUG_ON(err);

	ret = VM_FAULT_NOPAGE;

out:
	inode_unlock(inode);
	return ret;
}

static int nova_dax_file_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);
	ret = __nova_dax_file_fault(vma, vmf);
	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}
#endif

int nova_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
	unsigned flags, struct iomap *iomap, bool taking_lock)
{
	unsigned int blkbits = inode->i_blkbits;
	unsigned long first_block = offset >> blkbits;
	unsigned long max_blocks = (length + (1 << blkbits) - 1) >> blkbits;
	bool new = false, boundary = false;
	u32 bno;
	int ret;

	ret = nova_dax_get_blocks(inode, first_block, max_blocks, &bno, &new,
				&boundary, flags & IOMAP_WRITE, taking_lock);
	if (ret < 0)
		return ret;

	iomap->flags = 0;
	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = (u64)first_block << blkbits;

	if (ret == 0) {
		iomap->type = IOMAP_HOLE;
		iomap->blkno = IOMAP_NULL_BLOCK;
		iomap->length = 1 << blkbits;
	} else {
		iomap->type = IOMAP_MAPPED;
		iomap->blkno = (sector_t)bno << (blkbits - 9);
		iomap->length = (u64)ret << blkbits;
		iomap->flags |= IOMAP_F_MERGED;
	}

	if (new)
		iomap->flags |= IOMAP_F_NEW;
	return 0;
}

int nova_iomap_end(struct inode *inode, loff_t offset, loff_t length,
	ssize_t written, unsigned flags, struct iomap *iomap)
{
	if (iomap->type == IOMAP_MAPPED &&
			written < length &&
			(flags & IOMAP_WRITE))
		truncate_pagecache(inode, inode->i_size);
	return 0;
}


static int nova_iomap_begin_lock(struct inode *inode, loff_t offset,
	loff_t length, unsigned flags, struct iomap *iomap)
{
	return nova_iomap_begin(inode, offset, length, flags, iomap, true);
}

static struct iomap_ops nova_iomap_ops_lock = {
	.iomap_begin	= nova_iomap_begin_lock,
	.iomap_end	= nova_iomap_end,
};

static int nova_dax_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);

	ret = dax_iomap_fault(vma, vmf, &nova_iomap_ops_lock);

	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static int nova_dax_pmd_fault(struct vm_area_struct *vma, unsigned long addr,
	pmd_t *pmd, unsigned int flags)
{
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);

	ret = dax_iomap_pmd_fault(vma, addr, pmd, flags, &nova_iomap_ops_lock);

	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static int nova_dax_pfn_mkwrite(struct vm_area_struct *vma,
	struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vma->vm_file);
	loff_t size;
	int ret = 0;
	timing_t fault_time;

	NOVA_START_TIMING(mmap_fault_t, fault_time);

	inode_lock(inode);
	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (vmf->pgoff >= size)
		ret = VM_FAULT_SIGBUS;
	else
		ret = dax_pfn_mkwrite(vma, vmf);
	inode_unlock(inode);

	NOVA_END_TIMING(mmap_fault_t, fault_time);
	return ret;
}

static inline int nova_rbtree_compare_vma(struct vma_item *curr,
	struct vm_area_struct *vma)
{
	if (vma < curr->vma)
		return -1;
	if (vma > curr->vma)
		return 1;

	return 0;
}

static int nova_append_write_mmap_to_log(struct super_block *sb,
	struct inode *inode, struct vma_item *item)
{
	struct vm_area_struct *vma = item->vma;
	struct nova_inode *pi;
	struct nova_mmap_entry data;
	struct nova_inode_update update;
	unsigned long num_pages;
	u64 epoch_id;
	int ret;

	/* Only for csum and parity update */
	if (data_csum == 0 && data_parity == 0)
		return 0;

	pi = nova_get_inode(sb, inode);
	epoch_id = nova_get_epoch_id(sb);
	update.tail = update.alter_tail = 0;

	memset(&data, 0, sizeof(struct nova_mmap_entry));
	data.entry_type = MMAP_WRITE;
	data.epoch_id = epoch_id;
	data.pgoff = cpu_to_le64(vma->vm_pgoff);
	num_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	data.num_pages = cpu_to_le64(num_pages);
	data.invalid = 0;

	nova_dbgv("%s : Appending mmap log entry for inode %lu, "
			"pgoff %llu, %llu pages\n",
			__func__, inode->i_ino,
			data.pgoff, data.num_pages);

	ret = nova_append_mmap_entry(sb, pi, inode, &data, &update, item);
	if (ret) {
		nova_dbg("%s: append write mmap entry failure\n", __func__);
		goto out;
	}

	nova_memunlock_inode(sb, pi);
	nova_update_inode(sb, inode, pi, &update, 1);
	nova_memlock_inode(sb, pi);
out:
	return ret;
}

static int nova_insert_write_vma(struct vm_area_struct *vma)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	unsigned long flags = VM_SHARED | VM_WRITE;
	struct vma_item *item, *curr;
	struct rb_node **temp, *parent;
	int compVal;
	int insert = 0;
	int ret;
	timing_t insert_vma_time;

	if (mmap_cow == 0 && data_csum == 0 && data_parity == 0)
		return 0;

	if ((vma->vm_flags & flags) != flags)
		return 0;

	NOVA_START_TIMING(insert_vma_t, insert_vma_time);

	item = nova_alloc_vma_item(sb);
	if (!item) {
		NOVA_END_TIMING(insert_vma_t, insert_vma_time);
		return -ENOMEM;
	}

	item->vma = vma;

	nova_dbgv("Inode %lu insert vma %p, start 0x%lx, end 0x%lx, "
			"pgoff %lu \n",
			inode->i_ino, vma, vma->vm_start, vma->vm_end,
			vma->vm_pgoff);

	inode_lock(inode);

	/* Append to log */
	ret = nova_append_write_mmap_to_log(sb, inode, item);
	if (ret)
		goto out;

	temp = &(sih->vma_tree.rb_node);
	parent = NULL;

	while (*temp) {
		curr = container_of(*temp, struct vma_item, node);
		compVal = nova_rbtree_compare_vma(curr, vma);
		parent = *temp;

		if (compVal == -1) {
			temp = &((*temp)->rb_left);
		} else if (compVal == 1) {
			temp = &((*temp)->rb_right);
		} else {
			nova_dbg("%s: vma %p already exists\n",
				__func__, vma);
			kfree(item);
			goto out;
		}
	}

	rb_link_node(&item->node, parent, temp);
	rb_insert_color(&item->node, &sih->vma_tree);

	sih->num_vmas++;
	if (sih->num_vmas == 1)
		insert = 1;

out:
	inode_unlock(inode);

	if (insert) {
		spin_lock(&sbi->vma_lock);
		list_add_tail(&sih->list, &sbi->mmap_sih_list);
		spin_unlock(&sbi->vma_lock);
	}

	NOVA_END_TIMING(insert_vma_t, insert_vma_time);
	return ret;
}

static int nova_remove_write_vma(struct vm_area_struct *vma)
{
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct nova_inode_info *si = NOVA_I(inode);
	struct nova_inode_info_header *sih = &si->header;
	struct super_block *sb = inode->i_sb;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct vma_item *curr = NULL;
	struct rb_node *temp;
	int compVal;
	int found = 0;
	int remove = 0;
	timing_t remove_vma_time;

	if (mmap_cow == 0 && data_csum == 0 && data_parity == 0)
		return 0;

	NOVA_START_TIMING(remove_vma_t, remove_vma_time);
	inode_lock(inode);

	temp = sih->vma_tree.rb_node;
	while (temp) {
		curr = container_of(temp, struct vma_item, node);
		compVal = nova_rbtree_compare_vma(curr, vma);

		if (compVal == -1) {
			temp = temp->rb_left;
		} else if (compVal == 1) {
			temp = temp->rb_right;
		} else {
			nova_reset_vma_csum_parity(sb, curr);
			rb_erase(&curr->node, &sih->vma_tree);
			found = 1;
			break;
		}
	}

	if (found) {
		sih->num_vmas--;
		if (sih->num_vmas == 0)
			remove = 1;
	}

	inode_unlock(inode);

	if (found) {
		nova_dbgv("Inode %lu remove vma %p, start 0x%lx, end 0x%lx, "
				"pgoff %lu\n", inode->i_ino,
				curr->vma, curr->vma->vm_start,
				curr->vma->vm_end, curr->vma->vm_pgoff);
		nova_free_vma_item(sb, curr);
	}

	if (remove) {
		spin_lock(&sbi->vma_lock);
		list_del(&sih->list);
		spin_unlock(&sbi->vma_lock);
	}

	NOVA_END_TIMING(remove_vma_t, remove_vma_time);
	return 0;
}

static int nova_restore_page_write(struct vm_area_struct *vma,
	unsigned long address)
{
	struct mm_struct *mm = vma->vm_mm;

	if (mmap_cow == 0)
		return 0;

	down_write(&mm->mmap_sem);

	nova_dbgv("Restore vma %p write, start 0x%lx, end 0x%lx, "
			" address 0x%lx\n", vma, vma->vm_start,
			vma->vm_end, address);

	/* Restore single page write */
	nova_mmap_to_new_blocks(vma, address, 1);

	up_write(&mm->mmap_sem);

	return 0;
}

static void nova_vma_open(struct vm_area_struct *vma)
{
	nova_dbgv("[%s:%d] MMAP 4KPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end,
			vma->vm_flags, pgprot_val(vma->vm_page_prot));

	if (mmap_cow || data_csum || data_parity)
		nova_insert_write_vma(vma);
}

static void nova_vma_close(struct vm_area_struct *vma)
{
	nova_dbgv("[%s:%d] MMAP 4KPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end,
			vma->vm_flags, pgprot_val(vma->vm_page_prot));

	vma->original_write = 0;
	if (mmap_cow || data_csum || data_parity)
		nova_remove_write_vma(vma);
}

static const struct vm_operations_struct nova_dax_vm_ops = {
	.fault	= nova_dax_fault,
	.pmd_fault = nova_dax_pmd_fault,
	.page_mkwrite = nova_dax_fault,
	.pfn_mkwrite = nova_dax_pfn_mkwrite,
	.open = nova_vma_open,
	.close = nova_vma_close,
	.dax_cow = nova_restore_page_write,
};

int nova_dax_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);

	vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;

	vma->vm_ops = &nova_dax_vm_ops;

	/* Check SHARED WRITE vma */
	if (mmap_cow || data_csum || data_parity)
		nova_insert_write_vma(vma);

	nova_dbg_mmap4k("[%s:%d] MMAP 4KPAGE vm_start(0x%lx),"
			" vm_end(0x%lx), vm_flags(0x%lx), "
			"vm_page_prot(0x%lx)\n", __func__,
			__LINE__, vma->vm_start, vma->vm_end,
			vma->vm_flags, pgprot_val(vma->vm_page_prot));

	return 0;
}

