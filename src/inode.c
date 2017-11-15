/*
 * Copyright (C) 2015 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/xattr.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/list_sort.h>

#include "format.h"
#include "super.h"
#include "key.h"
#include "inode.h"
#include "dir.h"
#include "data.h"
#include "scoutfs_trace.h"
#include "xattr.h"
#include "trans.h"
#include "msg.h"
#include "kvec.h"
#include "item.h"
#include "client.h"
#include "cmp.h"

/*
 * XXX
 *  - worry about i_ino trunctation, not sure if we do anything
 *  - use inode item value lengths for forward/back compat
 */

/*
 * XXX before committing:
 *  - describe all this better
 *  - describe data locking size problems
 */

struct free_ino_pool {
	wait_queue_head_t waitq;
	spinlock_t lock;
	u64 ino;
	u64 nr;
	bool in_flight;
};

struct inode_sb_info {
	struct free_ino_pool pool;

	spinlock_t writeback_lock;
	struct rb_root writeback_inodes;
};

#define DECLARE_INODE_SB_INFO(sb, name) \
	struct inode_sb_info *name = SCOUTFS_SB(sb)->inode_sb_info

static struct kmem_cache *scoutfs_inode_cachep;

/*
 * This is called once before all the allocations and frees of a inode
 * object within a slab.  It's for inode fields that don't need to be
 * initialized for a given instance of an inode.
 */
static void scoutfs_inode_ctor(void *obj)
{
	struct scoutfs_inode_info *ci = obj;

	mutex_init(&ci->item_mutex);
	seqcount_init(&ci->seqcount);
	ci->staging = false;
	scoutfs_per_task_init(&ci->pt_data_lock);
	init_rwsem(&ci->xattr_rwsem);
	RB_CLEAR_NODE(&ci->writeback_node);

	inode_init_once(&ci->inode);
}

struct inode *scoutfs_alloc_inode(struct super_block *sb)
{
	struct scoutfs_inode_info *ci;

	ci = kmem_cache_alloc(scoutfs_inode_cachep, GFP_NOFS);
	if (!ci)
		return NULL;

	return &ci->inode;
}

static void scoutfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	trace_scoutfs_i_callback(inode);
	kmem_cache_free(scoutfs_inode_cachep, SCOUTFS_I(inode));
}

static void insert_writeback_inode(struct inode_sb_info *inf,
				   struct scoutfs_inode_info *ins)
{
	struct rb_root *root = &inf->writeback_inodes;
	struct rb_node **node = &root->rb_node;
	struct rb_node *parent = NULL;
	struct scoutfs_inode_info *si;

	while (*node) {
		parent = *node;
		si = container_of(*node, struct scoutfs_inode_info,
				  writeback_node);

		if (ins->ino < si->ino)
			node = &(*node)->rb_left;
		else if (ins->ino > si->ino)
			node = &(*node)->rb_right;
		else
			BUG();
	}

	rb_link_node(&ins->writeback_node, parent, node);
	rb_insert_color(&ins->writeback_node, root);
}

static void remove_writeback_inode(struct inode_sb_info *inf,
			       struct scoutfs_inode_info *si)
{
	if (!RB_EMPTY_NODE(&si->writeback_node)) {
		rb_erase(&si->writeback_node, &inf->writeback_inodes);
		RB_CLEAR_NODE(&si->writeback_node);
	}
}

void scoutfs_destroy_inode(struct inode *inode)
{
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);

	spin_lock(&inf->writeback_lock);
	remove_writeback_inode(inf, SCOUTFS_I(inode));
	spin_unlock(&inf->writeback_lock);

	call_rcu(&inode->i_rcu, scoutfs_i_callback);
}

static const struct inode_operations scoutfs_file_iops = {
	.getattr	= scoutfs_getattr,
	.setattr	= scoutfs_setattr,
	.setxattr	= scoutfs_setxattr,
	.getxattr	= scoutfs_getxattr,
	.listxattr	= scoutfs_listxattr,
	.removexattr	= scoutfs_removexattr,
	.fiemap		= scoutfs_data_fiemap,
};

static const struct inode_operations scoutfs_special_iops = {
	.getattr	= scoutfs_getattr,
	.setattr	= scoutfs_setattr,
	.setxattr	= scoutfs_setxattr,
	.getxattr	= scoutfs_getxattr,
	.listxattr	= scoutfs_listxattr,
	.removexattr	= scoutfs_removexattr,
};

/*
 * Called once new inode allocation or inode reading has initialized
 * enough of the inode for us to set the ops based on the mode.
 */
static void set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_mapping->a_ops = &scoutfs_file_aops;
		inode->i_op = &scoutfs_file_iops;
		inode->i_fop = &scoutfs_file_fops;
		break;
	case S_IFDIR:
		inode->i_op = &scoutfs_dir_iops;
		inode->i_fop = &scoutfs_dir_fops;
		break;
	case S_IFLNK:
		inode->i_op = &scoutfs_symlink_iops;
		break;
	default:
		inode->i_op = &scoutfs_special_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}

	/* ephemeral data items avoid kmap for pointers to page contents */
	mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
}

/*
 * The caller has ensured that the fields in the incoming scoutfs inode
 * reflect both the inode item and the inode index items.  This happens
 * when reading, refreshing, or updating the inodes.  We set the inode
 * info fields to match so that next time we try to update the inode we
 * can tell which fields have changed.
 */
static void set_item_info(struct scoutfs_inode_info *si,
			  struct scoutfs_inode *sinode)
{
	BUG_ON(!mutex_is_locked(&si->item_mutex));

	memset(si->item_majors, 0, sizeof(si->item_majors));
	memset(si->item_minors, 0, sizeof(si->item_minors));

	si->have_item = true;
	si->item_majors[SCOUTFS_INODE_INDEX_SIZE_TYPE] =
		le64_to_cpu(sinode->size);
	si->item_majors[SCOUTFS_INODE_INDEX_META_SEQ_TYPE] =
		le64_to_cpu(sinode->meta_seq);
	si->item_majors[SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE] =
		le64_to_cpu(sinode->data_seq);
}

