// SPDX-License-Identifier: MIT
/*
 * VirtualBox Guest Shared Folders support: Directory inode and file operations
 *
 * Copyright (C) 2006-2018 Oracle Corporation
 */

#include <linux/namei.h>
#include <linux/vbox_utils.h>
#include "vfsmod.h"

/**
 * Open a directory. Read the complete content into a buffer.
 * Return: 0 or negative errno value.
 * @inode	inode
 * @file	file
 */
static int sf_dir_open(struct inode *inode, struct file *file)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(inode->i_sb);
	struct shfl_createparms params = {};
	struct sf_dir_info *sf_d;
	int err;

	sf_d = vboxsf_dir_info_alloc();
	if (!sf_d)
		return -ENOMEM;

	params.handle = SHFL_HANDLE_NIL;
	params.create_flags = 0
	    | SHFL_CF_DIRECTORY
	    | SHFL_CF_ACT_OPEN_IF_EXISTS
	    | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;

	err = vboxsf_create_at_dentry(file_dentry(file), &params);
	if (err == 0) {
		if (params.result == SHFL_FILE_EXISTS) {
			err = vboxsf_dir_read_all(sf_g, sf_d, params.handle);
			if (!err)
				file->private_data = sf_d;
		} else
			err = -ENOENT;

		vboxsf_close(sf_g->root, params.handle);
	}

	if (err)
		vboxsf_dir_info_free(sf_d);

	return err;
}

/**
 * This is called when reference count of [file] goes to zero. Notify
 * the host that it can free whatever is associated with this directory
 * and deallocate our own internal buffers
 * Return: 0 or negative errno value.
 * @inode	inode
 * @file	file
 */
static int sf_dir_release(struct inode *inode, struct file *file)
{
	if (file->private_data)
		vboxsf_dir_info_free(file->private_data);

	return 0;
}

/**
 * Translate RTFMODE into DT_xxx (in conjunction to rtDirType())
 * Return: d_type
 * @mode	file mode
 */
static int sf_get_d_type(u32 mode)
{
	int d_type;

	switch (mode & SHFL_TYPE_MASK) {
	case SHFL_TYPE_FIFO:
		d_type = DT_FIFO;
		break;
	case SHFL_TYPE_DEV_CHAR:
		d_type = DT_CHR;
		break;
	case SHFL_TYPE_DIRECTORY:
		d_type = DT_DIR;
		break;
	case SHFL_TYPE_DEV_BLOCK:
		d_type = DT_BLK;
		break;
	case SHFL_TYPE_FILE:
		d_type = DT_REG;
		break;
	case SHFL_TYPE_SYMLINK:
		d_type = DT_LNK;
		break;
	case SHFL_TYPE_SOCKET:
		d_type = DT_SOCK;
		break;
	case SHFL_TYPE_WHITEOUT:
		d_type = DT_WHT;
		break;
	default:
		d_type = DT_UNKNOWN;
		break;
	}
	return d_type;
}

/**
 * Extract element ([dir]->f_pos) from the directory [dir] into [d_name].
 * Return: 0 or negative errno value.
 * @dir		Directory to get element at f_pos from
 * @d_name	Buffer in which to return element name
 * @d_type	Buffer in which to return element file-type
 */
static int sf_getdent(struct file *dir, loff_t pos,
		      char d_name[NAME_MAX], int *d_type)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(file_inode(dir)->i_sb);
	struct sf_dir_info *sf_d = dir->private_data;
	struct shfl_dirinfo *info;
	struct sf_dir_buf *b;
	loff_t i, cur = 0;

	list_for_each_entry(b, &sf_d->info_list, head) {
		if (pos >= cur + b->entries) {
			cur += b->entries;
			continue;
		}

		/*
		 * Note the sf_dir_info objects we are iterating over here
		 * are variable sized, so the info pointer may end up being
		 * unaligned. This is how we get the data from the host.
		 * Since vboxsf is only supported on x86 machines this is not
		 * a problem.
		 */
		for (i = 0, info = b->buf; i < pos - cur; ++i) {
			size_t size;

			size = offsetof(struct shfl_dirinfo, name.string) +
			       info->name.size;
			info = (struct shfl_dirinfo *)((uintptr_t) info + size);
		}

		*d_type = sf_get_d_type(info->info.attr.mode);

		return vboxsf_nlscpy(sf_g, d_name, NAME_MAX,
				     info->name.string.utf8, info->name.length);
	}

	return 1;
}

