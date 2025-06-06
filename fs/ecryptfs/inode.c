/**
 * eCryptfs: Linux filesystem encryption layer
 *
 * Copyright (C) 1997-2004 Erez Zadok
 * Copyright (C) 2001-2004 Stony Brook University
 * Copyright (C) 2004-2007 International Business Machines Corp.
 *   Author(s): Michael A. Halcrow <mahalcro@us.ibm.com>
 *              Michael C. Thompsion <mcthomps@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <linux/file.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/dcache.h>
#include <linux/namei.h>
#include <linux/mount.h>
#include <linux/crypto.h>
#include <linux/fs_stack.h>
#include <linux/slab.h>
#include <linux/xattr.h>
#include <asm/unaligned.h>
#include "ecryptfs_kernel.h"

#ifdef CONFIG_SDP
#include <sdp/fs_request.h>
#include "ecryptfs_sdp_chamber.h"
#include "ecryptfs_dek.h"

#if (ANDROID_VERSION < 80000)
#include "../sdcardfs/sdcardfs.h"
#endif

#endif

#ifdef CONFIG_DLP
#include "ecryptfs_dlp.h"
#endif
/* Do not directly use this function. Use ECRYPTFS_OVERRIDE_CRED() instead. */
const struct cred * ecryptfs_override_fsids(uid_t fsuid, gid_t fsgid)
{
	struct cred * cred; 
	const struct cred * old_cred; 

	cred = prepare_creds(); 
	if (!cred) 
		return NULL; 

	cred->fsuid = make_kuid(current_user_ns(), fsuid);
	cred->fsgid = make_kgid(current_user_ns(), fsgid);

	old_cred = override_creds(cred); 

	return old_cred; 
}

/* Do not directly use this function, use REVERT_CRED() instead. */
void ecryptfs_revert_fsids(const struct cred * old_cred)
{
	const struct cred * cur_cred; 

	cur_cred = current->cred; 
	revert_creds(old_cred); 
	put_cred(cur_cred); 
}

#if !defined(CONFIG_SDP) || (ANDROID_VERSION >= 80000)
static struct dentry *lock_parent(struct dentry *dentry)
{
	struct dentry *dir;

	dir = dget_parent(dentry);
	mutex_lock_nested(&(d_inode(dir)->i_mutex), I_MUTEX_PARENT);
	return dir;
}

static void unlock_dir(struct dentry *dir)
{
	mutex_unlock(&d_inode(dir)->i_mutex);
	dput(dir);
}
#endif

static int ecryptfs_inode_test(struct inode *inode, void *lower_inode)
{
	return ecryptfs_inode_to_lower(inode) == lower_inode;
}

static int ecryptfs_inode_set(struct inode *inode, void *opaque)
{
	struct inode *lower_inode = opaque;

	ecryptfs_set_inode_lower(inode, lower_inode);
	fsstack_copy_attr_all(inode, lower_inode);
	/* i_size will be overwritten for encrypted regular files */
	fsstack_copy_inode_size(inode, lower_inode);
	inode->i_ino = lower_inode->i_ino;
	inode->i_version++;
	inode->i_mapping->a_ops = &ecryptfs_aops;

	if (S_ISLNK(inode->i_mode))
		inode->i_op = &ecryptfs_symlink_iops;
	else if (S_ISDIR(inode->i_mode))
		inode->i_op = &ecryptfs_dir_iops;
	else
		inode->i_op = &ecryptfs_main_iops;

	if (S_ISDIR(inode->i_mode))
		inode->i_fop = &ecryptfs_dir_fops;
	else if (special_file(inode->i_mode))
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
	else
		inode->i_fop = &ecryptfs_main_fops;

	return 0;
}

static struct inode *__ecryptfs_get_inode(struct inode *lower_inode,
					  struct super_block *sb)
{
	struct inode *inode;

	if (lower_inode->i_sb != ecryptfs_superblock_to_lower(sb))
		return ERR_PTR(-EXDEV);
	if (!igrab(lower_inode))
		return ERR_PTR(-ESTALE);
	inode = iget5_locked(sb, (unsigned long)lower_inode,
			     ecryptfs_inode_test, ecryptfs_inode_set,
			     lower_inode);
	if (!inode) {
		iput(lower_inode);
		return ERR_PTR(-EACCES);
	}
	if (!(inode->i_state & I_NEW))
		iput(lower_inode);

	return inode;
}

struct inode *ecryptfs_get_inode(struct inode *lower_inode,
				 struct super_block *sb)
{
	struct inode *inode = __ecryptfs_get_inode(lower_inode, sb);

	if (!IS_ERR(inode) && (inode->i_state & I_NEW))
		unlock_new_inode(inode);

	return inode;
}

/**
 * ecryptfs_interpose
 * @lower_dentry: Existing dentry in the lower filesystem
 * @dentry: ecryptfs' dentry
 * @sb: ecryptfs's super_block
 *
 * Interposes upper and lower dentries.
 *
 * Returns zero on success; non-zero otherwise
 */
static int ecryptfs_interpose(struct dentry *lower_dentry,
			      struct dentry *dentry, struct super_block *sb)
{
	struct inode *inode = ecryptfs_get_inode(d_inode(lower_dentry), sb);

	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
#if (ANDROID_VERSION < 80000)
	if(d_unhashed(dentry))
		d_rehash(dentry);
#endif

#ifdef CONFIG_SDP
	if(S_ISDIR(inode->i_mode) && dentry) {
	    if(IS_UNDER_ROOT(dentry)) {
	        struct ecryptfs_mount_crypt_stat *mount_crypt_stat  =
	                &ecryptfs_superblock_to_private(inode->i_sb)->mount_crypt_stat;
	        int engineid;

	        printk("Creating a directoy under root directory of current partition.\n");

	        if(is_chamber_directory(mount_crypt_stat, dentry->d_name.name, &engineid)) {
	            printk("This is a chamber directory engine[%d]\n", engineid);
	            set_chamber_flag(engineid, inode);
	        }
	    } else if(IS_SENSITIVE_DENTRY(dentry->d_parent)) {
	        /*
	         * When parent directory is sensitive
	         */
	        struct ecryptfs_crypt_stat *crypt_stat =
	                &ecryptfs_inode_to_private(inode)->crypt_stat;
            struct ecryptfs_crypt_stat *parent_crypt_stat =
                    &ecryptfs_inode_to_private(dentry->d_parent->d_inode)->crypt_stat;

            //TODO : remove this log
            DEK_LOGE("Parent %s[id:%d] is sensitive. so this directory is sensitive too\n",
	                dentry->d_parent->d_name.name, parent_crypt_stat->engine_id);
	        crypt_stat->flags |= ECRYPTFS_DEK_IS_SENSITIVE;
            crypt_stat->engine_id = parent_crypt_stat->engine_id;
	    }
	}
#endif

	return 0;
}

static int ecryptfs_do_unlink(struct inode *dir, struct dentry *dentry,
			      struct inode *inode)
{
	struct dentry *lower_dentry = ecryptfs_dentry_to_lower(dentry);
	struct inode *lower_dir_inode = ecryptfs_inode_to_lower(dir);
	struct dentry *lower_dir_dentry;
	int rc;

	dget(lower_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	rc = vfs_unlink(lower_dir_inode, lower_dentry, NULL);
	if (rc) {
		printk(KERN_ERR "Error in vfs_unlink; rc = [%d]\n", rc);
		goto out_unlock;
	}
	fsstack_copy_attr_times(dir, lower_dir_inode);
	set_nlink(inode, ecryptfs_inode_to_lower(inode)->i_nlink);
	inode->i_ctime = dir->i_ctime;
	d_drop(dentry);
out_unlock:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	return rc;
}

/**
 * ecryptfs_do_create
 * @directory_inode: inode of the new file's dentry's parent in ecryptfs
 * @ecryptfs_dentry: New file's dentry in ecryptfs
 * @mode: The mode of the new file
 *
 * Creates the underlying file and the eCryptfs inode which will link to
 * it. It will also update the eCryptfs directory inode to mimic the
 * stat of the lower directory inode.
 *
 * Returns the new eCryptfs inode on success; an ERR_PTR on error condition
 */
static struct inode *
ecryptfs_do_create(struct inode *directory_inode,
		   struct dentry *ecryptfs_dentry, umode_t mode)
{
	int rc;
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	struct inode *inode;