static void load_inode(struct inode *inode, struct scoutfs_inode *cinode)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);

	i_size_write(inode, le64_to_cpu(cinode->size));
	set_nlink(inode, le32_to_cpu(cinode->nlink));
	i_uid_write(inode, le32_to_cpu(cinode->uid));
	i_gid_write(inode, le32_to_cpu(cinode->gid));
	inode->i_mode = le32_to_cpu(cinode->mode);
	inode->i_rdev = le32_to_cpu(cinode->rdev);
	inode->i_atime.tv_sec = le64_to_cpu(cinode->atime.sec);
	inode->i_atime.tv_nsec = le32_to_cpu(cinode->atime.nsec);
	inode->i_mtime.tv_sec = le64_to_cpu(cinode->mtime.sec);
	inode->i_mtime.tv_nsec = le32_to_cpu(cinode->mtime.nsec);
	inode->i_ctime.tv_sec = le64_to_cpu(cinode->ctime.sec);
	inode->i_ctime.tv_nsec = le32_to_cpu(cinode->ctime.nsec);

	ci->meta_seq = le64_to_cpu(cinode->meta_seq);
	ci->data_seq = le64_to_cpu(cinode->data_seq);
	ci->data_version = le64_to_cpu(cinode->data_version);
	ci->next_readdir_pos = le64_to_cpu(cinode->next_readdir_pos);

	ci->flags = le32_to_cpu(cinode->flags);

	set_item_info(ci, cinode);
}

/*
 * Refresh the vfs inode fields if the lock indicates that the current
 * contents could be stale.
 *
 * This can be racing with many lock holders of an inode.  A bunch of
 * readers can be checking to refresh while one of them is refreshing.
 *
 * The vfs inode field updates can't be racing with valid readers of the
 * fields because they should have already had a locked refreshed inode
 * to be dereferencing its contents.
 */
int scoutfs_inode_refresh(struct inode *inode, struct scoutfs_lock *lock,
			  int flags)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_key_buf key;
	struct scoutfs_inode_key ikey;
	struct scoutfs_inode sinode;
	SCOUTFS_DECLARE_KVEC(val);
	const u64 refresh_gen = scoutfs_lock_refresh_gen(lock);
	int ret;

	/*
	 * Lock refresh gens are supposed to strictly increase.  Inodes
	 * having a greater gen means memory corruption or
	 * lifetime/logic bugs that could stop the inode from refreshing
	 * and expose stale data.
	 */
	BUG_ON(atomic64_read(&si->last_refreshed) > refresh_gen);

	if (atomic64_read(&si->last_refreshed) == refresh_gen)
		return 0;

	scoutfs_inode_init_key(&key, &ikey, scoutfs_ino(inode));
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	mutex_lock(&si->item_mutex);
	if (atomic64_read(&si->last_refreshed) < refresh_gen) {
		ret = scoutfs_item_lookup_exact(sb, &key, val, sizeof(sinode),
						lock);
		if (ret == 0) {
			load_inode(inode, &sinode);
			atomic64_set(&si->last_refreshed, refresh_gen);
		}
	} else {
		ret = 0;
	}
	mutex_unlock(&si->item_mutex);

	return ret;
}

void scoutfs_inode_init_key(struct scoutfs_key_buf *key,
			    struct scoutfs_inode_key *ikey, u64 ino)
{
	ikey->zone = SCOUTFS_FS_ZONE;
	ikey->ino = cpu_to_be64(ino);
	ikey->type = SCOUTFS_INODE_TYPE;

	scoutfs_key_init(key, ikey, sizeof(struct scoutfs_inode_key));
}

int scoutfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		    struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct scoutfs_lock *lock = NULL;
	int ret;

	ret = scoutfs_lock_inode(sb, DLM_LOCK_PR, SCOUTFS_LKF_REFRESH_INODE,
				 inode, &lock);
	if (ret == 0) {
		generic_fillattr(inode, stat);
		scoutfs_unlock(sb, lock, DLM_LOCK_PR);
	}
	return ret;
}

static int set_inode_size(struct inode *inode, struct scoutfs_lock *lock,
			  u64 new_size, bool truncate)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	LIST_HEAD(ind_locks);
	int ret;

	if (!S_ISREG(inode->i_mode))
		return 0;

	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks, new_size, true,
					    SIC_DIRTY_INODE());
	if (ret)
		return ret;

	truncate_setsize(inode, new_size);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	if (truncate)
		ci->flags |= SCOUTFS_INO_FLAG_TRUNCATE;
	scoutfs_inode_set_data_seq(inode);
	scoutfs_update_inode_item(inode, lock, &ind_locks);

	scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);

	return ret;
}

static int clear_truncate_flag(struct inode *inode, struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	LIST_HEAD(ind_locks);
	int ret;

	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks,
					    i_size_read(inode), false,
					    SIC_DIRTY_INODE());
	if (ret)
		return ret;

	ci->flags &= ~SCOUTFS_INO_FLAG_TRUNCATE;
	scoutfs_update_inode_item(inode, lock, &ind_locks);

	scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);

	return ret;
}

int scoutfs_complete_truncate(struct inode *inode, struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 start;
	int ret, err;

	trace_scoutfs_complete_truncate(inode, ci->flags);

	if (!(ci->flags & SCOUTFS_INO_FLAG_TRUNCATE))
		return 0;

	start = (i_size_read(inode) + SCOUTFS_BLOCK_SIZE - 1) >> SCOUTFS_BLOCK_SHIFT;
	ret = scoutfs_data_truncate_items(inode->i_sb, scoutfs_ino(inode),
					  start, ~0ULL, false, lock);
	err = clear_truncate_flag(inode, lock);

	return ret ? ret : err;
}

int scoutfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct scoutfs_lock *lock = NULL;
	LIST_HEAD(ind_locks);
	bool truncate = false;
	u64 attr_size;
	int ret;

	trace_scoutfs_setattr(dentry, attr);

	ret = scoutfs_lock_inode(sb, DLM_LOCK_EX, SCOUTFS_LKF_REFRESH_INODE,
				 inode, &lock);
	if (ret)
		return ret;

	ret = inode_change_ok(inode, attr);
	if (ret)
		goto out;

	attr_size = (attr->ia_valid & ATTR_SIZE) ? attr->ia_size :
		i_size_read(inode);

	if (S_ISREG(inode->i_mode) && attr->ia_valid & ATTR_SIZE) {
		/*
		 * Complete any truncates that may have failed while
		 * in progress
		 */
		ret = scoutfs_complete_truncate(inode, lock);
		if (ret)
			goto out;

		truncate = i_size_read(inode) > attr_size;

		ret = set_inode_size(inode, lock, attr_size, truncate);
		if (ret)
			goto out;

		if (truncate) {
			ret = scoutfs_complete_truncate(inode, lock);
			if (ret)
				goto out;
		}
	}

	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks,
					    i_size_read(inode), false,
					    SIC_DIRTY_INODE());
	if (ret)
		goto out;

	setattr_copy(inode, attr);
	scoutfs_update_inode_item(inode, lock, &ind_locks);

	scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);