/**
 * This is called when vfs wants to populate internal buffers with
 * directory [dir]s contents. [opaque] is an argument to the
 * [filldir]. [filldir] magically modifies it's argument - [opaque]
 * and takes following additional arguments (which i in turn get from
 * the host via sf_getdent):
 *
 * name : name of the entry (i must also supply it's length huh?)
 * type : type of the entry (FILE | DIR | etc) (i ellect to use DT_UNKNOWN)
 * pos : position/index of the entry
 * ino : inode number of the entry (i fake those)
 *
 * [dir] contains:
 * f_pos : cursor into the directory listing
 * private_data : mean of communication with the host side
 *
 * Extract elements from the directory listing (incrementing f_pos
 * along the way) and feed them to [filldir] until:
 *
 * a. there are no more entries (i.e. sf_getdent set done to 1)
 * b. failure to compute fake inode number
 * c. filldir returns an error (see comment on that)
 * Return: 0 or negative errno value.
 * @dir		Directory to read
 * @ctx		Directory context in which to store read elements
 */
static int sf_dir_iterate(struct file *dir, struct dir_context *ctx)
{
	for (;;) {
		int err;
		ino_t fake_ino;
		loff_t sanity;
		char d_name[NAME_MAX];
		int d_type = DT_UNKNOWN;

		err = sf_getdent(dir, ctx->pos, d_name, &d_type);
		if (unlikely(err)) { /* EOF or an error */
			if (err == 1)
				return 0;
			/* skip erroneous entry and proceed */
			ctx->pos += 1;
			continue;
		}

		/* d_name now contains a valid entry name */
		sanity = ctx->pos + 0xbeef;
		fake_ino = sanity;
		/*
		 * On 32 bit systems pos is 64 signed, while ino is 32 bit
		 * unsigned so fake_ino may overflow, check for this.
		 */
		if (sanity - fake_ino) {
			vbg_err("vboxsf: can not compute ino\n");
			return -EINVAL;
		}
		if (!dir_emit(ctx, d_name, strlen(d_name), fake_ino, d_type))
			return 0;

		ctx->pos += 1;
	}
}

const struct file_operations vboxsf_dir_fops = {
	.open = sf_dir_open,
	.iterate = sf_dir_iterate,
	.release = sf_dir_release,
	.read = generic_read_dir,
	.llseek = generic_file_llseek,
};

/*
 * This is called during name resolution/lookup to check if the [dentry] in
 * the cache is still valid. the job is handled by [sf_inode_revalidate].
 */
static int sf_dentry_revalidate(struct dentry *dentry, unsigned int flags)
{
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	if (d_really_is_positive(dentry))
		return vboxsf_inode_revalidate(dentry) == 0;
	else
		return vboxsf_stat_dentry(dentry, NULL) == -ENOENT;
}

const struct dentry_operations vboxsf_dentry_ops = {
	.d_revalidate = sf_dentry_revalidate
};

/* iops */

/**
 * This is called when vfs failed to locate dentry in the cache. The
 * job of this function is to allocate inode and link it to dentry.
 * [dentry] contains the name to be looked in the [parent] directory.
 * Failure to locate the name is not a "hard" error, in this case NULL
 * inode is added to [dentry] and vfs should proceed trying to create
 * the entry via other means.
 * Return: NULL on success, ERR_PTR on failure.
 * @parent	inode of the dentry parent-directory
 * @dentry	dentry to populate
 * @flags	flags
 */
static struct dentry *sf_lookup(struct inode *parent, struct dentry *dentry,
				unsigned int flags)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	struct shfl_fsobjinfo fsinfo;
	struct inode *inode;
	int err;

	dentry->d_time = jiffies;

	err = vboxsf_stat_dentry(dentry, &fsinfo);
	if (err) {
		inode = (err == -ENOENT) ? NULL : ERR_PTR(err);
	} else {
		inode = vboxsf_new_inode(parent->i_sb);
		if (!IS_ERR(inode))
			vboxsf_init_inode(sf_g, inode, &fsinfo);
	}

	return d_splice_alias(inode, dentry);
}