	lower_dentry = ecryptfs_dentry_to_lower(ecryptfs_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	rc = vfs_create(d_inode(lower_dir_dentry), lower_dentry, mode, true);
	if (rc) {
		printk(KERN_ERR "%s: Failure to create dentry in lower fs; "
		       "rc = [%d]\n", __func__, rc);
		inode = ERR_PTR(rc);
		goto out_lock;
	}
	inode = __ecryptfs_get_inode(d_inode(lower_dentry),
				     directory_inode->i_sb);
	if (IS_ERR(inode)) {
		vfs_unlink(d_inode(lower_dir_dentry), lower_dentry, NULL);
		goto out_lock;
	}
	fsstack_copy_attr_times(directory_inode, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(directory_inode, d_inode(lower_dir_dentry));
out_lock:
	unlock_dir(lower_dir_dentry);
	return inode;
}

/**
 * ecryptfs_initialize_file
 *
 * Cause the file to be changed from a basic empty file to an ecryptfs
 * file with a header and first data page.
 *
 * Returns zero on success
 */
int ecryptfs_initialize_file(struct dentry *ecryptfs_dentry,
			     struct inode *ecryptfs_inode)
{
	struct ecryptfs_crypt_stat *crypt_stat =
		&ecryptfs_inode_to_private(ecryptfs_inode)->crypt_stat;
	int rc = 0;

#ifdef CONFIG_DLP
	sdp_fs_command_t *cmd = NULL;
#endif

	if (S_ISDIR(ecryptfs_inode->i_mode)) {
		ecryptfs_printk(KERN_DEBUG, "This is a directory\n");
		crypt_stat->flags &= ~(ECRYPTFS_ENCRYPTED);
		goto out;
	}
	ecryptfs_printk(KERN_DEBUG, "Initializing crypto context\n");
	rc = ecryptfs_new_file_context(ecryptfs_inode);
	if (rc) {
		ecryptfs_printk(KERN_ERR, "Error creating new file "
				"context; rc = [%d]\n", rc);
		goto out;
	}
	rc = ecryptfs_get_lower_file(ecryptfs_dentry, ecryptfs_inode);
	if (rc) {
		printk(KERN_ERR "%s: Error attempting to initialize "
			"the lower file for the dentry with name "
			"[%pd]; rc = [%d]\n", __func__,
			ecryptfs_dentry, rc);
		goto out;
	}
#ifdef CONFIG_DLP
	if(crypt_stat->mount_crypt_stat->flags & ECRYPTFS_MOUNT_DLP_ENABLED) {
#if DLP_DEBUG
		printk(KERN_ERR "DLP %s: file name: [%s], userid: [%d]\n",
				__func__, ecryptfs_dentry->d_iname, crypt_stat->mount_crypt_stat->userid);
#endif
		if(!rc && (in_egroup_p(AID_KNOX_DLP) || in_egroup_p(AID_KNOX_DLP_RESTRICTED) || in_egroup_p(AID_KNOX_DLP_MEDIA))) {
			/* TODO: Can DLP files be created while in locked state? */
			struct timespec ts;
			crypt_stat->flags |= ECRYPTFS_DLP_ENABLED;
			getnstimeofday(&ts);
			if(in_egroup_p(AID_KNOX_DLP_MEDIA)) {
				printk(KERN_ERR "DLP %s: media process creating file  : %s\n", __func__, ecryptfs_dentry->d_iname);
			} else {
				crypt_stat->expiry.expiry_time.tv_sec = (int64_t)ts.tv_sec + 20;
				crypt_stat->expiry.expiry_time.tv_nsec = (int64_t)ts.tv_nsec;
			}
#if DLP_DEBUG
			printk(KERN_ERR "DLP %s: current->pid : %d\n", __func__, current->tgid);
			printk(KERN_ERR "DLP %s: crypt_stat->mount_crypt_stat->userid : %d\n", __func__, crypt_stat->mount_crypt_stat->userid);
			printk(KERN_ERR "DLP %s: crypt_stat->mount_crypt_stat->partition_id : %d\n", __func__, crypt_stat->mount_crypt_stat->partition_id);
#endif
			if(in_egroup_p(AID_KNOX_DLP)) {
				cmd = sdp_fs_command_alloc(FSOP_DLP_FILE_INIT,
                current->tgid, crypt_stat->mount_crypt_stat->userid, crypt_stat->mount_crypt_stat->partition_id,
                ecryptfs_inode->i_ino, GFP_KERNEL);
			}
			else if(in_egroup_p(AID_KNOX_DLP_RESTRICTED)) {
				cmd = sdp_fs_command_alloc(FSOP_DLP_FILE_INIT_RESTRICTED,
                current->tgid, crypt_stat->mount_crypt_stat->userid, crypt_stat->mount_crypt_stat->partition_id,
                ecryptfs_inode->i_ino, GFP_KERNEL);
			}
		} else {
			printk(KERN_ERR "DLP %s: not in group\n", __func__);
		}
	}
#endif
#ifdef CONFIG_WTL_ENCRYPTION_FILTER
	mutex_lock(&crypt_stat->cs_mutex);
	if (crypt_stat->flags & ECRYPTFS_ENCRYPTED) {
		struct dentry *fp_dentry =
			ecryptfs_inode_to_private(ecryptfs_inode)
			->lower_file->f_path.dentry;
		struct ecryptfs_mount_crypt_stat *mount_crypt_stat =
			&ecryptfs_superblock_to_private(ecryptfs_dentry->d_sb)
			->mount_crypt_stat;
		char filename[NAME_MAX+1] = {0};
		if (fp_dentry->d_name.len <= NAME_MAX)
			memcpy(filename, fp_dentry->d_name.name,
					fp_dentry->d_name.len + 1);

		if ((mount_crypt_stat->flags & ECRYPTFS_ENABLE_NEW_PASSTHROUGH)
		|| ((mount_crypt_stat->flags & ECRYPTFS_ENABLE_FILTERING) &&
			(is_file_name_match(mount_crypt_stat, fp_dentry) ||
			is_file_ext_match(mount_crypt_stat, filename)))) {
			crypt_stat->flags &= ~(ECRYPTFS_I_SIZE_INITIALIZED
				| ECRYPTFS_ENCRYPTED);
			ecryptfs_put_lower_file(ecryptfs_inode);
		} else {
			rc = ecryptfs_write_metadata(ecryptfs_dentry,
				 ecryptfs_inode);
			if (rc)
				printk(
				KERN_ERR "Error writing headers; rc = [%d]\n"
				    , rc);
			ecryptfs_put_lower_file(ecryptfs_inode);
		}
	}
	mutex_unlock(&crypt_stat->cs_mutex);
#else
	rc = ecryptfs_write_metadata(ecryptfs_dentry, ecryptfs_inode);
	if (rc)
		printk(KERN_ERR "Error writing headers; rc = [%d]\n", rc);
	ecryptfs_put_lower_file(ecryptfs_inode);
#endif
out:
#ifdef CONFIG_DLP
	if(cmd) {
		sdp_fs_request(cmd, NULL);
		sdp_fs_command_free(cmd);
	}
#endif
	return rc;
}

/**
 * ecryptfs_create
 * @dir: The inode of the directory in which to create the file.
 * @dentry: The eCryptfs dentry
 * @mode: The mode of the new file.
 *
 * Creates a new file.
 *
 * Returns zero on success; non-zero on error condition
 */
static int
ecryptfs_create(struct inode *directory_inode, struct dentry *ecryptfs_dentry,
		umode_t mode, bool excl)
{
	struct inode *ecryptfs_inode;
	int rc;

	ecryptfs_inode = ecryptfs_do_create(directory_inode, ecryptfs_dentry,
					    mode);
	if (IS_ERR(ecryptfs_inode)) {
		ecryptfs_printk(KERN_WARNING, "Failed to create file in"
				"lower filesystem\n");
		rc = PTR_ERR(ecryptfs_inode);
		goto out;
	}
	/* At this point, a file exists on "disk"; we need to make sure
	 * that this on disk file is prepared to be an ecryptfs file */
	rc = ecryptfs_initialize_file(ecryptfs_dentry, ecryptfs_inode);
	if (rc) {
		ecryptfs_do_unlink(directory_inode, ecryptfs_dentry,
				   ecryptfs_inode);
		make_bad_inode(ecryptfs_inode);
		unlock_new_inode(ecryptfs_inode);
		iput(ecryptfs_inode);
		goto out;
	}
	d_instantiate_new(ecryptfs_dentry, ecryptfs_inode);
out:
	return rc;
}

static int ecryptfs_i_size_read(struct dentry *dentry, struct inode *inode)
{
	struct ecryptfs_crypt_stat *crypt_stat;
	int rc;

	rc = ecryptfs_get_lower_file(dentry, inode);
	if (rc) {
		printk(KERN_ERR "%s: Error attempting to initialize "
			"the lower file for the dentry with name "
			"[%pd]; rc = [%d]\n", __func__,
			dentry, rc);
		return rc;
	}

	crypt_stat = &ecryptfs_inode_to_private(inode)->crypt_stat;
	/* TODO: lock for crypt_stat comparison */
	if (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED))
		ecryptfs_set_default_sizes(crypt_stat);

	rc = ecryptfs_read_and_validate_header_region(inode);
	ecryptfs_put_lower_file(inode);
	if (rc) {
		rc = ecryptfs_read_and_validate_xattr_region(dentry, inode);
		if (!rc)
			crypt_stat->flags |= ECRYPTFS_METADATA_IN_XATTR;
	}

	/* Must return 0 to allow non-eCryptfs files to be looked up, too */
	return 0;
}

/**
 * ecryptfs_lookup_interpose - Dentry interposition for a lookup
 */
static int ecryptfs_lookup_interpose(struct dentry *dentry,
				     struct dentry *lower_dentry,
				     struct inode *dir_inode)
{
	struct path *path = ecryptfs_dentry_to_lower_path(dentry->d_parent);
	struct inode *inode, *lower_inode;
	struct ecryptfs_dentry_info *dentry_info;
	int rc = 0;