out:
	scoutfs_unlock(sb, lock, DLM_LOCK_EX);
	return ret;
}

/*
 * Set a given seq to the current trans seq if it differs.  The caller
 * holds locks and a transaction which prevents the transaction from
 * committing and refreshing the seq.
 */
static void set_trans_seq(struct inode *inode, u64 *seq)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);

	if (*seq != sbi->trans_seq) {
		preempt_disable();
		write_seqcount_begin(&si->seqcount);
		*seq = sbi->trans_seq;
		write_seqcount_end(&si->seqcount);
		preempt_enable();
	}
}

void scoutfs_inode_set_meta_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	set_trans_seq(inode, &si->meta_seq);
}

void scoutfs_inode_set_data_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	set_trans_seq(inode, &si->data_seq);
}

void scoutfs_inode_inc_data_version(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	preempt_disable();
	write_seqcount_begin(&si->seqcount);
	si->data_version++;
	write_seqcount_end(&si->seqcount);
	preempt_enable();
}

static u64 read_seqcount_u64(struct inode *inode, u64 *val)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	unsigned int seq;
	u64 v;

	do {
		seq = read_seqcount_begin(&si->seqcount);
		v = *val;
	} while (read_seqcount_retry(&si->seqcount, seq));

	return v;
}

u64 scoutfs_inode_meta_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->meta_seq);
}

u64 scoutfs_inode_data_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->data_seq);
}

u64 scoutfs_inode_data_version(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->data_version);
}

static int scoutfs_iget_test(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 *ino = arg;

	return ci->ino == *ino;
}

static int scoutfs_iget_set(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);
	u64 *ino = arg;

	inode->i_ino = *ino;
	ci->ino = *ino;

	return 0;
}

struct inode *scoutfs_ilookup(struct super_block *sb, u64 ino)
{
	return ilookup5(sb, ino, scoutfs_iget_test, &ino);
}

struct inode *scoutfs_iget(struct super_block *sb, u64 ino)
{
	struct inode *inode;
	struct scoutfs_lock *lock = NULL;
	int ret;

	ret = scoutfs_lock_ino(sb, DLM_LOCK_PR, 0, ino, &lock);
	if (ret)
		return ERR_PTR(ret);

	inode = iget5_locked(sb, ino, scoutfs_iget_test, scoutfs_iget_set,
			     &ino);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}

	if (inode->i_state & I_NEW) {
		/* XXX ensure refresh, instead clear in drop_inode? */
		atomic64_set(&SCOUTFS_I(inode)->last_refreshed, 0);

		ret = scoutfs_inode_refresh(inode, lock, 0);
		if (ret) {
			iget_failed(inode);
			inode = ERR_PTR(ret);
		} else {
			set_inode_ops(inode);
			unlock_new_inode(inode);
		}
	}

out:
	scoutfs_unlock(sb, lock, DLM_LOCK_PR);
	return inode;
}

static void store_inode(struct scoutfs_inode *cinode, struct inode *inode)
{
	struct scoutfs_inode_info *ci = SCOUTFS_I(inode);

	cinode->size = cpu_to_le64(i_size_read(inode));
	cinode->nlink = cpu_to_le32(inode->i_nlink);
	cinode->uid = cpu_to_le32(i_uid_read(inode));
	cinode->gid = cpu_to_le32(i_gid_read(inode));
	cinode->mode = cpu_to_le32(inode->i_mode);
	cinode->rdev = cpu_to_le32(inode->i_rdev);
	cinode->atime.sec = cpu_to_le64(inode->i_atime.tv_sec);
	cinode->atime.nsec = cpu_to_le32(inode->i_atime.tv_nsec);
	cinode->ctime.sec = cpu_to_le64(inode->i_ctime.tv_sec);
	cinode->ctime.nsec = cpu_to_le32(inode->i_ctime.tv_nsec);
	cinode->mtime.sec = cpu_to_le64(inode->i_mtime.tv_sec);
	cinode->mtime.nsec = cpu_to_le32(inode->i_mtime.tv_nsec);

	cinode->meta_seq = cpu_to_le64(scoutfs_inode_meta_seq(inode));
	cinode->data_seq = cpu_to_le64(scoutfs_inode_data_seq(inode));
	cinode->data_version = cpu_to_le64(scoutfs_inode_data_version(inode));
	cinode->next_readdir_pos = cpu_to_le64(ci->next_readdir_pos);
	cinode->flags = cpu_to_le32(ci->flags);
}

/*
 * Create a pinned dirty inode item so that we can later update the
 * inode item without risking failure.  We often wouldn't want to have
 * to unwind inode modifcations (perhaps by shared vfs code!) if our
 * item update failed.  This is our chance to return errors for enospc
 * for lack of space for new logged dirty inode items.
 *
 * This dirty inode item will be found by lookups in the interim so we
 * have to update it now with the current inode contents.
 *
 * Callers don't delete these dirty items on errors.  They're still
 * valid and will be merged with the current item eventually.  They can
 * be found in the dirty block to avoid future dirtying (say repeated
 * creations in a directory).
 *
 * The caller has to prevent sync between dirtying and updating the
 * inodes.
 *
 * XXX this will have to do something about variable length inodes
 */
int scoutfs_dirty_inode_item(struct inode *inode, struct scoutfs_lock *lock)
{
	struct super_block *sb = inode->i_sb;
	struct scoutfs_inode_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_inode sinode;
	int ret;

	store_inode(&sinode, inode);

	scoutfs_inode_init_key(&key, &ikey, scoutfs_ino(inode));

	ret = scoutfs_item_dirty(sb, &key, lock);
	if (!ret)
		trace_scoutfs_dirty_inode(inode);
	return ret;
}

