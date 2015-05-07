/* @file
 * @brief  DVFS interface implementation
 * @author Denis Deryugin
 * @date   11 Mar 2014
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <embox/block_dev.h>
#include <fs/dvfs.h>
#include <fs/hlpr_path.h>
#include <kernel/task/resource/file_table.h>

/* Utility functions */
extern int inode_fill(struct super_block *, struct inode *, struct dentry *);
extern int dentry_fill(struct super_block *, struct inode *,
                       struct dentry *, struct dentry *);
extern int            dvfs_update_root(void);
extern struct dentry *dvfs_root(void);

/* Default handlers */
extern int           dvfs_default_pathname(struct inode *inode, char *buf);

/* Path-related functions */

/* @brief Get the full path to the inode from task's root dentry
 * @param inode The inode of which the path is to be resolved
 * @param buf   Char buffer where path would be put
 *
 * @retval  0 Success
 * @retval -1 Error
 */
int dvfs_pathname(struct inode *inode, char *buf) {
	assert(inode);
	assert(buf);

	if (inode->i_ops && inode->i_ops->pathname)
		return inode->i_ops->pathname(inode, buf);
	else
		return dvfs_default_pathname(inode, buf);
}

/* @brief Get the length of next element int the path
 * @param path Pointer to the path
 *
 * @return The length of next element in the path
 * @revtal -1 Error
 */
int dvfs_path_next_len(const char *path) {
	int len = strlen(path);
	int off = 0;

	while (path[off] == '/')
		off++;

	while ((path[off] != '/') && (off < len))
		off++;

	return off;
}

/* @brief Resolve one more element in the path
 * @param path   Pointer to relative path
 * @param dentry The previous dentry
 * @param lookup Structure which is to contain result of path walk
 *
 * @return Negative error code
 * @retval       0 Ok
 * @retval -ENOENT Node not found
 */
int dvfs_path_walk(const char *path, struct dentry *parent, struct lookup *lookup) {
	char buff[DENTRY_NAME_LEN];
	struct inode *in;
	int len = dvfs_path_next_len(path);
	struct dentry *d;
	struct dlist_head *l;
	assert(parent);
	assert(path);

	while (*path == '/')
		path++;

	memcpy(buff, path, len);
	buff[len] = '\0';

	if (buff[0] == '\0') {
		*lookup = (struct lookup) {
			.item   = parent,
			.parent = parent->parent,
		};
		return 0;
	}

	if (strlen(buff) > 1 && path_is_double_dot(buff))
		return dvfs_path_walk(path + 2, parent->parent, lookup);

	if (strlen(buff) > 1 && path_is_single_dot(buff))
		return dvfs_path_walk(path + 2, parent, lookup);

	/* TODO use cache instead */
	dlist_foreach(l, &parent->children) {
		if (l == &parent->children)
			continue;
		d = mcast_out(l, struct dentry, children_lnk);

		if (!strcmp(d->name, buff))
			return dvfs_path_walk(path + strlen(buff), d, lookup);
	}

	assert(parent->d_sb);
	assert(parent->d_sb->sb_iops);
	assert(parent->d_sb->sb_iops->lookup);

	if (!(in = parent->d_sb->sb_iops->lookup(buff, parent))) {
		*lookup = (struct lookup) {
			.item   = NULL,
			.parent = parent,
		};
		return -ENOENT;
	} else {
		struct dentry *d;
		d = dvfs_alloc_dentry();
		in->i_dentry = parent;
		dentry_fill(parent->d_sb, in, d, parent);
		strcpy(d->name, buff);
		d->flags = in->flags;
	}

	return dvfs_path_walk(path + strlen(buff), in->i_dentry, lookup);
}

/* DVFS interface */

/* @brief Try to find dentry at specified path
 * @param path   Absolute or relative path
 * @param lookup Structure where result will be stored
 *
 * @return Negative error code
 * @retval       0 Ok
 * @retval -ENOENT No node found or incorrect root/pwd dentry
 */
int dvfs_lookup(const char *path, struct lookup *lookup) {
	struct dentry *dentry;
	if (path[0] == '/') {
		dentry = task_fs()->root;
		path++;
	} else
		dentry = task_fs()->pwd;

	if (dentry->d_sb == NULL)
		return -ENOENT;

	/* TODO look in dcache */
	/* TODO flocks */

	return dvfs_path_walk(path, dentry, lookup);
}