	dentry_info = kmem_cache_alloc(ecryptfs_dentry_info_cache, GFP_KERNEL);
	if (!dentry_info) {
		printk(KERN_ERR "%s: Out of memory whilst attempting "
		       "to allocate ecryptfs_dentry_info struct\n",
			__func__);
		dput(lower_dentry);
		return -ENOMEM;
	}

	fsstack_copy_attr_atime(dir_inode, d_inode(path->dentry));
	BUG_ON(!d_count(lower_dentry));

	ecryptfs_set_dentry_private(dentry, dentry_info);
	dentry_info->lower_path.mnt = mntget(path->mnt);
	dentry_info->lower_path.dentry = lower_dentry;

#if (ANDROID_VERSION >= 80000)
	/*
	 * negative dentry can go positive under us here - its parent is not
	 * locked.  That's OK and that could happen just as we return from
	 * ecryptfs_lookup() anyway.  Just need to be careful and fetch
	 * ->d_inode only once - it's not stable here.
	 */
	lower_inode = READ_ONCE(lower_dentry->d_inode);

	if (!lower_inode) {
		/* We want to add because we couldn't find in lower */
		d_add(dentry, NULL);
#endif
		return 0;
	}
	inode = __ecryptfs_get_inode(lower_inode, dir_inode->i_sb);
	if (IS_ERR(inode)) {
		printk(KERN_ERR "%s: Error interposing; rc = [%ld]\n",
		       __func__, PTR_ERR(inode));
		return PTR_ERR(inode);
	}
	if (S_ISREG(inode->i_mode)) {
		rc = ecryptfs_i_size_read(dentry, inode);
		if (rc) {
			make_bad_inode(inode);
			return rc;
		}
	}

#ifdef CONFIG_SDP
	if (S_ISDIR(inode->i_mode) && dentry) {
	    if(IS_UNDER_ROOT(dentry)) {
	        struct ecryptfs_mount_crypt_stat *mount_crypt_stat  =
	                &ecryptfs_superblock_to_private(inode->i_sb)->mount_crypt_stat;
	        int engineid;

	        //printk("Lookup a directoy under root directory of current partition.\n");

	        if(is_chamber_directory(mount_crypt_stat, dentry->d_name.name, &engineid)) {
	            /*
	             * When this directory is under ROOT directory and the name is registered
	             * as Chamber.
	             */
	            printk("This is a chamber directory engine[%d]\n", engineid);
	            set_chamber_flag(engineid, inode);
	        }
	    } else if(IS_SENSITIVE_DENTRY(dentry->d_parent)) {
	        /*
	         * When parent directory is sensitive
	         */
	        struct ecryptfs_crypt_stat *crypt_stat =
	                &ecryptfs_inode_to_private(inode)->crypt_stat;
	        struct ecryptfs_crypt_stat *parent_crypt_stat =
	                &ecryptfs_inode_to_private(dentry->d_parent->d_inode)->crypt_stat;
	        printk("Parent %s is sensitive. so this directory is sensitive too\n",
	                dentry->d_parent->d_name.name);
	        crypt_stat->flags |= ECRYPTFS_DEK_IS_SENSITIVE;
	        crypt_stat->engine_id = parent_crypt_stat->engine_id;
	    }
	}
#endif

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);
	d_add(dentry, inode);