struct index_lock {
	struct list_head head;
	struct scoutfs_lock *lock;
	u8 type;
	u64 major;
	u32 minor;
	u64 ino;
};

static bool will_del_index(struct scoutfs_inode_info *si,
			   u8 type, u64 major, u32 minor)
{
	return si && si->have_item &&
	       (si->item_majors[type] != major ||
		si->item_minors[type] != minor);
}

static bool will_ins_index(struct scoutfs_inode_info *si,
			   u8 type, u64 major, u32 minor)
{
	return !si || !si->have_item ||
	       (si->item_majors[type] != major ||
		si->item_minors[type] != minor);
}

static bool inode_has_index(umode_t mode, u8 type)
{
	switch(type) {
		case SCOUTFS_INODE_INDEX_SIZE_TYPE:
		case SCOUTFS_INODE_INDEX_META_SEQ_TYPE:
			return true;
		case SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE:
			return S_ISREG(mode);
		default:
			return WARN_ON_ONCE(false);
	}
}

static int cmp_index_lock(void *priv, struct list_head *A, struct list_head *B)
{
	struct index_lock *a = list_entry(A, struct index_lock, head);
	struct index_lock *b = list_entry(B, struct index_lock, head);

	return ((int)a->type - (int)b->type) ?:
	       scoutfs_cmp_u64s(a->major, b->major) ?:
	       scoutfs_cmp_u64s(a->minor, b->minor) ?:
	       scoutfs_cmp_u64s(a->ino, b->ino);
}

static void clamp_inode_index(u8 type, u64 *major, u32 *minor, u64 *ino)
{
	struct scoutfs_inode_index_key start;

	scoutfs_lock_get_index_item_range(type, *major, *ino, &start, NULL);

	*major = be64_to_cpu(start.major);
	*minor = be32_to_cpu(start.minor);
	*ino = be64_to_cpu(start.ino);
}

/*
 * Find the lock that covers the given index item.  Returns NULL if
 * there isn't a lock that covers the item.  We know that the list is
 * sorted at this point so we can stop once our search value is less
 * than a list entry.
 */
static struct scoutfs_lock *find_index_lock(struct list_head *lock_list,
					    u8 type, u64 major, u32 minor,
					    u64 ino)
{
	struct index_lock *ind_lock;
	struct index_lock needle;
	int cmp;

	clamp_inode_index(type, &major, &minor, &ino);
	needle.type = type;
	needle.major = major;
	needle.minor = minor;
	needle.ino = ino;

	list_for_each_entry(ind_lock, lock_list, head) {
		cmp = cmp_index_lock(NULL, &needle.head, &ind_lock->head);
		if (cmp == 0)
			return ind_lock->lock;
		if (cmp < 0)
			break;
	}

	return NULL;
}

/*
 * The inode info reflects the current inode index items.  Create or delete
 * index items to bring the index in line with the caller's item.  The list
 * should contain locks that cover any item modifications that are made.
 */
static int update_index_items(struct super_block *sb,
			      struct scoutfs_inode_info *si, u64 ino, u8 type,
			      u64 major, u32 minor,
			      struct list_head *lock_list)
{
	struct scoutfs_inode_index_key ins_ikey;
	struct scoutfs_inode_index_key del_ikey;
	struct scoutfs_lock *ins_lock;
	struct scoutfs_lock *del_lock;
	struct scoutfs_key_buf ins;
	struct scoutfs_key_buf del;
	int ret;
	int err;

	if (!will_ins_index(si, type, major, minor))
		return 0;

	trace_scoutfs_create_index_item(sb, type, major, minor, ino);

	ins_ikey.zone = SCOUTFS_INODE_INDEX_ZONE;
	ins_ikey.type = type;
	ins_ikey.major = cpu_to_be64(major);
	ins_ikey.minor = cpu_to_be32(minor);
	ins_ikey.ino = cpu_to_be64(ino);
	scoutfs_key_init(&ins, &ins_ikey, sizeof(ins_ikey));

	ins_lock = find_index_lock(lock_list, type, major, minor, ino);
	ret = scoutfs_item_create_force(sb, &ins, NULL, ins_lock);
	if (ret || !will_del_index(si, type, major, minor))
		return ret;

	trace_scoutfs_delete_index_item(sb, type, si->item_majors[type],
					si->item_minors[type], ino);

	del_ikey.zone = SCOUTFS_INODE_INDEX_ZONE;
	del_ikey.type = type;
	del_ikey.major = cpu_to_be64(si->item_majors[type]);
	del_ikey.minor = cpu_to_be32(si->item_minors[type]);
	del_ikey.ino = cpu_to_be64(ino);
	scoutfs_key_init(&del, &del_ikey, sizeof(del_ikey));

	del_lock = find_index_lock(lock_list, type, si->item_majors[type],
				   si->item_minors[type], ino);
	ret = scoutfs_item_delete_force(sb, &del, del_lock);
	if (ret) {
		err = scoutfs_item_delete(sb, &ins, ins_lock);
		BUG_ON(err);
	}

	return ret;
}

static int update_indices(struct super_block *sb,
			  struct scoutfs_inode_info *si, u64 ino, umode_t mode,
			  struct scoutfs_inode *sinode,
			  struct list_head *lock_list)
{
	struct index_update {
		u8 type;
		u64 major;
		u32 minor;
	} *upd, upds[] = {
		{ SCOUTFS_INODE_INDEX_SIZE_TYPE,
			le64_to_cpu(sinode->size), 0 },
		{ SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			le64_to_cpu(sinode->meta_seq), 0 },
		{ SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
			le64_to_cpu(sinode->data_seq), 0 },
	};
	int ret;
	int i;

	for (i = 0, upd = upds; i < ARRAY_SIZE(upds); i++, upd++) {
		if (!inode_has_index(mode, upd->type))
			continue;

		ret = update_index_items(sb, si, ino, upd->type, upd->major,
					 upd->minor, lock_list);
		if (ret)
			break;
	}

	return ret;
}

/*
 * Every time we modify the inode in memory we copy it to its inode
 * item.  This lets us write out items without having to track down
 * dirty vfs inodes.
 *
 * The caller makes sure that the item is dirty and pinned so they don't
 * have to deal with errors and unwinding after they've modified the vfs
 * inode and get here.
 *
 * Index items that track inode fields are updated here as we update the
 * inode item.  The caller must have acquired locks on all the index
 * items that might change.
 */
