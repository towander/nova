/*
 * BRIEF DESCRIPTION
 *
 * Proc fs operations
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 *
 * This program is free software; you can redistribute it and/or modify it
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "nova.h"

const char *proc_dirname = "fs/NOVA";
struct proc_dir_entry *nova_proc_root;

/* ====================== Statistics ======================== */
static int nova_seq_timing_show(struct seq_file *seq, void *v)
{
	int i;

	nova_get_timing_stats();

	seq_printf(seq, "=========== NOVA kernel timing stats ===========\n");
	for (i = 0; i < TIMING_NUM; i++) {
		/* Title */
		if (Timingstring[i][0] == '=') {
			seq_printf(seq, "\n%s\n\n", Timingstring[i]);
			continue;
		}

		if (measure_timing || Timingstats[i]) {
			seq_printf(seq, "%s: count %llu, timing %llu, "
				"average %llu\n",
				Timingstring[i],
				Countstats[i],
				Timingstats[i],
				Countstats[i] ?
				Timingstats[i] / Countstats[i] : 0);
		} else {
			seq_printf(seq, "%s: count %llu\n",
				Timingstring[i],
				Countstats[i]);
		}
	}

	seq_printf(seq, "\n");
	return 0;
}

static int nova_seq_timing_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_timing_show, PDE_DATA(inode));
}

ssize_t nova_seq_clear_stats(struct file *filp, const char __user *buf,
	size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct super_block *sb = PDE_DATA(inode);

	nova_clear_stats(sb);
	return len;
}

static const struct file_operations nova_seq_timing_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_timing_open,
	.read		= seq_read,
	.write		= nova_seq_clear_stats,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int nova_seq_IO_show(struct seq_file *seq, void *v)
{
	struct super_block *sb = seq->private;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	unsigned long alloc_log_count = 0;
	unsigned long alloc_log_pages = 0;
	unsigned long alloc_data_count = 0;
	unsigned long alloc_data_pages = 0;
	unsigned long free_log_count = 0;
	unsigned long freed_log_pages = 0;
	unsigned long free_data_count = 0;
	unsigned long freed_data_pages = 0;
	int i;

	nova_get_timing_stats();
	nova_get_IO_stats();

	seq_printf(seq, "============ NOVA allocation stats ============\n\n");

	for (i = 0; i < sbi->cpus; i++) {
		free_list = nova_get_free_list(sb, i);

		alloc_log_count += free_list->alloc_log_count;
		alloc_log_pages += free_list->alloc_log_pages;
		alloc_data_count += free_list->alloc_data_count;
		alloc_data_pages += free_list->alloc_data_pages;
		free_log_count += free_list->free_log_count;
		freed_log_pages += free_list->freed_log_pages;
		free_data_count += free_list->free_data_count;
		freed_data_pages += free_list->freed_data_pages;
	}

	seq_printf(seq, "alloc log count %lu, allocated log pages %lu\n"
		"alloc data count %lu, allocated data pages %lu\n"
		"free log count %lu, freed log pages %lu\n"
		"free data count %lu, freed data pages %lu\n",
		alloc_log_count, alloc_log_pages,
		alloc_data_count, alloc_data_pages,
		free_log_count, freed_log_pages,
		free_data_count, freed_data_pages);

	seq_printf(seq, "Fast GC %llu, check pages %llu, free pages %llu, average %llu\n",
		Countstats[fast_gc_t], IOstats[fast_checked_pages],
		IOstats[fast_gc_pages], Countstats[fast_gc_t] ?
			IOstats[fast_gc_pages] / Countstats[fast_gc_t] : 0);
	seq_printf(seq, "Thorough GC %llu, checked pages %llu, free pages %llu, "
		"average %llu\n", Countstats[thorough_gc_t],
		IOstats[thorough_checked_pages], IOstats[thorough_gc_pages],
		Countstats[thorough_gc_t] ?
			IOstats[thorough_gc_pages] / Countstats[thorough_gc_t] : 0);

	seq_printf(seq, "\n");

	seq_printf(seq, "================ NOVA I/O stats ================\n\n");
	seq_printf(seq, "Read %llu, bytes %llu, average %llu\n",
		Countstats[dax_read_t], IOstats[read_bytes],
		Countstats[dax_read_t] ?
			IOstats[read_bytes] / Countstats[dax_read_t] : 0);
	seq_printf(seq, "COW write %llu, bytes %llu, average %llu, "
		"write breaks %llu, average %llu\n",
		Countstats[cow_write_t], IOstats[cow_write_bytes],
		Countstats[cow_write_t] ?
			IOstats[cow_write_bytes] / Countstats[cow_write_t] : 0,
		IOstats[cow_write_breaks], Countstats[cow_write_t] ?
			IOstats[cow_write_breaks] / Countstats[cow_write_t] : 0);
	seq_printf(seq, "Inplace write %llu, bytes %llu, average %llu, "
		"write breaks %llu, average %llu\n",
		Countstats[inplace_write_t], IOstats[inplace_write_bytes],
		Countstats[inplace_write_t] ?
			IOstats[inplace_write_bytes] / Countstats[inplace_write_t] : 0,
		IOstats[inplace_write_breaks], Countstats[inplace_write_t] ?
			IOstats[inplace_write_breaks] / Countstats[inplace_write_t] : 0);
	seq_printf(seq, "Dirty pages %llu\n", IOstats[dirty_pages]);
	seq_printf(seq, "Protect head %llu, tail %llu\n",
			IOstats[protect_head], IOstats[protect_tail]);
	seq_printf(seq, "Block csum parity %llu\n", IOstats[block_csum_parity]);
	seq_printf(seq, "Page fault %llu, dax cow fault %llu, "
			"dax cow fault during snapshot creation %llu, "
			"mapping/pfn updated pages %llu\n",
			Countstats[mmap_fault_t], Countstats[mmap_cow_t],
			IOstats[dax_cow_during_snapshot],
			IOstats[mapping_updated_pages]);

	seq_printf(seq, "\n");

	nova_print_snapshot_lists(sb, seq);
	seq_printf(seq, "\n");

	return 0;
}