	return rc;
}

#ifdef CONFIG_SDP
static inline int isdigit(int ch)
{
	return (ch >= '0') && (ch <= '9');
}
#endif

/**
 * ecryptfs_lookup
 * @ecryptfs_dir_inode: The eCryptfs directory inode
 * @ecryptfs_dentry: The eCryptfs dentry that we are looking up
 * @flags: lookup flags
 *
 * Find a file on disk. If the file does not exist, then we'll add it to the
 * dentry cache and continue on to read it from the disk.
 */
static struct dentry *ecryptfs_lookup(struct inode *ecryptfs_dir_inode,
				      struct dentry *ecryptfs_dentry,
				      unsigned int flags)
{
	char *encrypted_and_encoded_name = NULL;
	size_t encrypted_and_encoded_name_size;
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat = NULL;
	struct dentry *lower_dir_dentry, *lower_dentry;
	int rc = 0;

	lower_dir_dentry = ecryptfs_dentry_to_lower(ecryptfs_dentry->d_parent);
	mutex_lock(&d_inode(lower_dir_dentry)->i_mutex);
	lower_dentry = lookup_one_len(ecryptfs_dentry->d_name.name,
				      lower_dir_dentry,
				      ecryptfs_dentry->d_name.len);
	mutex_unlock(&d_inode(lower_dir_dentry)->i_mutex);
	if (IS_ERR(lower_dentry)) {
		rc = PTR_ERR(lower_dentry);
		ecryptfs_printk(KERN_DEBUG, "%s: lookup_one_len() returned "
				"[%d] on lower_dentry = [%pd]\n", __func__, rc,
				ecryptfs_dentry);
		goto out;
	}
	if (d_really_is_positive(lower_dentry))
		goto interpose;
	mount_crypt_stat = &ecryptfs_superblock_to_private(
				ecryptfs_dentry->d_sb)->mount_crypt_stat;
	if (!(mount_crypt_stat
	    && (mount_crypt_stat->flags & ECRYPTFS_GLOBAL_ENCRYPT_FILENAMES)))
		goto interpose;
	dput(lower_dentry);
	rc = ecryptfs_encrypt_and_encode_filename(
		&encrypted_and_encoded_name, &encrypted_and_encoded_name_size,
		NULL, mount_crypt_stat, ecryptfs_dentry->d_name.name,
		ecryptfs_dentry->d_name.len);
	if (rc) {
		printk(KERN_ERR "%s: Error attempting to encrypt and encode "
		       "filename; rc = [%d]\n", __func__, rc);
		goto out;
	}
	mutex_lock(&d_inode(lower_dir_dentry)->i_mutex);

#if defined(CONFIG_SDP) && (ANDROID_VERSION < 80000)
	if(!strncmp(lower_dir_dentry->d_sb->s_type->name, "sdcardfs", 8)) {
		struct sdcardfs_dentry_info *dinfo = SDCARDFS_D(lower_dir_dentry);
		struct dentry *parent = dget_parent(lower_dir_dentry);
		struct sdcardfs_dentry_info *parent_info = SDCARDFS_D(parent);

		dinfo->under_knox = 1;
		dinfo->userid = -1;

		if(IS_UNDER_ROOT(ecryptfs_dentry)) {
			parent_info->permission = PERMISSION_PRE_ROOT;
			if(mount_crypt_stat->userid >= 100 && mount_crypt_stat->userid < 2000) {
				parent_info->userid = mount_crypt_stat->userid;

				/* Assume masked off by default. */
				if (!strcasecmp(ecryptfs_dentry->d_name.name, "Android")) {
					/* App-specific directories inside; let anyone traverse */
					dinfo->permission = PERMISSION_ROOT;
				}	
			}
			else {
				int len = strlen(ecryptfs_dentry->d_name.name);
				int i, numeric = 1;

				for(i=0 ; i < len ; i++)
					if(!isdigit(ecryptfs_dentry->d_name.name[i])) { numeric = 0; break; }
				if(numeric) {
					dinfo->userid = simple_strtoul(ecryptfs_dentry->d_name.name, NULL, 10);
				}
			} 
		}
		else {
			struct sdcardfs_sb_info *sbi = SDCARDFS_SB(lower_dir_dentry->d_sb);
			
			/* Derive custom permissions based on parent and current node */
			switch (parent_info->permission) {
				case PERMISSION_ROOT:
					if (!strcasecmp(ecryptfs_dentry->d_name.name, "data") || !strcasecmp(ecryptfs_dentry->d_name.name, "obb") || !strcasecmp(ecryptfs_dentry->d_name.name, "media")) {
						/* App-specific directories inside; let anyone traverse */
						dinfo->permission = PERMISSION_ANDROID;
					} 
					break;
               			case PERMISSION_ANDROID:
					dinfo->permission = PERMISSION_UNDER_ANDROID;
               				dinfo->appid = get_appid(sbi->pkgl_id, ecryptfs_dentry->d_name.name);
					break;
			}
		}
		dput(parent);
	}
#endif

	lower_dentry = lookup_one_len(encrypted_and_encoded_name,
				      lower_dir_dentry,
				      encrypted_and_encoded_name_size);
#if defined(CONFIG_SDP) && (ANDROID_VERSION < 80000)
	if(!strncmp(lower_dir_dentry->d_sb->s_type->name, "sdcardfs", 8)) {
		struct sdcardfs_dentry_info *dinfo = SDCARDFS_D(lower_dir_dentry);
		dinfo->under_knox = 0;
		dinfo->userid = -1;
	}
#endif
	mutex_unlock(&d_inode(lower_dir_dentry)->i_mutex);
	if (IS_ERR(lower_dentry)) {
		rc = PTR_ERR(lower_dentry);
		ecryptfs_printk(KERN_DEBUG, "%s: lookup_one_len() returned "
				"[%d] on lower_dentry = [%s]\n", __func__, rc,
				encrypted_and_encoded_name);
		goto out;
	}
interpose:
	rc = ecryptfs_lookup_interpose(ecryptfs_dentry, lower_dentry,
				       ecryptfs_dir_inode);
out:
	kfree(encrypted_and_encoded_name);
	return ERR_PTR(rc);
}

static int ecryptfs_link(struct dentry *old_dentry, struct inode *dir,
			 struct dentry *new_dentry)
{
	struct dentry *lower_old_dentry;
	struct dentry *lower_new_dentry;
	struct dentry *lower_dir_dentry;
	u64 file_size_save;
	int rc;

	file_size_save = i_size_read(d_inode(old_dentry));
	lower_old_dentry = ecryptfs_dentry_to_lower(old_dentry);
	lower_new_dentry = ecryptfs_dentry_to_lower(new_dentry);
	dget(lower_old_dentry);
	dget(lower_new_dentry);
	lower_dir_dentry = lock_parent(lower_new_dentry);
	rc = vfs_link(lower_old_dentry, d_inode(lower_dir_dentry),
		      lower_new_dentry, NULL);
	if (rc || d_really_is_negative(lower_new_dentry))
		goto out_lock;
	rc = ecryptfs_interpose(lower_new_dentry, new_dentry, dir->i_sb);
	if (rc)
		goto out_lock;
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
	set_nlink(d_inode(old_dentry),
		  ecryptfs_inode_to_lower(d_inode(old_dentry))->i_nlink);
	i_size_write(d_inode(new_dentry), file_size_save);
out_lock:
	unlock_dir(lower_dir_dentry);
	dput(lower_new_dentry);
	dput(lower_old_dentry);
	return rc;
}

static int ecryptfs_unlink(struct inode *dir, struct dentry *dentry)
{
	return ecryptfs_do_unlink(dir, dentry, d_inode(dentry));
}

static int ecryptfs_symlink(struct inode *dir, struct dentry *dentry,
			    const char *symname)
{
	int rc;
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	char *encoded_symname;
	size_t encoded_symlen;
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat = NULL;

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	dget(lower_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	mount_crypt_stat = &ecryptfs_superblock_to_private(
		dir->i_sb)->mount_crypt_stat;
	rc = ecryptfs_encrypt_and_encode_filename(&encoded_symname,
						  &encoded_symlen,
						  NULL,
						  mount_crypt_stat, symname,
						  strlen(symname));
	if (rc)
		goto out_lock;
	rc = vfs_symlink(d_inode(lower_dir_dentry), lower_dentry,
			 encoded_symname);
	kfree(encoded_symname);
	if (rc || d_really_is_negative(lower_dentry))
		goto out_lock;
	rc = ecryptfs_interpose(lower_dentry, dentry, dir->i_sb);
	if (rc)
		goto out_lock;
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
out_lock:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	if (d_really_is_negative(dentry))
		d_drop(dentry);
	return rc;
}

static int ecryptfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int rc;
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	lower_dir_dentry = lock_parent(lower_dentry);

#if defined(CONFIG_SDP) && (ANDROID_VERSION < 80000)
	if(!strncmp(lower_dir_dentry->d_sb->s_type->name, "sdcardfs", 8)) {
		struct sdcardfs_dentry_info *dinfo = SDCARDFS_D(lower_dir_dentry);
		int len = strlen(dentry->d_name.name);
		int i, numeric = 1;

		dinfo->under_knox = 1;
		dinfo->userid = -1;
		if(IS_UNDER_ROOT(dentry)) {
			for(i=0 ; i < len ; i++)
				if(!isdigit(dentry->d_name.name[i])) { numeric = 0; break; }
			if(numeric) {
				dinfo->userid = simple_strtoul(dentry->d_name.name, NULL, 10);
			}
		}
	}
#endif
	rc = vfs_mkdir(d_inode(lower_dir_dentry), lower_dentry, mode);
	if (rc || d_really_is_negative(lower_dentry))
		goto out;
	rc = ecryptfs_interpose(lower_dentry, dentry, dir->i_sb);
	if (rc)
		goto out;
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
	set_nlink(dir, d_inode(lower_dir_dentry)->i_nlink);
out:
#if defined(CONFIG_SDP) && (ANDROID_VERSION < 80000)
	if(!strncmp(lower_dir_dentry->d_sb->s_type->name, "sdcardfs", 8)) {
		struct sdcardfs_dentry_info *dinfo = SDCARDFS_D(lower_dir_dentry);
		dinfo->under_knox = 0;
		dinfo->userid = -1;
	}
#endif
	unlock_dir(lower_dir_dentry);
	if (d_really_is_negative(dentry))
		d_drop(dentry);
	return rc;
}

static int ecryptfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	int rc;

#ifdef CONFIG_SDP
	if(IS_CHAMBER_DENTRY(dentry)) {
		printk("You're removing chamber directory. I/O error\n");
		return -EIO;
	}
#endif

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	dget(dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	dget(lower_dentry);
	rc = vfs_rmdir(d_inode(lower_dir_dentry), lower_dentry);
	dput(lower_dentry);
	if (!rc && d_really_is_positive(dentry))
		clear_nlink(d_inode(dentry));
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	set_nlink(dir, d_inode(lower_dir_dentry)->i_nlink);
	unlock_dir(lower_dir_dentry);
	if (!rc)
		d_drop(dentry);
	dput(dentry);
	return rc;
}

static int
ecryptfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode, dev_t dev)
{
	int rc;
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	rc = vfs_mknod(d_inode(lower_dir_dentry), lower_dentry, mode, dev);
	if (rc || d_really_is_negative(lower_dentry))
		goto out;
	rc = ecryptfs_interpose(lower_dentry, dentry, dir->i_sb);
	if (rc)
		goto out;
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
out:
	unlock_dir(lower_dir_dentry);
	if (d_really_is_negative(dentry))
		d_drop(dentry);
	return rc;
}