void scoutfs_update_inode_item(struct inode *inode, struct scoutfs_lock *lock,
			       struct list_head *lock_list)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	const u64 ino = scoutfs_ino(inode);
	struct scoutfs_inode_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_inode sinode;
	SCOUTFS_DECLARE_KVEC(val);
	int ret;
	int err;

	mutex_lock(&si->item_mutex);

	/* set the meta version once per trans for any inode updates */
	scoutfs_inode_set_meta_seq(inode);

	/* only race with other inode field stores once */
	store_inode(&sinode, inode);

	ret = update_indices(sb, si, ino, inode->i_mode, &sinode, lock_list);
	BUG_ON(ret);

	scoutfs_inode_init_key(&key, &ikey, ino);
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	err = scoutfs_item_update(sb, &key, val, lock);
	if (err) {
		scoutfs_err(sb, "inode %llu update err %d", ino, err);
		BUG_ON(err);
	}

	set_item_info(si, &sinode);
	trace_scoutfs_update_inode(inode);

	mutex_unlock(&si->item_mutex);
}

/*
 * We map the item to coarse locks here.  This reduces the number of
 * locks we track and means that when we later try to find the lock that
 * covers an item we can deal with the item update changing a little
 * (seq, size) while still being covered.  It does mean we have to share
 * some logic with lock naming.
 */
static int add_index_lock(struct list_head *list, u64 ino, u8 type, u64 major,
			  u32 minor)
{
	struct index_lock *ind_lock;

	clamp_inode_index(type, &major, &minor, &ino);

	list_for_each_entry(ind_lock, list, head) {
		if (ind_lock->type == type && ind_lock->major == major &&
		    ind_lock->minor == minor && ind_lock->ino == ino) {
			return 0;
		}
	}

	ind_lock = kzalloc(sizeof(struct index_lock), GFP_NOFS);
	if (!ind_lock)
		return -ENOMEM;

	ind_lock->type = type;
	ind_lock->major = major;
	ind_lock->minor = minor;
	ind_lock->ino = ino;
	list_add(&ind_lock->head, list);

	return 0;
}

static int prepare_index_items(struct scoutfs_inode_info *si,
			       struct list_head *list, u64 ino, umode_t mode,
			       u8 type, u64 major, u32 minor)
{
	int ret;

	if (will_ins_index(si, type, major, minor)) {
		ret = add_index_lock(list, ino, type, major, minor);
		if (ret)
			return ret;
	}

	if (will_del_index(si, type, major, minor)) {
		ret = add_index_lock(list, ino, type, si->item_majors[type],
				     si->item_minors[type]);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Return the data seq that we expect to see in the updated inode.  The
 * caller tells us if they know they're going to update it.  If the
 * inode doesn't exist it'll also get the current data_seq.
 */
static u64 upd_data_seq(struct scoutfs_sb_info *sbi,
			struct scoutfs_inode_info *si, bool set_data_seq)
{
	if (!si || !si->have_item || set_data_seq)
		return sbi->trans_seq;

	return si->item_majors[SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE];
}

/*
 * Prepare locks that will cover the inode index items that will be
 * modified when this inode's item is updated during the upcoming
 * transaction.
 *
 * To lock the index items that will be created we need to predict the
 * new indexed values.  We assume that the meta seq will always be set
 * to the current seq.  This will usually be a nop in a running
 * transaction.  The caller tells us what the size will be and whether
 * data_seq will also be set to the current transaction.
 */
static int prepare_indices(struct super_block *sb, struct list_head *list,
			   struct scoutfs_inode_info *si, u64 ino,
			   umode_t mode, u64 new_size, bool set_data_seq)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct index_update {
		u8 type;
		u64 major;
		u32 minor;
	} *upd, upds[] = {
		{ SCOUTFS_INODE_INDEX_SIZE_TYPE, new_size, 0},
		{ SCOUTFS_INODE_INDEX_META_SEQ_TYPE, sbi->trans_seq, 0},
		{ SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
			upd_data_seq(sbi, si, set_data_seq), 0},
	};
	int ret;
	int i;

	for (i = 0, upd = upds; i < ARRAY_SIZE(upds); i++, upd++) {
		if (!inode_has_index(mode, upd->type))
			continue;

		ret = prepare_index_items(si, list, ino, mode,
					  upd->type, upd->major, upd->minor);
		if (ret)
			break;
	}

	return ret;
}

int scoutfs_inode_index_prepare(struct super_block *sb, struct list_head *list,
			        struct inode *inode, u64 new_size,
				bool set_data_seq)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return prepare_indices(sb, list, si, scoutfs_ino(inode),
			       inode->i_mode, new_size, set_data_seq);
}

/*
 * This is used to initially create the index items for a newly created
 * inode.  We don't have a populated vfs inode yet.  The existing
 * indexed values don't matter because it's 'have_item' is false.  It
 * will try to create all the appropriate index items.
 */
int scoutfs_inode_index_prepare_ino(struct super_block *sb,
				    struct list_head *list, u64 ino,
				    umode_t mode, u64 new_size)
{
	return prepare_indices(sb, list, NULL, ino, mode, new_size, true);
}

/*
 * Prepare the locks needed to delete all the index items associated
 * with the inode.  We know the items have to exist and can skip straight
 * to adding locks for each of them.
 */
static int prepare_index_deletion(struct super_block *sb,
				  struct list_head *list, u64 ino,
				  umode_t mode, struct scoutfs_inode *sinode)
{
	struct index_item {
		u8 type;
		u64 major;
		u32 minor;
	} *ind, inds[] = {
		{ SCOUTFS_INODE_INDEX_SIZE_TYPE,
			le64_to_cpu(sinode->size), 0 },
		{ SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			le64_to_cpu(sinode->meta_seq), 0 },
		{ SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
			le64_to_cpu(sinode->data_seq), 0 },
	};
	int ret;
	int i;

	for (i = 0, ind = inds; i < ARRAY_SIZE(inds); i++, ind++) {
		if (!inode_has_index(mode, ind->type))
			continue;

		ret = add_index_lock(list, ino, ind->type,  ind->major,
				     ind->minor);
		if (ret)
			break;
	}

	return ret;
}