static int nova_seq_IO_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_IO_show, PDE_DATA(inode));
}

static const struct file_operations nova_seq_IO_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_IO_open,
	.read		= seq_read,
	.write		= nova_seq_clear_stats,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int nova_seq_show_allocator(struct seq_file *seq, void *v)
{
	struct super_block *sb = seq->private;
	struct nova_sb_info *sbi = NOVA_SB(sb);
	struct free_list *free_list;
	int i;
	unsigned long log_pages = 0;
	unsigned long data_pages = 0;

	seq_printf(seq, "======== NOVA per-CPU allocator stats ========\n");
	for (i = 0; i < sbi->cpus; i++) {
		free_list = nova_get_free_list(sb, i);
		seq_printf(seq, "Free list %d: block start %lu, block end %lu, "
			"num_blocks %lu, num_free_blocks %lu, blocknode %lu\n",
			i, free_list->block_start, free_list->block_end,
			free_list->block_end - free_list->block_start + 1,
			free_list->num_free_blocks, free_list->num_blocknode);

		if (free_list->first_node) {
			seq_printf(seq, "First node %lu - %lu\n",
					free_list->first_node->range_low,
					free_list->first_node->range_high);
		}

		if (free_list->last_node) {
			seq_printf(seq, "Last node %lu - %lu\n",
					free_list->last_node->range_low,
					free_list->last_node->range_high);
		}

		seq_printf(seq, "Free list %d: csum start %lu, "
			"replica csum start %lu, csum blocks %lu, "
			"parity start %lu, parity blocks %lu\n",
			i, free_list->csum_start, free_list->replica_csum_start,
			free_list->num_csum_blocks,
			free_list->parity_start, free_list->num_parity_blocks);

		seq_printf(seq, "Free list %d: alloc log count %lu, "
			"allocated log pages %lu, alloc data count %lu, "
			"allocated data pages %lu, free log count %lu, "
			"freed log pages %lu, free data count %lu, "
			"freed data pages %lu\n", i,
			free_list->alloc_log_count,
			free_list->alloc_log_pages,
			free_list->alloc_data_count,
			free_list->alloc_data_pages,
			free_list->free_log_count,
			free_list->freed_log_pages,
			free_list->free_data_count,
			free_list->freed_data_pages);

		log_pages += free_list->alloc_log_pages;
		log_pages -= free_list->freed_log_pages;

		data_pages += free_list->alloc_data_pages;
		data_pages -= free_list->freed_data_pages;
	}

	seq_printf(seq, "\nCurrently used pmem pages: log %lu, data %lu\n",
			log_pages, data_pages);

	return 0;
}

static int nova_seq_allocator_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_show_allocator,
				PDE_DATA(inode));
}

static const struct file_operations nova_seq_allocator_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_allocator_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ====================== Snapshot ======================== */
static int nova_seq_create_snapshot_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "Write to create a snapshot\n");
	return 0;
}

static int nova_seq_create_snapshot_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_create_snapshot_show,
				PDE_DATA(inode));
}

ssize_t nova_seq_create_snapshot(struct file *filp, const char __user *buf,
	size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct super_block *sb = PDE_DATA(inode);

	nova_create_snapshot(sb);
	return len;
}