#define ECRYPTFS_SDP_RENAME_DEBUG 0

static int
ecryptfs_rename(struct inode *old_dir, struct dentry *old_dentry,
		struct inode *new_dir, struct dentry *new_dentry,
		unsigned int flags)
{
	int rc;
	struct dentry *lower_old_dentry;
	struct dentry *lower_new_dentry;
	struct dentry *lower_old_dir_dentry;
	struct dentry *lower_new_dir_dentry;
	struct dentry *trap = NULL;
	struct inode *target_inode;
#ifdef CONFIG_DLP
	sdp_fs_command_t *cmd1 = NULL;
	unsigned long old_inode = old_dentry->d_inode->i_ino;
#endif
#ifdef CONFIG_SDP
	sdp_fs_command_t *cmd = NULL;
	int rename_event = 0x00;
	struct ecryptfs_crypt_stat *crypt_stat =
	        &(ecryptfs_inode_to_private(old_dentry->d_inode)->crypt_stat);
	struct ecryptfs_crypt_stat *parent_crypt_stat =
	        &(ecryptfs_inode_to_private(old_dentry->d_parent->d_inode)->crypt_stat);
	struct ecryptfs_crypt_stat *new_parent_crypt_stat =
	        &(ecryptfs_inode_to_private(new_dentry->d_parent->d_inode)->crypt_stat);
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat =
	        &ecryptfs_superblock_to_private(old_dentry->d_sb)->mount_crypt_stat;

	if (flags)
		return -EINVAL;

#if ECRYPTFS_SDP_RENAME_DEBUG
	printk("You're renaming %s to %s\n",
			old_dentry->d_name.name,
			new_dentry->d_name.name);
	printk("old_dentry[%p] : %s [parent %s : %s] inode:%p\n",
			old_dentry, old_dentry->d_name.name,
			old_dentry->d_parent->d_name.name,
			IS_SENSITIVE_DENTRY(old_dentry->d_parent) ? "sensitive" : "protected",
					old_dentry->d_inode);
	printk("new_dentry[%p] : %s [parent %s : %s] inode:%p\n",
			new_dentry, new_dentry->d_name.name,
			new_dentry->d_parent->d_name.name,
			IS_SENSITIVE_DENTRY(new_dentry->d_parent) ? "sensitive" : "protected",
					new_dentry->d_inode);
#endif

    if(IS_CHAMBER_DENTRY(old_dentry)) {
        printk("Rename trial on chamber : failed\n");
        return -EIO;
    }

#if 0 // kernel panic. new_crypt_stat->engine_id
    if(IS_SENSITIVE_DENTRY(old_dentry->d_parent) &&
            IS_SENSITIVE_DENTRY(new_dentry->d_parent)) {
        if(crypt_stat->engine_id != new_crypt_stat->engine_id) {
            printk("Rename chamber file to another chamber : failed\n");
            return -EIO;
        }
    }
#endif

	if(IS_SENSITIVE_DENTRY(old_dentry->d_parent)) {
	    if(ecryptfs_is_sdp_locked(parent_crypt_stat->engine_id)) {
	        printk("Rename/move trial in locked state\n");
	        return -EIO;
	    }
	}

	if(IS_SENSITIVE_DENTRY(old_dentry->d_parent) &&
			IS_SENSITIVE_DENTRY(new_dentry->d_parent)) {
		if(parent_crypt_stat->engine_id != new_parent_crypt_stat->engine_id) {
	        printk("Can't move between chambers\n");
			return -EIO;
		}
	}

	if(IS_SENSITIVE_DENTRY(old_dentry->d_parent) &&
			!IS_SENSITIVE_DENTRY(new_dentry->d_parent))
		rename_event |= ECRYPTFS_EVT_RENAME_OUT_OF_CHAMBER;

	if(!IS_SENSITIVE_DENTRY(old_dentry->d_parent) &&
			IS_SENSITIVE_DENTRY(new_dentry->d_parent))
		rename_event |= ECRYPTFS_EVT_RENAME_TO_CHAMBER;
#endif

	lower_old_dentry = ecryptfs_dentry_to_lower(old_dentry);
	lower_new_dentry = ecryptfs_dentry_to_lower(new_dentry);
	dget(lower_old_dentry);
	dget(lower_new_dentry);
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);
	target_inode = d_inode(new_dentry);
	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	/* source should not be ancestor of target */
	if (trap == lower_old_dentry) {
		rc = -EINVAL;
		goto out_lock;
	}
	/* target should not be ancestor of source */
	if (trap == lower_new_dentry) {
		rc = -ENOTEMPTY;
		goto out_lock;
	}
	rc = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			d_inode(lower_new_dir_dentry), lower_new_dentry,
			NULL, 0);
	if (rc)
		goto out_lock;
	if (target_inode)
		fsstack_copy_attr_all(target_inode,
				      ecryptfs_inode_to_lower(target_inode));
	fsstack_copy_attr_all(new_dir, d_inode(lower_new_dir_dentry));
	if (new_dir != old_dir)
		fsstack_copy_attr_all(old_dir, d_inode(lower_old_dir_dentry));