/* @brief Create new inode
 * @param name   Directory name for new inode
 * @param lookup Structure containing parent dentry; lookup->item should be NULL
 * @param flags  Flags passed to FS driver
 *
 * @return Negative error number
 * @retval       0 Ok
 * @retval -ENOMEM New dentry can't be allocated
 */
int dvfs_create_new(const char *name, struct lookup *lookup, int flags) {
	struct super_block *sb;
	struct inode *new_inode;
	int res;
	assert(lookup);
	assert(lookup->parent);
	assert(lookup->parent->flags & O_DIRECTORY);

	sb = lookup->parent->d_sb;
	lookup->item = dvfs_alloc_dentry();
	if (!lookup->item)
		return -ENOMEM;

	new_inode = dvfs_alloc_inode(sb);
	dentry_fill(sb, new_inode, lookup->item, lookup->parent);
	strncpy(lookup->item->name, name, DENTRY_NAME_LEN);
	inode_fill(sb, new_inode, lookup->item);
	res = sb->sb_iops->create(new_inode, lookup->parent->d_inode, flags);

	if (res) {
		dvfs_destroy_dentry(lookup->item);
	}

	return res;
}

/* @brief Initialize file descriptor for usage according to path
 * @param path Path to the file
 * @param desc The file descriptor to be initailized
 * @param mode Defines behavior according to POSIX
 *
 * @returns Negative error number
 * @retval       0 Ok
 * @retval -ENOENT File is directory or file not found and
 *                 creating is not requested
 */
int dvfs_open(const char *path, struct file *desc, int mode) {
	struct lookup lookup;
	struct inode  *i_no;

	dvfs_lookup(path, &lookup);

	assert(desc);

	if (!lookup.item) {
		if (mode & O_CREAT) {
			char *last_name = strrchr(path, '/');
			if (dvfs_create_new(last_name ? last_name + 1 : path, &lookup, mode))
				return -ENOSPC;
		} else {
			desc->f_inode = NULL;
			desc->f_dentry = NULL;
			return -ENOENT;
		}
	}

	i_no = lookup.item->d_inode;

	*desc = (struct file) {
		.f_dentry = lookup.item,
		.f_inode  = i_no,
		.f_ops    = lookup.item->d_sb->sb_fops,
	};

	if (i_no == NULL || i_no->flags & O_DIRECTORY) {
		if (lookup.item)
			dvfs_destroy_dentry(lookup.item);
		return -ENOENT;
	}

	assert(desc->f_ops);
	assert(desc->f_ops->open);

	lookup.item->usage_count++;

	return desc->f_ops->open(i_no, desc);
}

/* @brief Delete file from storage
 * @param path Path to file
 *
 * @return Negative error code
 * @retval  0 Ok
 * @retval -1 File not found
 */
int dvfs_remove(const char *path) {
	struct lookup lookup;
	struct inode  *i_no;
	int res;

	dvfs_lookup(path, &lookup);

	if (!lookup.item) {
		return -ENOENT;
	}

	i_no = lookup.item->d_inode;

	assert(i_no->i_ops);

	if (!i_no->i_ops->remove)
		return -EPERM;

	res = i_no->i_ops->remove(i_no);

	if (res == 0)
		dvfs_destroy_dentry(lookup.item);

	return res;
}

/* @brief Uninitialize file descriptor
 * @param desc File descriptor to be uninitialized
 *
 * @return Negative error code
 * @retval  0 Ok
 * @retval -1 Descriptor fields are inconsistent
 */
int dvfs_close(struct file *desc) {
	if (!desc || !desc->f_inode || !desc->f_dentry)
		return -1;

	desc->f_dentry->usage_count--;
	if (!desc->f_dentry->usage_count) {
		dvfs_destroy_dentry(desc->f_dentry);
	}

	dvfs_destroy_file(desc);
	return 0;
}

/* @brief Application level interface to write the file
 * @param desc  File to be written
 * @param buf   Source of the data
 * @param count Length of the data
 *
 * @return Bytes written or negative error code
 * @retval       0 Ok
 * @retval -ENOSYS Function is not implemented in file system driver
 */