/**
 * This should allocate memory for sf_inode_info, compute a unique inode
 * number, get an inode from vfs, initialize inode info, instantiate
 * dentry.
 * Return: 0 or negative errno value.
 * @parent	inode entry of the directory
 * @dentry	directory cache entry
 * @info	file information
 */
static int sf_instantiate(struct inode *parent, struct dentry *dentry,
			  struct shfl_fsobjinfo *info)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	struct sf_inode_info *sf_i;
	struct inode *inode;

	inode = vboxsf_new_inode(parent->i_sb);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	sf_i = GET_INODE_INFO(inode);
	/* the host may have given us different attr then requested */
	sf_i->force_restat = 1;
	vboxsf_init_inode(sf_g, inode, info);

	d_instantiate(dentry, inode);

	return 0;
}

/**
 * Create a new regular file / directory.
 * Return: 0 or negative errno value.
 * @parent	inode of the directory
 * @dentry	directory cache entry
 * @mode	file mode
 * @is_dir	true if directory, false otherwise
 */
static int sf_create_aux(struct inode *parent, struct dentry *dentry,
			 umode_t mode, int is_dir)
{
	struct sf_inode_info *sf_parent_i = GET_INODE_INFO(parent);
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	struct shfl_createparms params = {};
	int err;

	params.handle = SHFL_HANDLE_NIL;
	params.create_flags = 0
	    | SHFL_CF_ACT_CREATE_IF_NEW
	    | SHFL_CF_ACT_FAIL_IF_EXISTS
	    | SHFL_CF_ACCESS_READWRITE | (is_dir ? SHFL_CF_DIRECTORY : 0);
	params.info.attr.mode = 0
	    | (is_dir ? SHFL_TYPE_DIRECTORY : SHFL_TYPE_FILE)
	    | (mode & 0777);
	params.info.attr.additional = SHFLFSOBJATTRADD_NOTHING;

	err = vboxsf_create_at_dentry(dentry, &params);
	if (err)
		return err;

	if (params.result != SHFL_FILE_CREATED)
		return -EPERM;

	vboxsf_close(sf_g->root, params.handle);

	err = sf_instantiate(parent, dentry, &params.info);
	if (err)
		return err;

	/* parent directory access/change time changed */
	sf_parent_i->force_restat = 1;

	return 0;
}

/**
 * Create a new regular file.
 * Return: 0 or negative errno value.
 * @parent	inode of the directory
 * @dentry	directory cache entry
 * @mode	file mode
 * @excl	Possible O_EXCL...
 */
static int sf_create(struct inode *parent, struct dentry *dentry, umode_t mode,
		     bool excl)
{
	return sf_create_aux(parent, dentry, mode, 0);
}

/**
 * Create a new directory.
 * Return: 0 or negative errno value.
 * @parent	inode of the directory
 * @dentry	directory cache entry
 * @mode	file mode
 */
static int sf_mkdir(struct inode *parent, struct dentry *dentry, umode_t mode)
{
	return sf_create_aux(parent, dentry, mode, 1);
}

/**
 * Remove a regular file / directory.
 * Return: 0 or negative errno value.
 * @parent	inode of the directory
 * @dentry	directory cache entry
 * @is_dir	true if directory, false otherwise
 */
static int sf_unlink_aux(struct inode *parent, struct dentry *dentry,
			 int is_dir)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	struct sf_inode_info *sf_parent_i = GET_INODE_INFO(parent);
	struct inode *inode = d_inode(dentry);
	struct shfl_string *path;
	uint32_t flags;
	int err;

	flags = is_dir ? SHFL_REMOVE_DIR : SHFL_REMOVE_FILE;
	if ((inode->i_mode & S_IFLNK) == S_IFLNK)
		flags |= SHFL_REMOVE_SYMLINK;

	path = vboxsf_path_from_dentry(sf_g, dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);

	err = vboxsf_remove(sf_g->root, path, flags);
	__putname(path);
	if (err)
		return err;

	/* parent directory access/change time changed */
	sf_parent_i->force_restat = 1;

	return 0;
}