static const struct file_operations nova_seq_create_snapshot_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_create_snapshot_open,
	.read		= seq_read,
	.write		= nova_seq_create_snapshot,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int nova_seq_delete_snapshot_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "Echo index to delete a snapshot\n");
	return 0;
}

static int nova_seq_delete_snapshot_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_delete_snapshot_show,
				PDE_DATA(inode));
}

ssize_t nova_seq_delete_snapshot(struct file *filp, const char __user *buf,
	size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct super_block *sb = PDE_DATA(inode);
	u64 epoch_id;

	sscanf(buf, "%llu", &epoch_id);
	nova_delete_snapshot(sb, epoch_id);
	return len;
}

static const struct file_operations nova_seq_delete_snapshot_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_delete_snapshot_open,
	.read		= seq_read,
	.write		= nova_seq_delete_snapshot,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int nova_seq_show_snapshots(struct seq_file *seq, void *v)
{
	struct super_block *sb = seq->private;

	nova_print_snapshots(sb, seq);
	return 0;
}

static int nova_seq_show_snapshots_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_show_snapshots,
				PDE_DATA(inode));
}

static const struct file_operations nova_seq_show_snapshots_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_show_snapshots_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ====================== Performance ======================== */
static int nova_seq_test_perf_show(struct seq_file *seq, void *v)
{
	seq_printf(seq, "Echo function:poolmb:size:disks to test function "
			"performance working on size of data.\n"
			"    example: echo 1:128:4096:8 > "
			"/proc/fs/NOVA/pmem0/test_perf\n"
			"The disks value only matters for raid functions.\n"
			"Set function to 0 to test all functions.\n");
	return 0;
}

static int nova_seq_test_perf_open(struct inode *inode, struct file *file)
{
	return single_open(file, nova_seq_test_perf_show, PDE_DATA(inode));
}

ssize_t nova_seq_test_perf(struct file *filp, const char __user *buf,
	size_t len, loff_t *ppos)
{
	struct address_space *mapping = filp->f_mapping;
	struct inode *inode = mapping->host;
	struct super_block *sb = PDE_DATA(inode);
	size_t size;
	unsigned int func_id, poolmb, disks;

	sscanf(buf, "%u:%u:%zu:%u", &func_id, &poolmb, &size, &disks);
	nova_test_perf(sb, func_id, poolmb, size, disks);

	return len;
}

static const struct file_operations nova_seq_test_perf_fops = {
	.owner		= THIS_MODULE,
	.open		= nova_seq_test_perf_open,
	.read		= seq_read,
	.write		= nova_seq_test_perf,
	.llseek		= seq_lseek,
	.release	= single_release,
};

void nova_sysfs_init(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	if (nova_proc_root)
		sbi->s_proc = proc_mkdir(sbi->s_bdev->bd_disk->disk_name,
					 nova_proc_root);

	if (sbi->s_proc) {
		proc_create_data("timing_stats", S_IRUGO, sbi->s_proc,
				 &nova_seq_timing_fops, sb);
		proc_create_data("IO_stats", S_IRUGO, sbi->s_proc,
				 &nova_seq_IO_fops, sb);
		proc_create_data("allocator", S_IRUGO, sbi->s_proc,
				 &nova_seq_allocator_fops, sb);
		proc_create_data("create_snapshot", S_IRUGO, sbi->s_proc,
				 &nova_seq_create_snapshot_fops, sb);
		proc_create_data("delete_snapshot", S_IRUGO, sbi->s_proc,
				 &nova_seq_delete_snapshot_fops, sb);
		proc_create_data("snapshots", S_IRUGO, sbi->s_proc,
				 &nova_seq_show_snapshots_fops, sb);
		proc_create_data("test_perf", S_IRUGO, sbi->s_proc,
				 &nova_seq_test_perf_fops, sb);
	}
}

void nova_sysfs_exit(struct super_block *sb)
{
	struct nova_sb_info *sbi = NOVA_SB(sb);

	if (sbi->s_proc) {
		remove_proc_entry("timing_stats", sbi->s_proc);
		remove_proc_entry("IO_stats", sbi->s_proc);
		remove_proc_entry("allocator", sbi->s_proc);
		remove_proc_entry("create_snapshot", sbi->s_proc);
		remove_proc_entry("delete_snapshot", sbi->s_proc);
		remove_proc_entry("snapshots", sbi->s_proc);
		remove_proc_entry("test_perf", sbi->s_proc);
		remove_proc_entry(sbi->s_bdev->bd_disk->disk_name,
					nova_proc_root);
	}
}