#ifdef CONFIG_SDP
	if(!rc) {
		crypt_stat = &(ecryptfs_inode_to_private(old_dentry->d_inode)->crypt_stat);

        if(rename_event > 0) {
            switch(rename_event) {
            case ECRYPTFS_EVT_RENAME_TO_CHAMBER:
                cmd = sdp_fs_command_alloc(FSOP_SDP_SET_SENSITIVE, current->pid,
                		mount_crypt_stat->userid, mount_crypt_stat->partition_id,
                        old_dentry->d_inode->i_ino,
                        GFP_NOFS);
                break;
            case ECRYPTFS_EVT_RENAME_OUT_OF_CHAMBER:
                cmd = sdp_fs_command_alloc(FSOP_SDP_SET_PROTECTED, current->pid,
                		mount_crypt_stat->userid, mount_crypt_stat->partition_id,
                        old_dentry->d_inode->i_ino,
                        GFP_NOFS);
                break;
            default:
                cmd = NULL;
                break;
            }
        }
#if ECRYPTFS_SDP_RENAME_DEBUG
		printk("[end of rename] old_dentry[%p] : %s [parent %s : %s] inode:%p\n",
				old_dentry, old_dentry->d_name.name,
				old_dentry->d_parent->d_name.name,
				IS_SENSITIVE_DENTRY(old_dentry->d_parent) ? "sensitive" : "protected",
						old_dentry->d_inode);
		printk("[end of rename] new_dentry[%p] : %s [parent %s : %s] inode:%p\n",
				new_dentry, new_dentry->d_name.name,
				new_dentry->d_parent->d_name.name,
				IS_SENSITIVE_DENTRY(new_dentry->d_parent) ? "sensitive" : "protected",
						new_dentry->d_inode);
#endif

    }
#endif

out_lock:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dentry);
	dput(lower_old_dentry);

#ifdef CONFIG_SDP
	if(!rc && cmd != NULL) {
	    sdp_fs_request(cmd, ecryptfs_fs_request_callback);
	    sdp_fs_command_free(cmd);
	}
#endif

#ifdef CONFIG_DLP
	//create new init command and send--Handle transient case MS-Apps
	if(crypt_stat->flags & ECRYPTFS_DLP_ENABLED) {
		if(!rc && (in_egroup_p(AID_KNOX_DLP) || in_egroup_p(AID_KNOX_DLP_RESTRICTED))){
            cmd1 = sdp_fs_command_alloc(FSOP_DLP_FILE_RENAME,
						current->tgid, mount_crypt_stat->userid, mount_crypt_stat->partition_id,
						old_inode, GFP_KERNEL);
            //send cmd
			if(cmd1) {
                sdp_fs_request(cmd1, NULL);
                sdp_fs_command_free(cmd1);
			}
		}
	}
    //end- Handle transient case MS-Apps
#endif
	return rc;
}

static char *ecryptfs_readlink_lower(struct dentry *dentry, size_t *bufsiz)
{
	struct dentry *lower_dentry = ecryptfs_dentry_to_lower(dentry);
	char *lower_buf;
	char *buf;
	mm_segment_t old_fs;
	int rc;

	lower_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!lower_buf)
		return ERR_PTR(-ENOMEM);
	old_fs = get_fs();
	set_fs(get_ds());
	rc = d_inode(lower_dentry)->i_op->readlink(lower_dentry,
						   (char __user *)lower_buf,
						   PATH_MAX);
	set_fs(old_fs);
	if (rc < 0)
		goto out;
	rc = ecryptfs_decode_and_decrypt_filename(&buf, bufsiz, dentry->d_sb,
						  lower_buf, rc);
out:
	kfree(lower_buf);
	return rc ? ERR_PTR(rc) : buf;
}

static const char *ecryptfs_follow_link(struct dentry *dentry, void **cookie)
{
	size_t len;
	char *buf = ecryptfs_readlink_lower(dentry, &len);
	if (IS_ERR(buf))
		return buf;
	fsstack_copy_attr_atime(d_inode(dentry),
				d_inode(ecryptfs_dentry_to_lower(dentry)));
	buf[len] = '\0';
	return *cookie = buf;
}

/**
 * upper_size_to_lower_size
 * @crypt_stat: Crypt_stat associated with file
 * @upper_size: Size of the upper file
 *
 * Calculate the required size of the lower file based on the
 * specified size of the upper file. This calculation is based on the
 * number of headers in the underlying file and the extent size.
 *
 * Returns Calculated size of the lower file.
 */
static loff_t
upper_size_to_lower_size(struct ecryptfs_crypt_stat *crypt_stat,
			 loff_t upper_size)
{
	loff_t lower_size;

	lower_size = ecryptfs_lower_header_size(crypt_stat);
	if (upper_size != 0) {
		loff_t num_extents;

		num_extents = upper_size >> crypt_stat->extent_shift;
		if (upper_size & ~crypt_stat->extent_mask)
			num_extents++;
		lower_size += (num_extents * crypt_stat->extent_size);
	}
	return lower_size;
}

/**
 * truncate_upper
 * @dentry: The ecryptfs layer dentry
 * @ia: Address of the ecryptfs inode's attributes
 * @lower_ia: Address of the lower inode's attributes
 *
 * Function to handle truncations modifying the size of the file. Note
 * that the file sizes are interpolated. When expanding, we are simply
 * writing strings of 0's out. When truncating, we truncate the upper
 * inode and update the lower_ia according to the page index
 * interpolations. If ATTR_SIZE is set in lower_ia->ia_valid upon return,
 * the caller must use lower_ia in a call to notify_change() to perform
 * the truncation of the lower inode.
 *
 * Returns zero on success; non-zero otherwise
 */
static int truncate_upper(struct dentry *dentry, struct iattr *ia,
			  struct iattr *lower_ia)
{
	int rc = 0;
	struct inode *inode = d_inode(dentry);
	struct ecryptfs_crypt_stat *crypt_stat;
	loff_t i_size = i_size_read(inode);
	loff_t lower_size_before_truncate;
	loff_t lower_size_after_truncate;

	if (unlikely((ia->ia_size == i_size))) {
		lower_ia->ia_valid &= ~ATTR_SIZE;
		return 0;
	}
	rc = ecryptfs_get_lower_file(dentry, inode);
	if (rc)
		return rc;
	crypt_stat = &ecryptfs_inode_to_private(d_inode(dentry))->crypt_stat;
	/* Switch on growing or shrinking file */
	if (ia->ia_size > i_size) {
		char zero[] = { 0x00 };

		lower_ia->ia_valid &= ~ATTR_SIZE;
		/* Write a single 0 at the last position of the file;
		 * this triggers code that will fill in 0's throughout
		 * the intermediate portion of the previous end of the
		 * file and the new and of the file */
		rc = ecryptfs_write(inode, zero,
				    (ia->ia_size - 1), 1);
	} else { /* ia->ia_size < i_size_read(inode) */
		/* We're chopping off all the pages down to the page
		 * in which ia->ia_size is located. Fill in the end of
		 * that page from (ia->ia_size & ~PAGE_CACHE_MASK) to
		 * PAGE_CACHE_SIZE with zeros. */
		size_t num_zeros = (PAGE_CACHE_SIZE
				    - (ia->ia_size & ~PAGE_CACHE_MASK));

		if (!(crypt_stat->flags & ECRYPTFS_ENCRYPTED)) {
			truncate_setsize(inode, ia->ia_size);
			lower_ia->ia_size = ia->ia_size;
			lower_ia->ia_valid |= ATTR_SIZE;
			goto out;
		}
		if (num_zeros) {
			char *zeros_virt;

			zeros_virt = kzalloc(num_zeros, GFP_KERNEL);
			if (!zeros_virt) {
				rc = -ENOMEM;
				goto out;
			}
			rc = ecryptfs_write(inode, zeros_virt,
					    ia->ia_size, num_zeros);
			kfree(zeros_virt);
			if (rc) {
				printk(KERN_ERR "Error attempting to zero out "
				       "the remainder of the end page on "
				       "reducing truncate; rc = [%d]\n", rc);
				goto out;
			}
		}
		truncate_setsize(inode, ia->ia_size);
		rc = ecryptfs_write_inode_size_to_metadata(inode);
		if (rc) {
			printk(KERN_ERR	"Problem with "
			       "ecryptfs_write_inode_size_to_metadata; "
			       "rc = [%d]\n", rc);
			goto out;
		}
		/* We are reducing the size of the ecryptfs file, and need to
		 * know if we need to reduce the size of the lower file. */
		lower_size_before_truncate =
		    upper_size_to_lower_size(crypt_stat, i_size);
		lower_size_after_truncate =
		    upper_size_to_lower_size(crypt_stat, ia->ia_size);
		if (lower_size_after_truncate < lower_size_before_truncate) {
			lower_ia->ia_size = lower_size_after_truncate;
			lower_ia->ia_valid |= ATTR_SIZE;
		} else
			lower_ia->ia_valid &= ~ATTR_SIZE;
	}
out:
	ecryptfs_put_lower_file(inode);
	return rc;
}