/*
 * Sample the transaction sequence before we start checking it to see if
 * indexed meta seq and data seq items will change.
 */
int scoutfs_inode_index_start(struct super_block *sb, u64 *seq)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);

	/* XXX this feels racey in a bad way :) */
	*seq = sbi->trans_seq;
	return 0;
}

/*
 * Acquire the prepared index locks and hold the transaction.  If the
 * sequence number changes as we enter the transaction then we need to
 * retry so that we can use the new seq to prepare locks.
 *
 * Returns > 0 if the seq changed and the locks should be retried.
 */
int scoutfs_inode_index_try_lock_hold(struct super_block *sb,
				      struct list_head *list, u64 seq,
				      const struct scoutfs_item_count cnt)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct index_lock *ind_lock;
	int ret = 0;

	list_sort(NULL, list, cmp_index_lock);

	list_for_each_entry(ind_lock, list, head) {
		ret = scoutfs_lock_inode_index(sb, DLM_LOCK_CW, ind_lock->type,
					       ind_lock->major, ind_lock->ino,
					       &ind_lock->lock);
		if (ret)
			goto out;
	}

	ret = scoutfs_hold_trans(sb, cnt);
	if (ret == 0 && seq != sbi->trans_seq) {
		scoutfs_release_trans(sb);
		ret = 1;
	}

out:
	if (ret)
		scoutfs_inode_index_unlock(sb, list);

	return ret;
}

int scoutfs_inode_index_lock_hold(struct inode *inode, struct list_head *list,
				  u64 size, bool set_data_seq,
				  const struct scoutfs_item_count cnt)
{
	struct super_block *sb = inode->i_sb;
	int ret;
	u64 seq;

	do {
		ret = scoutfs_inode_index_start(sb, &seq) ?:
		      scoutfs_inode_index_prepare(sb, list, inode, size,
						  set_data_seq) ?:
		      scoutfs_inode_index_try_lock_hold(sb, list, seq, cnt);
	} while (ret > 0);

	return ret;
}

/*
 * Unlocks and frees all the locks on the list.
 */
void scoutfs_inode_index_unlock(struct super_block *sb, struct list_head *list)
{
	struct index_lock *ind_lock;
	struct index_lock *tmp;

	list_for_each_entry_safe(ind_lock, tmp, list, head) {
		scoutfs_unlock(sb, ind_lock->lock, DLM_LOCK_CW);
		list_del_init(&ind_lock->head);
		kfree(ind_lock);
	}
}

/* this is called on final inode cleanup so enoent is fine */
static int remove_index(struct super_block *sb, u64 ino, u8 type, u64 major,
			u32 minor, struct list_head *ind_locks)
{
	struct scoutfs_inode_index_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_lock *lock;
	int ret;

	ikey.zone = SCOUTFS_INODE_INDEX_ZONE;
	ikey.type = type;
	ikey.major = cpu_to_be64(major);
	ikey.minor = cpu_to_be32(minor);
	ikey.ino = cpu_to_be64(ino);
	scoutfs_key_init(&key, &ikey, sizeof(ikey));

	lock = find_index_lock(ind_locks, type, major, minor, ino);
	ret = scoutfs_item_delete_force(sb, &key, lock);
	if (ret == -ENOENT)
		ret = 0;
	return ret;
}

/*
 * Remove all the inode's index items.  The caller has ensured that
 * there are no more active users of the inode.  This can be racing with
 * users of the inode index items.  Once we can use them we'll get CW
 * locks around the index items to invalidate remote caches.  Racing
 * users of the index items already have to deal with the possibility
 * that the inodes returned by the index queries can go out of sync by
 * the time they get to it, including being deleted.
 */
static int remove_index_items(struct super_block *sb, u64 ino,
			      struct scoutfs_inode *sinode,
			      struct list_head *ind_locks)
{
	umode_t mode = le32_to_cpu(sinode->mode);
	int ret;

	ret = remove_index(sb, ino, SCOUTFS_INODE_INDEX_SIZE_TYPE,
			   le64_to_cpu(sinode->size), 0, ind_locks) ?:
	      remove_index(sb, ino, SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			   le64_to_cpu(sinode->meta_seq), 0, ind_locks);
	if (ret == 0 && S_ISREG(mode))
		ret = remove_index(sb, ino, SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
				   le64_to_cpu(sinode->data_seq), 0, ind_locks);
	return ret;
}

/*
 * A quick atomic sample of the last inode number that's been allocated.
 */
u64 scoutfs_last_ino(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	u64 last;

	spin_lock(&sbi->next_ino_lock);
	last = le64_to_cpu(super->next_ino);
	spin_unlock(&sbi->next_ino_lock);

	return last;
}

/*
 * Network replies refill the pool, providing ino = ~0ULL nr = 0 when
 * there's no more inodes (which should never happen in practice.)
 */
void scoutfs_inode_fill_pool(struct super_block *sb, u64 ino, u64 nr)
{
	struct free_ino_pool *pool = &SCOUTFS_SB(sb)->inode_sb_info->pool;

	trace_scoutfs_inode_fill_pool(sb, ino, nr);

	spin_lock(&pool->lock);

	pool->ino = ino;
	pool->nr = nr;
	pool->in_flight = false;

	spin_unlock(&pool->lock);

	wake_up(&pool->waitq);
}

static bool pool_in_flight(struct free_ino_pool *pool)
{
	bool in_flight;

	spin_lock(&pool->lock);
	in_flight = pool->in_flight;
	spin_unlock(&pool->lock);

	return in_flight;
}

/*
 * We have a pool of free inodes given to us by the server.  If it
 * empties we only ever have one request for new inodes in flight.  The
 * net layer calls us when it gets a reply.  If there's no more inodes
 * we'll get ino == ~0 and nr == 0.
 */