/**
 * Remove a regular file.
 * Return: 0 or negative errno value.
 * @parent	inode of the directory
 * @dentry	directory cache entry
 */
static int sf_unlink(struct inode *parent, struct dentry *dentry)
{
	return sf_unlink_aux(parent, dentry, 0);
}

/**
 * Remove a directory.
 * Return: 0 or negative errno value.
 * @parent	inode of the directory
 * @dentry	directory cache entry
 */
static int sf_rmdir(struct inode *parent, struct dentry *dentry)
{
	return sf_unlink_aux(parent, dentry, 1);
}

/**
 * Rename a regular file / directory.
 * Return: 0 or negative errno value.
 * @old_parent	inode of the old parent directory
 * @old_dentry	old directory cache entry
 * @new_parent	inode of the new parent directory
 * @new_dentry	new directory cache entry
 * @flags	flags
 */
static int sf_rename(struct inode *old_parent, struct dentry *old_dentry,
		     struct inode *new_parent, struct dentry *new_dentry,
		     unsigned int flags)
{
	struct sf_glob_info *sf_g = GET_GLOB_INFO(old_parent->i_sb);
	struct sf_inode_info *sf_old_parent_i = GET_INODE_INFO(old_parent);
	struct sf_inode_info *sf_new_parent_i = GET_INODE_INFO(new_parent);
	u32 shfl_flags = SHFL_RENAME_FILE | SHFL_RENAME_REPLACE_IF_EXISTS;
	struct shfl_string *old_path, *new_path;
	int err;

	if (flags)
		return -EINVAL;

	if (sf_g != GET_GLOB_INFO(new_parent->i_sb))
		return -EINVAL;

	old_path = vboxsf_path_from_dentry(sf_g, old_dentry);
	if (IS_ERR(old_path))
		return PTR_ERR(old_path);

	new_path = vboxsf_path_from_dentry(sf_g, new_dentry);
	if (IS_ERR(new_path)) {
		__putname(old_path);
		return PTR_ERR(new_path);
	}

	if (d_inode(old_dentry)->i_mode & S_IFDIR)
		shfl_flags = 0;

	err = vboxsf_rename(sf_g->root, old_path, new_path, shfl_flags);
	if (err == 0) {
		/* parent directories access/change time changed */
		sf_new_parent_i->force_restat = 1;
		sf_old_parent_i->force_restat = 1;
	}

	__putname(new_path);
	__putname(old_path);
	return err;
}

static int sf_symlink(struct inode *parent, struct dentry *dentry,
		      const char *symname)
{
	struct sf_inode_info *sf_parent_i = GET_INODE_INFO(parent);
	struct sf_glob_info *sf_g = GET_GLOB_INFO(parent->i_sb);
	int symname_size = strlen(symname) + 1;
	struct shfl_string *path, *ssymname;
	struct shfl_fsobjinfo info;
	int err;

	path = vboxsf_path_from_dentry(sf_g, dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);

	ssymname = kmalloc(SHFLSTRING_HEADER_SIZE + symname_size, GFP_KERNEL);
	if (!ssymname) {
		__putname(path);
		return -ENOMEM;
	}
	ssymname->length = symname_size - 1;
	ssymname->size = symname_size;
	memcpy(ssymname->string.utf8, symname, symname_size);

	err = vboxsf_symlink(sf_g->root, path, ssymname, &info);
	kfree(ssymname);
	__putname(path);
	if (err) {
		/* -EROFS means symlinks are note support -> -EPERM */
		return (err == -EROFS) ? -EPERM : err;
	}

	err = sf_instantiate(parent, dentry, &info);
	if (err)
		return err;

	/* parent directory access/change time changed */
	sf_parent_i->force_restat = 1;
	return 0;
}

const struct inode_operations vboxsf_dir_iops = {
	.lookup = sf_lookup,
	.create = sf_create,
	.mkdir = sf_mkdir,
	.rmdir = sf_rmdir,
	.unlink = sf_unlink,
	.rename = sf_rename,
	.getattr = vboxsf_getattr,
	.setattr = vboxsf_setattr,
	.symlink = sf_symlink
};
