/*
 * Copyright (C) 2017 Versity Software, Inc.  All rights reserved.
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
#include <linux/slab.h>
#include <linux/fs.h>

#include "super.h"
#include "sysfs.h"

static struct kset *scoutfs_kset;

struct sysfs_info {
	struct super_block *sb;
	struct kobject sb_id_kobj;
	struct completion sb_id_comp;
};

#define KOBJ_TO_SB(kobj, which) \
	container_of(kobj, struct sysfs_info, which)->sb

struct attr_funcs {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
			char *buf);
};

#define ATTR_FUNCS_RO(_name) \
	static struct attr_funcs _name##_attr_funcs = __ATTR_RO(_name)

static ssize_t fsid_show(struct kobject *kobj, struct attribute *attr,
			 char *buf)
{
	struct super_block *sb = KOBJ_TO_SB(kobj, sb_id_kobj);
	struct scoutfs_super_block *super = &SCOUTFS_SB(sb)->super;

	return snprintf(buf, PAGE_SIZE, "%llx\n", le64_to_cpu(super->hdr.fsid));
}
ATTR_FUNCS_RO(fsid);

/*
 * ops are defined per type, not per attribute.  To have attributes with
 * different types that want different funcs we wrap them with a struct
 * that has per-type funcs.
 */
static ssize_t attr_funcs_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct attr_funcs *af = container_of(attr, struct attr_funcs, attr);

	return af->show(kobj, attr, buf);
}

#define KTYPE(_name)							\
	static void _name##_release(struct kobject *kobj)		\
	{								\
		struct sysfs_info *sfsinfo;				\
									\
		sfsinfo = container_of(kobj, struct sysfs_info, _name##_kobj);\
									\
		complete(&sfsinfo->_name##_comp);			\
	}								\
	static const struct sysfs_ops _name##_sysfs_ops = {		\
		.show   = attr_funcs_show,				\
	};								\
									\
	static struct kobj_type _name##_ktype = {			\
		.default_attrs  = _name##_attrs,			\
		.sysfs_ops      = &_name##_sysfs_ops,			\
		.release        = _name##_release,			\
	};


static struct attribute *sb_id_attrs[] = {
	&fsid_attr_funcs.attr,
	NULL,
};
KTYPE(sb_id);

struct kobject *scoutfs_sysfs_sb_dir(struct super_block *sb)
{
	struct sysfs_info *sfsinfo = SCOUTFS_SB(sb)->sfsinfo;

	return &sfsinfo->sb_id_kobj;
}

static void kobj_del_put_wait(struct kobject *kobj, struct completion *comp)
{
	kobject_del(kobj);
	kobject_put(kobj);
	wait_for_completion(comp);
}

#define shutdown_kobj(sfinfo, _name) \
	kobj_del_put_wait(&sfsinfo->_name##_kobj, &sfsinfo->_name##_comp)

/*
 * Only the return from kobj_init_and_add() tells us if the kobj needs
 * to be cleaned up or not.  This must manually clean up the kobjs and
 * only leave full cleanup to _destroy_.
 */
int scoutfs_setup_sysfs(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct sysfs_info *sfsinfo;
	int ret;

	sfsinfo = kzalloc(sizeof(struct sysfs_info), GFP_KERNEL);
	if (!sfsinfo)
		return -ENOMEM;

	sfsinfo->sb = sb;
	sbi->sfsinfo = sfsinfo;

	/* XXX can have multiple mounts of a  device, need mount id */
	init_completion(&sfsinfo->sb_id_comp);
	ret = kobject_init_and_add(&sfsinfo->sb_id_kobj, &sb_id_ktype,
				   &scoutfs_kset->kobj, "%s", sb->s_id);
	if (ret)
		kfree(sfsinfo);

	return ret;
}

void scoutfs_destroy_sysfs(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct sysfs_info *sfsinfo = sbi->sfsinfo;

	if (sfsinfo) {
		shutdown_kobj(sfsinfo, sb_id);

		kfree(sfsinfo);
		sbi->sfsinfo = NULL;
	}
}

int __init scoutfs_sysfs_init(void)
{
	scoutfs_kset = kset_create_and_add("scoutfs", NULL, fs_kobj);
	if (!scoutfs_kset)
		return -ENOMEM;

	return 0;
}

void __exit scoutfs_sysfs_exit(void)
{
	if (scoutfs_kset)
		kset_unregister(scoutfs_kset);
}