int scoutfs_alloc_ino(struct super_block *sb, u64 *ino)
{
	struct free_ino_pool *pool = &SCOUTFS_SB(sb)->inode_sb_info->pool;
	bool request;
	int ret;

	*ino = 0;

	spin_lock(&pool->lock);

	while (pool->nr == 0 && pool->ino != ~0ULL) {
		if (pool->in_flight) {
			request = false;
		} else {
			pool->in_flight = true;
			request = true;
		}

		spin_unlock(&pool->lock);

		if (request) {
			ret = scoutfs_client_alloc_inodes(sb);
			if (ret) {
				spin_lock(&pool->lock);
				pool->in_flight = false;
				spin_unlock(&pool->lock);
				wake_up(&pool->waitq);
				goto out;
			}
		}

		ret = wait_event_interruptible(pool->waitq,
					       !pool_in_flight(pool));
		if (ret)
			goto out;

		spin_lock(&pool->lock);
	}

	if (pool->nr == 0) {
		*ino = 0;
		ret = -ENOSPC;
	} else {
		*ino = pool->ino++;
		pool->nr--;
		ret = 0;

	}

	spin_unlock(&pool->lock);

out:

	trace_scoutfs_alloc_ino(sb, ret, *ino, pool->ino, pool->nr,
				pool->in_flight);
	return ret;
}

/*
 * Allocate and initialize a new inode.  The caller is responsible for
 * creating links to it and updating it.  @dir can be null.
 */
struct inode *scoutfs_new_inode(struct super_block *sb, struct inode *dir,
				umode_t mode, dev_t rdev, u64 ino,
				struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *ci;
	struct scoutfs_inode_key ikey;
	struct scoutfs_key_buf key;
	struct scoutfs_inode sinode;
	SCOUTFS_DECLARE_KVEC(val);
	struct inode *inode;
	int ret;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ci = SCOUTFS_I(inode);
	ci->ino = ino;
	ci->data_version = 0;
	ci->next_readdir_pos = SCOUTFS_DIRENT_FIRST_POS;
	ci->have_item = false;
	atomic64_set(&ci->last_refreshed, scoutfs_lock_refresh_gen(lock));
	ci->flags = 0;

	scoutfs_inode_set_meta_seq(inode);
	scoutfs_inode_set_data_seq(inode);

	inode->i_ino = ino; /* XXX overflow */
	inode_init_owner(inode, dir, mode);
	inode_set_bytes(inode, 0);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_rdev = rdev;
	set_inode_ops(inode);

	store_inode(&sinode, inode);
	scoutfs_inode_init_key(&key, &ikey, scoutfs_ino(inode));
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	ret = scoutfs_item_create(sb, &key, val, lock);
	if (ret) {
		iput(inode);
		return ERR_PTR(ret);
	}

	return inode;
}

static void init_orphan_key(struct scoutfs_key_buf *key,
			    struct scoutfs_orphan_key *okey, u64 node_id, u64 ino)
{
	okey->zone = SCOUTFS_NODE_ZONE;
	okey->node_id = cpu_to_be64(node_id);
	okey->type = SCOUTFS_ORPHAN_TYPE;
	okey->ino = cpu_to_be64(ino);

	scoutfs_key_init(key, okey, sizeof(struct scoutfs_orphan_key));
}

static int remove_orphan_item(struct super_block *sb, u64 ino)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_lock *lock = sbi->node_id_lock;
	struct scoutfs_orphan_key okey;
	struct scoutfs_key_buf key;
	int ret;

	init_orphan_key(&key, &okey, sbi->node_id, ino);

	ret = scoutfs_item_delete(sb, &key, lock);
	if (ret == -ENOENT)
		ret = 0;

	return ret;
}

/*
 * Remove all the items associated with a given inode.  This is only
 * called once nlink has dropped to zero so we don't have to worry about
 * dirents referencing the inode or link backrefs.  Dropping nlink to 0
 * also created an orphan item.  That orphan item will continue
 * triggering attempts to finish previous partial deletion until all
 * deletion is complete and the orphan item is removed.
 */
static int delete_inode_items(struct super_block *sb, u64 ino)
{
	struct scoutfs_lock *lock = NULL;
	struct scoutfs_inode_key ikey;
	struct scoutfs_inode sinode;
	struct scoutfs_key_buf key;
	SCOUTFS_DECLARE_KVEC(val);
	LIST_HEAD(ind_locks);
	bool release = false;
	umode_t mode;
	u64 ind_seq;
	int ret;

	ret = scoutfs_lock_ino(sb, DLM_LOCK_EX, 0, ino, &lock);
	if (ret)
		return ret;

	scoutfs_inode_init_key(&key, &ikey, ino);
	scoutfs_kvec_init(val, &sinode, sizeof(sinode));

	ret = scoutfs_item_lookup_exact(sb, &key, val, sizeof(sinode), lock);
	if (ret < 0) {
		if (ret == -ENOENT)
			ret = 0;
		goto out;
	}

	/* XXX corruption, inode probably won't be freed without repair */
	if (le32_to_cpu(sinode.nlink)) {
		scoutfs_warn(sb, "Dangling orphan item for inode %llu.", ino);
		ret = -EIO;
		goto out;
	}

	mode = le32_to_cpu(sinode.mode);
	trace_scoutfs_delete_inode(sb, ino, mode);

	/* XXX the trans reservation count is obviously bonkers :) */
retry:
	ret = scoutfs_inode_index_start(sb, &ind_seq) ?:
	      prepare_index_deletion(sb, &ind_locks, ino, mode, &sinode) ?:
	      scoutfs_inode_index_try_lock_hold(sb, &ind_locks, ind_seq,
						SIC_DIRTY_INODE());
	if (ret > 0)
		goto retry;
	if (ret)
		goto out;

	release = true;

	/* first remove index items to try to avoid indexing partial deletion */
	ret = remove_index_items(sb, ino, &sinode, &ind_locks);
	if (ret)
		goto out;

#if 0
	ret = scoutfs_xattr_drop(sb, ino);
	if (ret)
		goto out;

	if (S_ISLNK(mode))
		ret = scoutfs_symlink_drop(sb, ino, i_size);
	else if (S_ISREG(mode))
		ret = scoutfs_truncate_extent_items(sb, ino, 0, ~0ULL, false);
	if (ret)
		goto out;

#endif
	ret = scoutfs_item_delete(sb, &key, lock);
	if (ret)
		goto out;

	ret = remove_orphan_item(sb, ino);
out:
	if (release)
		scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);
	scoutfs_unlock(sb, lock, DLM_LOCK_EX);
	return ret;
}

/*
 * iput_final has already written out the dirty pages to the inode
 * before we get here.  We're left with a clean inode that we have to
 * tear down.  If there are no more links to the inode then we also
 * remove all its persistent structures.
 */
