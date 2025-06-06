// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2020 iXsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/dmu.h>
#include <sys/dmu_impl.h>
#include <sys/dmu_recv.h>
#include <sys/dmu_tx.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/zfs_context.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/zfs_ioctl.h>
#include <sys/zap.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_file.h>
#include <sys/buf.h>
#include <sys/stat.h>

int
zfs_file_open(const char *path, int flags, int mode, zfs_file_t **fpp)
{
	struct thread *td;
	struct vnode *vp;
	struct file *fp;
	struct nameidata nd;
	int error;

	td = curthread;
	pwd_ensure_dirs();

	KASSERT((flags & (O_EXEC | O_PATH)) == 0,
	    ("invalid flags: 0x%x", flags));
	KASSERT((flags & O_ACCMODE) != O_ACCMODE,
	    ("invalid flags: 0x%x", flags));
	flags = FFLAGS(flags);

	error = falloc_noinstall(td, &fp);
	if (error != 0) {
		return (error);
	}
	fp->f_flag = flags & FMASK;

#if __FreeBSD_version >= 1400043
	NDINIT(&nd, LOOKUP, FOLLOW, UIO_SYSSPACE, path);
#else
	NDINIT(&nd, LOOKUP, FOLLOW, UIO_SYSSPACE, path, td);
#endif
	error = vn_open(&nd, &flags, mode, fp);
	if (error != 0) {
		falloc_abort(td, fp);
		return (SET_ERROR(error));
	}
	NDFREE_PNBUF(&nd);
	vp = nd.ni_vp;
	fp->f_vnode = vp;
	if (fp->f_ops == &badfileops) {
		finit_vnode(fp, flags, NULL, &vnops);
	}
	VOP_UNLOCK(vp);
	if (vp->v_type != VREG) {
		zfs_file_close(fp);
		return (SET_ERROR(EACCES));
	}

	if (flags & O_TRUNC) {
		error = fo_truncate(fp, 0, td->td_ucred, td);
		if (error != 0) {
			zfs_file_close(fp);
			return (SET_ERROR(error));
		}
	}

	*fpp = fp;

	return (0);
}

void
zfs_file_close(zfs_file_t *fp)
{
	fdrop(fp, curthread);
}

static int
zfs_file_write_impl(zfs_file_t *fp, const void *buf, size_t count, loff_t *offp,
    ssize_t *resid)
{
	ssize_t rc;
	struct uio auio;
	struct thread *td;
	struct iovec aiov;

	td = curthread;
	aiov.iov_base = (void *)(uintptr_t)buf;
	aiov.iov_len = count;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_resid = count;
	auio.uio_rw = UIO_WRITE;
	auio.uio_td = td;
	auio.uio_offset = *offp;

	if ((fp->f_flag & FWRITE) == 0)
		return (SET_ERROR(EBADF));

	if (fp->f_type == DTYPE_VNODE)
		bwillwrite();

	rc = fo_write(fp, &auio, td->td_ucred, FOF_OFFSET, td);
	if (rc)
		return (SET_ERROR(rc));
	if (resid)
		*resid = auio.uio_resid;
	else if (auio.uio_resid)
		return (SET_ERROR(EIO));
	*offp += count - auio.uio_resid;
	return (rc);
}

int
zfs_file_write(zfs_file_t *fp, const void *buf, size_t count, ssize_t *resid)
{
	loff_t off = fp->f_offset;
	ssize_t rc;

	rc = zfs_file_write_impl(fp, buf, count, &off, resid);
	if (rc == 0)
		fp->f_offset = off;

	return (SET_ERROR(rc));
}

int
zfs_file_pwrite(zfs_file_t *fp, const void *buf, size_t count, loff_t off,
    ssize_t *resid)
{
	return (zfs_file_write_impl(fp, buf, count, &off, resid));
}

static int
zfs_file_read_impl(zfs_file_t *fp, void *buf, size_t count, loff_t *offp,
    ssize_t *resid)
{
	ssize_t rc;
	struct uio auio;
	struct thread *td;
	struct iovec aiov;

	td = curthread;
	aiov.iov_base = (void *)(uintptr_t)buf;
	aiov.iov_len = count;
	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_resid = count;
	auio.uio_rw = UIO_READ;
	auio.uio_td = td;
	auio.uio_offset = *offp;

	if ((fp->f_flag & FREAD) == 0)
		return (SET_ERROR(EBADF));

	rc = fo_read(fp, &auio, td->td_ucred, FOF_OFFSET, td);
	if (rc)
		return (SET_ERROR(rc));
	if (resid)
		*resid = auio.uio_resid;
	*offp += count - auio.uio_resid;
	return (SET_ERROR(0));
}