static int ecryptfs_inode_newsize_ok(struct inode *inode, loff_t offset)
{
	struct ecryptfs_crypt_stat *crypt_stat;
	loff_t lower_oldsize, lower_newsize;

	crypt_stat = &ecryptfs_inode_to_private(inode)->crypt_stat;
	lower_oldsize = upper_size_to_lower_size(crypt_stat,
						 i_size_read(inode));
	lower_newsize = upper_size_to_lower_size(crypt_stat, offset);
	if (lower_newsize > lower_oldsize) {
		/*
		 * The eCryptfs inode and the new *lower* size are mixed here
		 * because we may not have the lower i_mutex held and/or it may
		 * not be appropriate to call inode_newsize_ok() with inodes
		 * from other filesystems.
		 */
		return inode_newsize_ok(inode, lower_newsize);
	}

	return 0;
}

/**
 * ecryptfs_truncate
 * @dentry: The ecryptfs layer dentry
 * @new_length: The length to expand the file to
 *
 * Simple function that handles the truncation of an eCryptfs inode and
 * its corresponding lower inode.
 *
 * Returns zero on success; non-zero otherwise
 */
int ecryptfs_truncate(struct dentry *dentry, loff_t new_length)
{
	struct iattr ia = { .ia_valid = ATTR_SIZE, .ia_size = new_length };
	struct iattr lower_ia = { .ia_valid = 0 };
	int rc;

	rc = ecryptfs_inode_newsize_ok(d_inode(dentry), new_length);
	if (rc)
		return rc;

	rc = truncate_upper(dentry, &ia, &lower_ia);
	if (!rc && lower_ia.ia_valid & ATTR_SIZE) {
		struct dentry *lower_dentry = ecryptfs_dentry_to_lower(dentry);

		mutex_lock(&d_inode(lower_dentry)->i_mutex);
		rc = notify_change(lower_dentry, &lower_ia, NULL);
		mutex_unlock(&d_inode(lower_dentry)->i_mutex);
	}
	return rc;
}

static int
ecryptfs_permission(struct inode *inode, int mask)
{
	return inode_permission(ecryptfs_inode_to_lower(inode), mask);
}

/**
 * ecryptfs_setattr
 * @dentry: dentry handle to the inode to modify
 * @ia: Structure with flags of what to change and values
 *
 * Updates the metadata of an inode. If the update is to the size
 * i.e. truncation, then ecryptfs_truncate will handle the size modification
 * of both the ecryptfs inode and the lower inode.
 *
 * All other metadata changes will be passed right to the lower filesystem,
 * and we will just update our inode to look like the lower.
 */
static int ecryptfs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int rc = 0;
	struct dentry *lower_dentry;
	struct iattr lower_ia;
	struct inode *inode;
	struct inode *lower_inode;
	struct ecryptfs_crypt_stat *crypt_stat;

	crypt_stat = &ecryptfs_inode_to_private(d_inode(dentry))->crypt_stat;
	if (!(crypt_stat->flags & ECRYPTFS_STRUCT_INITIALIZED))
		ecryptfs_init_crypt_stat(crypt_stat);
	inode = d_inode(dentry);
	lower_inode = ecryptfs_inode_to_lower(inode);
	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	mutex_lock(&crypt_stat->cs_mutex);
	if (d_is_dir(dentry))
		crypt_stat->flags &= ~(ECRYPTFS_ENCRYPTED);
	else if (d_is_reg(dentry)
		 && (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED)
		     || !(crypt_stat->flags & ECRYPTFS_KEY_VALID))) {
		struct ecryptfs_mount_crypt_stat *mount_crypt_stat;

		mount_crypt_stat = &ecryptfs_superblock_to_private(
			dentry->d_sb)->mount_crypt_stat;
		rc = ecryptfs_get_lower_file(dentry, inode);
		if (rc) {
			mutex_unlock(&crypt_stat->cs_mutex);
			goto out;
		}
		rc = ecryptfs_read_metadata(dentry);
		ecryptfs_put_lower_file(inode);
		if (rc) {
			if (!(mount_crypt_stat->flags
			      & ECRYPTFS_PLAINTEXT_PASSTHROUGH_ENABLED)) {
				rc = -EIO;
				printk(KERN_WARNING "Either the lower file "
				       "is not in a valid eCryptfs format, "
				       "or the key could not be retrieved. "
				       "Plaintext passthrough mode is not "
				       "enabled; returning -EIO\n");
				mutex_unlock(&crypt_stat->cs_mutex);
				goto out;
			}
			rc = 0;
			crypt_stat->flags &= ~(ECRYPTFS_I_SIZE_INITIALIZED
					       | ECRYPTFS_ENCRYPTED);
		}
	}
	mutex_unlock(&crypt_stat->cs_mutex);

	rc = inode_change_ok(inode, ia);
	if (rc)
		goto out;
	if (ia->ia_valid & ATTR_SIZE) {
		rc = ecryptfs_inode_newsize_ok(inode, ia->ia_size);
		if (rc)
			goto out;
	}

	memcpy(&lower_ia, ia, sizeof(lower_ia));
	if (ia->ia_valid & ATTR_FILE)
		lower_ia.ia_file = ecryptfs_file_to_lower(ia->ia_file);
	if (ia->ia_valid & ATTR_SIZE) {
		rc = truncate_upper(dentry, ia, &lower_ia);
		if (rc < 0)
			goto out;
	}

	/*
	 * mode change is for clearing setuid/setgid bits. Allow lower fs
	 * to interpret this in its own way.
	 */
	if (lower_ia.ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		lower_ia.ia_valid &= ~ATTR_MODE;

	mutex_lock(&d_inode(lower_dentry)->i_mutex);
	rc = notify_change(lower_dentry, &lower_ia, NULL);
	mutex_unlock(&d_inode(lower_dentry)->i_mutex);
out:
	fsstack_copy_attr_all(inode, lower_inode);
	return rc;
}

static int ecryptfs_getattr_link(struct vfsmount *mnt, struct dentry *dentry,
				 struct kstat *stat)
{
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat;
	int rc = 0;

	mount_crypt_stat = &ecryptfs_superblock_to_private(
						dentry->d_sb)->mount_crypt_stat;
	generic_fillattr(d_inode(dentry), stat);
	if (mount_crypt_stat->flags & ECRYPTFS_GLOBAL_ENCRYPT_FILENAMES) {
		char *target;
		size_t targetsiz;

		target = ecryptfs_readlink_lower(dentry, &targetsiz);
		if (!IS_ERR(target)) {
			kfree(target);
			stat->size = targetsiz;
		} else {
			rc = PTR_ERR(target);
		}
	}
	return rc;
}

static int ecryptfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
			    struct kstat *stat)
{
	struct kstat lower_stat;
	int rc;

	rc = vfs_getattr(ecryptfs_dentry_to_lower_path(dentry), &lower_stat);
	if (!rc) {
		fsstack_copy_attr_all(d_inode(dentry),
				      ecryptfs_inode_to_lower(d_inode(dentry)));
		generic_fillattr(d_inode(dentry), stat);
		stat->blocks = lower_stat.blocks;
	}
	return rc;
}