void scoutfs_evict_inode(struct inode *inode)
{
	trace_scoutfs_evict_inode(inode->i_sb, scoutfs_ino(inode),
				  inode->i_nlink, is_bad_inode(inode));

	if (is_bad_inode(inode))
		goto clear;

	truncate_inode_pages_final(&inode->i_data);

	if (inode->i_nlink == 0)
		delete_inode_items(inode->i_sb, scoutfs_ino(inode));
clear:
	clear_inode(inode);
}

int scoutfs_drop_inode(struct inode *inode)
{
	int ret = generic_drop_inode(inode);

	trace_scoutfs_drop_inode(inode->i_sb, scoutfs_ino(inode),
				 inode->i_nlink, inode_unhashed(inode));
	return ret;
}

/*
 * Find orphan items and process each one.
 *
 * Runtime of this will be bounded by the number of orphans, which could
 * theoretically be very large. If that becomes a problem we might want to push
 * this work off to a thread.
 *
 * This only scans orphans for this node.  This will need to be covered by
 * the rest of node zone cleanup.
 */
int scoutfs_scan_orphans(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_lock *lock = sbi->node_id_lock;
	struct scoutfs_orphan_key okey;
	struct scoutfs_orphan_key last_okey;
	struct scoutfs_key_buf key;
	struct scoutfs_key_buf last;
	int err = 0;
	int ret;

	trace_scoutfs_scan_orphans(sb);

	init_orphan_key(&key, &okey, sbi->node_id, 0);
	init_orphan_key(&last, &last_okey, sbi->node_id, ~0ULL);

	while (1) {
		ret = scoutfs_item_next_same(sb, &key, &last, NULL, lock);
		if (ret == -ENOENT) /* No more orphan items */
			break;
		if (ret < 0)
			goto out;

		ret = delete_inode_items(sb, be64_to_cpu(okey.ino));
		if (ret && ret != -ENOENT && !err)
			err = ret;

		scoutfs_key_inc_cur_len(&key);
	}

	ret = 0;
out:
	return err ? err : ret;
}

int scoutfs_orphan_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_lock *lock = sbi->node_id_lock;
	struct scoutfs_orphan_key okey;
	struct scoutfs_key_buf key;
	int ret;

	trace_scoutfs_orphan_inode(sb, inode);

	init_orphan_key(&key, &okey, sbi->node_id, scoutfs_ino(inode));

	ret = scoutfs_item_create(sb, &key, NULL, lock);

	return ret;
}

/*
 * Track an inode that could have dirty pages.  Used to kick off writeback
 * on all dirty pages during transaction commit without tying ourselves in
 * knots trying to call through the high level vfs sync methods.
 */
void scoutfs_inode_queue_writeback(struct inode *inode)
{
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	spin_lock(&inf->writeback_lock);
	if (RB_EMPTY_NODE(&si->writeback_node))
		insert_writeback_inode(inf, si);
	spin_unlock(&inf->writeback_lock);
}

/*
 * Walk our dirty inodes in ino order and either start dirty page
 * writeback or wait for writeback to complete.
 *
 * This is called by transaction commiting so other writers are
 * excluded.  We're still very careful to iterate over the tree while it
 * and the inodes could be changing.
 *
 * Because writes are excluded we know that there's no remaining dirty
 * pages once waiting returns successfully.
 *
 * XXX not sure what to do about retrying io errors.
 */
int scoutfs_inode_walk_writeback(struct super_block *sb, bool write)
{
	DECLARE_INODE_SB_INFO(sb, inf);
	struct scoutfs_inode_info *si;
	struct rb_node *node;
	struct inode *inode;
	struct inode *defer_iput = NULL;
	int ret;

	spin_lock(&inf->writeback_lock);

	node = rb_first(&inf->writeback_inodes);
	while (node) {
		si = container_of(node, struct scoutfs_inode_info,
				  writeback_node);
		node = rb_next(node);
		inode = igrab(&si->inode);
		if (!inode)
			continue;

		spin_unlock(&inf->writeback_lock);

		if (defer_iput) {
			iput(defer_iput);
			defer_iput = NULL;
		}

		if (write)
			ret = filemap_fdatawrite(inode->i_mapping);
		else
			ret = filemap_fdatawait(inode->i_mapping);
		trace_scoutfs_inode_walk_writeback(sb, scoutfs_ino(inode),
						   write, ret);
		if (ret) {
			iput(inode);
			goto out;
		}

		spin_lock(&inf->writeback_lock);

		if (WARN_ON_ONCE(RB_EMPTY_NODE(&si->writeback_node)))
			node = rb_first(&inf->writeback_inodes);
		else
			node = rb_next(&si->writeback_node);

		if (!write)
			remove_writeback_inode(inf, si);

		/* avoid iput->destroy lock deadlock */
		defer_iput = inode;
	}

	spin_unlock(&inf->writeback_lock);
out:
	if (defer_iput)
		iput(defer_iput);
	return ret;
}

int scoutfs_inode_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct free_ino_pool *pool;
	struct inode_sb_info *inf;

	inf = kzalloc(sizeof(struct inode_sb_info), GFP_KERNEL);
	if (!inf)
		return -ENOMEM;

	pool = &inf->pool;
	init_waitqueue_head(&pool->waitq);
	spin_lock_init(&pool->lock);

	spin_lock_init(&inf->writeback_lock);
	inf->writeback_inodes = RB_ROOT;

	sbi->inode_sb_info = inf;

	return 0;
}

void scoutfs_inode_destroy(struct super_block *sb)
{
	struct inode_sb_info *inf = SCOUTFS_SB(sb)->inode_sb_info;

	kfree(inf);
}

void scoutfs_inode_exit(void)
{
	if (scoutfs_inode_cachep) {
		rcu_barrier();
		kmem_cache_destroy(scoutfs_inode_cachep);
		scoutfs_inode_cachep = NULL;
	}
}

int scoutfs_inode_init(void)
{
	scoutfs_inode_cachep = kmem_cache_create("scoutfs_inode_info",
					sizeof(struct scoutfs_inode_info), 0,
					SLAB_RECLAIM_ACCOUNT,
					scoutfs_inode_ctor);
	if (!scoutfs_inode_cachep)
		return -ENOMEM;

	return 0;
}