int dvfs_write(struct file *desc, char *buf, int count) {
	int res;
	if (!desc)
		return -1;

	if (desc->f_ops && desc->f_ops->write)
		res = desc->f_ops->write(desc, buf, count);
	else
		return -ENOSYS;

	if (res > 0)
		desc->pos += res;

	return res;
}

/* @brief Application level interface to read the file
 * @param desc  File to be read
 * @param buf   Destination
 * @param count Length of the data
 *
 * @return Bytes read or negative error code
 * @retval       0 Ok
 * @retval -ENOSYS Function is not implemented in file system driver
 */
int dvfs_read(struct file *desc, char *buf, int count) {
	int res;
	if (!desc)
		return -1;

	if (desc->f_ops && desc->f_ops->read)
		res = desc->f_ops->read(desc, buf, count);
	else
		return -ENOSYS;

	if (res > 0)
		desc->pos += res;

	return res;
}

extern int set_rootfs_sb(struct super_block *sb);
/* @brief Mount file system
 * @param dev    Path to the source device (e.g. /dev/sda1)
 * @param dest   Path to the mount point (e.g. /mnt)
 * @param fstype File system type related to FS driver
 * @param flags  NIY
 *
 * @return Negative error value
 * @retval       0 Ok
 * @retval -ENOENT Mount point or device not found
 */
int dvfs_mount(struct block_dev *dev, char *dest, char *fstype, int flags) {
	struct lookup lookup;
	struct dumb_fs_driver *drv;
	struct super_block *sb;
	struct dentry *d;

	drv = dumb_fs_driver_find(fstype);
	sb  = dvfs_alloc_sb(drv, dev);

	if (!strcmp(dest, "/")) {
		set_rootfs_sb(sb);
		dvfs_update_root();
	} else {
		dvfs_lookup(dest, &lookup);

		if (lookup.item == NULL)
			return -ENOENT;

		assert(lookup.item->flags & O_DIRECTORY);

		/* Hide dentry of the directory */
		dlist_del(&lookup.item->children_lnk);

		d = dvfs_alloc_dentry();
		dentry_fill(sb, NULL, d, lookup.parent);
		d->usage_count++;
		sb->root = d;
		d->flags = O_DIRECTORY;
		strcpy(d->name, lookup.item->name);

		d->d_inode = dvfs_alloc_inode(sb);
		*d->d_inode = (struct inode) {
			.flags    = O_DIRECTORY,
			.i_ops   = sb->sb_iops,
			.i_sb     = sb,
			.i_dentry = d,
		};
	}

	if (drv->mount_end)
		drv->mount_end(sb);

	return 0;
}

/* @brief Get next entry in the directory
 * @param lookup  Contains directory dentry (.parent) and
 *                previous element (.item)
 * @param dir_ctx Position to be found in directory
 *
 * @return Negative error value
 * @retval 0 Ok
 */
int dvfs_iterate(struct lookup *lookup, struct dir_ctx *ctx) {
	struct super_block *sb;
	struct inode *parent_inode;
	struct inode *next_inode;
	struct dentry *next_dentry = NULL;
	int res;
	assert(lookup);
	assert(ctx);

	sb = lookup->parent->d_sb;
	parent_inode = lookup->parent->d_inode;
	next_inode   = dvfs_alloc_inode(sb);
	next_dentry  = dvfs_alloc_dentry();

	if (!next_inode || !next_dentry) {
		if (next_dentry)
			dvfs_destroy_dentry(next_dentry);
		if (next_inode)
			dvfs_destroy_inode(next_inode);
		return -ENOMEM;
	}

	dentry_fill(sb, next_inode, next_dentry, lookup->parent);
	assert(sb && sb->sb_iops && sb->sb_iops->iterate);
	res = sb->sb_iops->iterate(next_inode, parent_inode, ctx);
	inode_fill(sb, next_inode, next_dentry);

	if (res) {
		ctx->pos = 0;
		dvfs_destroy_dentry(next_dentry);
		next_dentry = NULL;
	} else {
		dvfs_pathname(next_inode, next_dentry->name);
		ctx->pos++;
	}

	lookup->item = next_dentry;

	return 0;
}