int
ecryptfs_setxattr(struct dentry *dentry, const char *name, const void *value,
		  size_t size, int flags)
{
	int rc = 0;
	struct dentry *lower_dentry;
#ifdef CONFIG_DLP
	struct ecryptfs_crypt_stat *crypt_stat = NULL;
	int flag = 1;
#endif

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	if (!d_inode(lower_dentry)->i_op->setxattr) {
		rc = -EOPNOTSUPP;
		goto out;
	}

#ifdef CONFIG_DLP
	if (!strcmp(name, KNOX_DLP_XATTR_NAME)) {
#if DLP_DEBUG
		printk(KERN_ERR "DLP %s: setting knox_dlp by [%d]\n", __func__, from_kuid(&init_user_ns, current_uid()));
#endif
		if (!is_root() && !is_system_server()) {
			printk(KERN_ERR "DLP %s: setting knox_dlp not allowed by [%d]\n", __func__, from_kuid(&init_user_ns, current_uid()));
			return -EPERM;
		}
		if (dentry->d_inode) {
			crypt_stat = &ecryptfs_inode_to_private(dentry->d_inode)->crypt_stat;
			if(crypt_stat) {
				crypt_stat->flags |= ECRYPTFS_DLP_ENABLED;
				flag = 0;
			}
		}
		if(flag){
			printk(KERN_ERR "DLP %s: setting knox_dlp failed\n", __func__);
			return -EOPNOTSUPP;
		}
	}
#endif

	rc = vfs_setxattr(lower_dentry, name, value, size, flags);
	if (!rc && d_really_is_positive(dentry))
		fsstack_copy_attr_all(d_inode(dentry), d_inode(lower_dentry));
out:
	return rc;
}

ssize_t
ecryptfs_getxattr_lower(struct dentry *lower_dentry, const char *name,
			void *value, size_t size)
{
	int rc = 0;

	if (!d_inode(lower_dentry)->i_op->getxattr) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	mutex_lock(&d_inode(lower_dentry)->i_mutex);
	rc = d_inode(lower_dentry)->i_op->getxattr(lower_dentry, name, value,
						   size);
	mutex_unlock(&d_inode(lower_dentry)->i_mutex);
out:
	return rc;
}

static ssize_t
ecryptfs_getxattr(struct dentry *dentry, const char *name, void *value,
		  size_t size)
{
#ifdef CONFIG_DLP
	int rc = 0;
	struct ecryptfs_crypt_stat *crypt_stat = NULL;

	rc = ecryptfs_getxattr_lower(ecryptfs_dentry_to_lower(dentry), name,
			value, size);

	if (rc == 8 && !strcmp(name, KNOX_DLP_XATTR_NAME)) {
		uint32_t msw, lsw;
		struct knox_dlp_data *dlp_data = value;
		if (size < sizeof(struct knox_dlp_data)) {
			return -ERANGE;
		}
		msw = (dlp_data->expiry_time.tv_sec >> 32) & 0xFFFFFFFF;
		lsw = dlp_data->expiry_time.tv_sec & 0xFFFFFFFF;
		dlp_data->expiry_time.tv_sec = (uint64_t)lsw;
		dlp_data->expiry_time.tv_nsec = (uint64_t)msw;
		rc = sizeof(struct knox_dlp_data);
#if DLP_DEBUG
		printk(KERN_ERR "DLP %s: conversion done, tv_sec=[%ld]\n",
				__func__, (long)dlp_data->expiry_time.tv_sec);
#endif
	}

	if ((rc == -ENODATA) && (!strcmp(name, KNOX_DLP_XATTR_NAME))) {
		if (dentry->d_inode) {
			crypt_stat = &ecryptfs_inode_to_private(dentry->d_inode)->crypt_stat;
		}
		if (crypt_stat && (crypt_stat->flags & ECRYPTFS_DLP_ENABLED)) {
			if (size < sizeof(struct knox_dlp_data)) {
				return -ERANGE;
			}
			if (crypt_stat->expiry.expiry_time.tv_sec <= 0) {
#if DLP_DEBUG
				printk(KERN_ERR "DLP %s: expiry time=[%ld], fileName [%s]\n", __func__, (long)crypt_stat->expiry.expiry_time.tv_sec, dentry->d_name.name);
#endif
			}
			memcpy(value, &crypt_stat->expiry, sizeof(struct knox_dlp_data));
			rc = sizeof(struct knox_dlp_data);
		}
	}
	return rc;

#else
	return ecryptfs_getxattr_lower(ecryptfs_dentry_to_lower(dentry), name,
				       value, size);
#endif
}

static ssize_t
ecryptfs_listxattr(struct dentry *dentry, char *list, size_t size)
{
	int rc = 0;
	struct dentry *lower_dentry;

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	if (!d_inode(lower_dentry)->i_op->listxattr) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	mutex_lock(&d_inode(lower_dentry)->i_mutex);
	rc = d_inode(lower_dentry)->i_op->listxattr(lower_dentry, list, size);
	mutex_unlock(&d_inode(lower_dentry)->i_mutex);
out:
	return rc;
}

static int ecryptfs_removexattr(struct dentry *dentry, const char *name)
{
	int rc = 0;
	struct dentry *lower_dentry;

	lower_dentry = ecryptfs_dentry_to_lower(dentry);
	if (!d_inode(lower_dentry)->i_op->removexattr) {
		rc = -EOPNOTSUPP;
		goto out;
	}

#ifdef CONFIG_DLP
	if (!strcmp(name, KNOX_DLP_XATTR_NAME)) {
#if DLP_DEBUG
		printk(KERN_ERR "DLP %s: removing knox_dlp by [%d]\n", __func__, from_kuid(&init_user_ns, current_uid()));
#endif
		if (!is_root() && !is_system_server()) {
			printk(KERN_ERR "DLP %s: removing knox_dlp not allowed by [%d]\n", __func__, from_kuid(&init_user_ns, current_uid()));
			return -EPERM;
		}
	}
#endif

	mutex_lock(&d_inode(lower_dentry)->i_mutex);
	rc = d_inode(lower_dentry)->i_op->removexattr(lower_dentry, name);
	mutex_unlock(&d_inode(lower_dentry)->i_mutex);
out:
	return rc;
}

const struct inode_operations ecryptfs_symlink_iops = {
	.readlink = generic_readlink,
	.follow_link = ecryptfs_follow_link,
	.put_link = kfree_put_link,
	.permission = ecryptfs_permission,
	.setattr = ecryptfs_setattr,
	.getattr = ecryptfs_getattr_link,
	.setxattr = ecryptfs_setxattr,
	.getxattr = ecryptfs_getxattr,
	.listxattr = ecryptfs_listxattr,
	.removexattr = ecryptfs_removexattr
};

const struct inode_operations ecryptfs_dir_iops = {
	.create = ecryptfs_create,
	.lookup = ecryptfs_lookup,
	.link = ecryptfs_link,
	.unlink = ecryptfs_unlink,
	.symlink = ecryptfs_symlink,
	.mkdir = ecryptfs_mkdir,
	.rmdir = ecryptfs_rmdir,
	.mknod = ecryptfs_mknod,
	.rename = ecryptfs_rename,
	.permission = ecryptfs_permission,
	.setattr = ecryptfs_setattr,
	.getattr = ecryptfs_getattr,
	.setxattr = ecryptfs_setxattr,
	.getxattr = ecryptfs_getxattr,
	.listxattr = ecryptfs_listxattr,
	.removexattr = ecryptfs_removexattr
};

const struct inode_operations ecryptfs_main_iops = {
	.permission = ecryptfs_permission,
	.setattr = ecryptfs_setattr,
	.getattr = ecryptfs_getattr,
	.setxattr = ecryptfs_setxattr,
	.getxattr = ecryptfs_getxattr,
	.listxattr = ecryptfs_listxattr,
	.removexattr = ecryptfs_removexattr
};