int
zfs_file_read(zfs_file_t *fp, void *buf, size_t count, ssize_t *resid)
{
	loff_t off = fp->f_offset;
	ssize_t rc;

	rc = zfs_file_read_impl(fp, buf, count, &off, resid);
	if (rc == 0)
		fp->f_offset = off;
	return (rc);
}

int
zfs_file_pread(zfs_file_t *fp, void *buf, size_t count, loff_t off,
    ssize_t *resid)
{
	return (zfs_file_read_impl(fp, buf, count, &off, resid));
}

int
zfs_file_seek(zfs_file_t *fp, loff_t *offp, int whence)
{
	int rc;
	struct thread *td;

	td = curthread;
	if ((fp->f_ops->fo_flags & DFLAG_SEEKABLE) == 0)
		return (SET_ERROR(ESPIPE));
	rc = fo_seek(fp, *offp, whence, td);
	if (rc == 0)
		*offp = td->td_uretoff.tdu_off;
	return (SET_ERROR(rc));
}

int
zfs_file_getattr(zfs_file_t *fp, zfs_file_attr_t *zfattr)
{
	struct thread *td;
	struct stat sb;
	int rc;

	td = curthread;

#if __FreeBSD_version < 1400037
	rc = fo_stat(fp, &sb, td->td_ucred, td);
#else
	rc = fo_stat(fp, &sb, td->td_ucred);
#endif
	if (rc)
		return (SET_ERROR(rc));
	zfattr->zfa_size = sb.st_size;
	zfattr->zfa_mode = sb.st_mode;

	return (0);
}

static __inline int
zfs_vop_fsync(vnode_t *vp)
{
	struct mount *mp;
	int error;

#if __FreeBSD_version < 1400068
	if ((error = vn_start_write(vp, &mp, V_WAIT | PCATCH)) != 0)
#else
	if ((error = vn_start_write(vp, &mp, V_WAIT | V_PCATCH)) != 0)
#endif
		goto drop;
	vn_lock(vp, LK_EXCLUSIVE | LK_RETRY);
	error = VOP_FSYNC(vp, MNT_WAIT, curthread);
	VOP_UNLOCK(vp);
	vn_finished_write(mp);
drop:
	return (SET_ERROR(error));
}

int
zfs_file_fsync(zfs_file_t *fp, int flags)
{
	if (fp->f_type != DTYPE_VNODE)
		return (EINVAL);

	return (zfs_vop_fsync(fp->f_vnode));
}

/*
 * deallocate - zero and/or deallocate file storage
 *
 * fp - file pointer
 * offset - offset to start zeroing or deallocating
 * len - length to zero or deallocate
 */
int
zfs_file_deallocate(zfs_file_t *fp, loff_t offset, loff_t len)
{
	int rc;
#if __FreeBSD_version >= 1400029
	struct thread *td;

	td = curthread;
	rc = fo_fspacectl(fp, SPACECTL_DEALLOC, &offset, &len, 0,
	    td->td_ucred, td);
#else
	(void) fp, (void) offset, (void) len;
	rc = EOPNOTSUPP;
#endif
	if (rc)
		return (SET_ERROR(rc));
	return (0);
}

zfs_file_t *
zfs_file_get(int fd)
{
	struct file *fp;

	if (fget(curthread, fd, &cap_no_rights, &fp))
		return (NULL);

	return (fp);
}

void
zfs_file_put(zfs_file_t *fp)
{
	zfs_file_close(fp);
}

loff_t
zfs_file_off(zfs_file_t *fp)
{
	return (fp->f_offset);
}

void *
zfs_file_private(zfs_file_t *fp)
{
	file_t *tmpfp;
	void *data;
	int error;

	tmpfp = curthread->td_fpop;
	curthread->td_fpop = fp;
	error = devfs_get_cdevpriv(&data);
	curthread->td_fpop = tmpfp;
	if (error != 0)
		return (NULL);
	return (data);
}

int
zfs_file_unlink(const char *fnamep)
{
	zfs_uio_seg_t seg = UIO_SYSSPACE;
	int rc;

	rc = kern_funlinkat(curthread, AT_FDCWD, fnamep, FD_NONE, seg, 0, 0);
	return (SET_ERROR(rc));
}
