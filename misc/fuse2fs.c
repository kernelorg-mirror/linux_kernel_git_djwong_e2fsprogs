/*
 * fuse2fs.c - FUSE server for e2fsprogs.
 *
 * Copyright (C) 2014 Oracle.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include <pthread.h>
#ifdef __linux__
# include <linux/fs.h>
# include <linux/falloc.h>
# include <linux/xattr.h>
# include <sys/prctl.h>
#endif
#ifdef HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <ctype.h>
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#ifdef __SET_FOB_FOR_FUSE
# error Do not set magic value __SET_FOB_FOR_FUSE!!!!
#endif
#ifndef _FILE_OFFSET_BITS
/*
 * Old versions of libfuse (e.g. Debian 2.9.9 package) required that the build
 * system set _FILE_OFFSET_BITS explicitly, even if doing so isn't required to
 * get a 64-bit off_t.  AC_SYS_LARGEFILE doesn't set any _FILE_OFFSET_BITS if
 * it's not required (such as on aarch64), so we must inject it here.
 */
# define __SET_FOB_FOR_FUSE
# define _FILE_OFFSET_BITS 64
#endif /* _FILE_OFFSET_BITS */
#include <fuse.h>
#include <fuse_lowlevel.h>
#ifdef __SET_FOB_FOR_FUSE
# undef _FILE_OFFSET_BITS
#endif /* __SET_FOB_FOR_FUSE */
#include <inttypes.h>
#include "ext2fs/ext2fs.h"
#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fsP.h"
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
# define FUSE_PLATFORM_OPTS	""
#else
# ifdef __linux__
#  define FUSE_PLATFORM_OPTS	",use_ino,big_writes"
# else
#  define FUSE_PLATFORM_OPTS	",use_ino"
# endif
#endif

#include "../version.h"
#include "uuid/uuid.h"
#include "e2p/e2p.h"

#ifdef ENABLE_NLS
#include <libintl.h>
#include <locale.h>
#define _(a) (gettext(a))
#ifdef gettext_noop
#define N_(a) gettext_noop(a)
#else
#define N_(a) (a)
#endif
#define P_(singular, plural, n) (ngettext(singular, plural, n))
#ifndef NLS_CAT_NAME
#define NLS_CAT_NAME "e2fsprogs"
#endif
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif
#else
#define _(a) (a)
#define N_(a) a
#define P_(singular, plural, n) ((n) == 1 ? (singular) : (plural))
#endif

#ifndef XATTR_NAME_POSIX_ACL_DEFAULT
#define XATTR_NAME_POSIX_ACL_DEFAULT "posix_acl_default"
#endif
#ifndef XATTR_SECURITY_PREFIX
#define XATTR_SECURITY_PREFIX "security."
#define XATTR_SECURITY_PREFIX_LEN (sizeof (XATTR_SECURITY_PREFIX) - 1)
#endif

/*
 * Linux and MacOS implement the setxattr(2) interface, which defines
 * XATTR_CREATE and XATTR_REPLACE.  However, FreeBSD uses
 * extattr_set_file(2), which does not have a flags or options
 * parameter, and does not define XATTR_CREATE and XATTR_REPLACE.
 */
#ifndef XATTR_CREATE
#define XATTR_CREATE 0
#endif

#ifndef XATTR_REPLACE
#define XATTR_REPLACE 0
#endif

#if !defined(EUCLEAN)
#if !defined(EBADMSG)
#define EUCLEAN EBADMSG
#elif !defined(EPROTO)
#define EUCLEAN EPROTO
#else
#define EUCLEAN EIO
#endif
#endif /* !defined(EUCLEAN) */

#if !defined(ENODATA)
#ifdef ENOATTR
#define ENODATA ENOATTR
#else
#define ENODATA ENOENT
#endif
#endif /* !defined(ENODATA) */

static inline uint64_t round_up(uint64_t b, unsigned int align)
{
	unsigned int m;

	if (align == 0)
		return b;
	m = b % align;
	if (m)
		b += align - m;
	return b;
}

static inline uint64_t round_down(uint64_t b, unsigned int align)
{
	unsigned int m;

	if (align == 0)
		return b;
	m = b % align;
	return b - m;
}

#define max(a, b)	((a) > (b) ? (a) : (b))
#define min(x, y)	((x) < (y) ? (y) : (x))

#define dbg_printf(fuse2fs, format, ...) \
	while ((fuse2fs)->debug) { \
		printf("FUSE2FS (%s): " format, (fuse2fs)->shortdev, ##__VA_ARGS__); \
		fflush(stdout); \
		break; \
	}

#define log_printf(fuse2fs, format, ...) \
	do { \
		printf("FUSE2FS (%s): " format, (fuse2fs)->shortdev, ##__VA_ARGS__); \
		fflush(stdout); \
	} while (0)

#define err_printf(fuse2fs, format, ...) \
	do { \
		fprintf(stderr, "FUSE2FS (%s): " format, (fuse2fs)->shortdev, ##__VA_ARGS__); \
		fflush(stderr); \
	} while (0)

#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
# ifdef _IOR
#  ifdef _IOW
#   define SUPPORT_I_FLAGS
#  endif
# endif
#endif

#ifdef FALLOC_FL_KEEP_SIZE
# define FL_KEEP_SIZE_FLAG FALLOC_FL_KEEP_SIZE
# define SUPPORT_FALLOCATE
#else
# define FL_KEEP_SIZE_FLAG (0)
#endif

#ifdef FALLOC_FL_PUNCH_HOLE
# define FL_PUNCH_HOLE_FLAG FALLOC_FL_PUNCH_HOLE
#else
# define FL_PUNCH_HOLE_FLAG (0)
#endif

#ifdef FALLOC_FL_ZERO_RANGE
# define FL_ZERO_RANGE_FLAG FALLOC_FL_ZERO_RANGE
#else
# define FL_ZERO_RANGE_FLAG (0)
#endif

#ifndef NSEC_PER_SEC
# define NSEC_PER_SEC	(1000000000L)
#endif

errcode_t ext2fs_run_ext3_journal(ext2_filsys *fs);

const char *err_shortdev;

#ifdef CONFIG_JBD_DEBUG		/* Enabled by configure --enable-jbd-debug */
int journal_enable_debug = -1;
#endif

/*
 * ext2_file_t contains a struct inode, so we can't leave files open.
 * Use this as a proxy instead.
 */
#define FUSE2FS_FILE_MAGIC	(0xEF53DEAFUL)
struct fuse2fs_file_handle {
	unsigned long magic;
	ext2_ino_t ino;
	int open_flags;
};

enum fuse2fs_opstate {
	F2OP_READONLY,
	F2OP_WRITABLE,
	F2OP_SHUTDOWN,
};

enum fuse2fs_feature_toggle {
	FT_DISABLE,
	FT_ENABLE,
	FT_DEFAULT,
};

#ifdef HAVE_FUSE_IOMAP
enum fuse2fs_iomap_state {
	IOMAP_DISABLED,
	IOMAP_UNKNOWN,
	IOMAP_ENABLED,
	IOMAP_FILEIO,	/* enabled and does all file data block IO */
};
#endif

/* Main program context */
#define FUSE2FS_MAGIC		(0xEF53DEADUL)
struct fuse2fs {
	unsigned long magic;
	ext2_filsys fs;
	pthread_mutex_t bfl;
	char *device;
	char *shortdev;
	uint8_t ro;
	uint8_t debug;
	uint8_t no_default_opts;
	uint8_t errors_behavior; /* actually an enum */
	uint8_t minixdf;
	uint8_t fakeroot;
	uint8_t alloc_all_blocks;
	uint8_t norecovery;
	uint8_t kernel;
	uint8_t directio;
	uint8_t acl;
	uint8_t dirsync;
	uint8_t unmount_in_destroy;
	uint8_t noblkdev;
	uint8_t can_hardlink;
	uint8_t iomap_passthrough_options;
	uint8_t write_gdt_on_destroy;

	enum fuse2fs_opstate opstate;
	int blocklog;
#ifdef HAVE_FUSE_IOMAP
	enum fuse2fs_feature_toggle iomap_want;
	enum fuse2fs_iomap_state iomap_state;
	uint32_t iomap_dev;
#endif
	unsigned int blockmask;
	int retcode;
	unsigned long offset;
	unsigned int next_generation;
	unsigned long long cache_size;
	char *lockfile;
};

#define FUSE2FS_CHECK_HANDLE(ff, fh) \
	do { \
		if ((fh) == NULL || (fh)->magic != FUSE2FS_FILE_MAGIC) { \
			fprintf(stderr, \
				"FUSE2FS: Corrupt in-memory file handle at %s:%d!\n", \
				__func__, __LINE__); \
			fflush(stderr); \
			return -EUCLEAN; \
		} \
	} while (0)

#define __FUSE2FS_CHECK_CONTEXT(ff, retcode, shutcode) \
	do { \
		if ((ff) == NULL || (ff)->magic != FUSE2FS_MAGIC) { \
			fprintf(stderr, \
				"FUSE2FS: Corrupt in-memory data at %s:%d!\n", \
				__func__, __LINE__); \
			fflush(stderr); \
			retcode; \
		} \
		if ((ff) != NULL && (ff)->opstate == F2OP_SHUTDOWN) { \
			shutcode; \
		} \
	} while (0)

#define FUSE2FS_CHECK_CONTEXT(ff) \
	__FUSE2FS_CHECK_CONTEXT((ff), return -EUCLEAN, return -EIO)
#define FUSE2FS_CHECK_CONTEXT_NORET(ff) \
	__FUSE2FS_CHECK_CONTEXT((ff), return, return)
#define FUSE2FS_CHECK_CONTEXT_NULL(ff) \
	__FUSE2FS_CHECK_CONTEXT((ff), return NULL, return NULL)

static int __translate_error(ext2_filsys fs, ext2_ino_t ino, errcode_t err,
			     const char *func, int line);
#define translate_error(fs, ino, err) __translate_error((fs), (ino), (err), \
			__func__, __LINE__)

/* for macosx */
#ifndef W_OK
#  define W_OK 2
#endif

#ifndef R_OK
#  define R_OK 4
#endif

static inline int u_log2(unsigned int arg)
{
	int	l = 0;

	arg >>= 1;
	while (arg) {
		l++;
		arg >>= 1;
	}
	return l;
}

static inline blk64_t FUSE2FS_B_TO_FSBT(const struct fuse2fs *ff, off_t pos)
{
	return pos >> ff->blocklog;
}

static inline blk64_t FUSE2FS_B_TO_FSB(const struct fuse2fs *ff, off_t pos)
{
	return (pos + ff->blockmask) >> ff->blocklog;
}

static inline unsigned int FUSE2FS_OFF_IN_FSB(const struct fuse2fs *ff,
					      off_t pos)
{
	return pos & ff->blockmask;
}

static inline off_t FUSE2FS_FSB_TO_B(const struct fuse2fs *ff, blk64_t bno)
{
	return bno << ff->blocklog;
}

#define EXT4_EPOCH_BITS 2
#define EXT4_EPOCH_MASK ((1 << EXT4_EPOCH_BITS) - 1)
#define EXT4_NSEC_MASK  (~0UL << EXT4_EPOCH_BITS)

/*
 * Extended fields will fit into an inode if the filesystem was formatted
 * with large inodes (-I 256 or larger) and there are not currently any EAs
 * consuming all of the available space. For new inodes we always reserve
 * enough space for the kernel's known extended fields, but for inodes
 * created with an old kernel this might not have been the case. None of
 * the extended inode fields is critical for correct filesystem operation.
 * This macro checks if a certain field fits in the inode. Note that
 * inode-size = GOOD_OLD_INODE_SIZE + i_extra_isize
 */
#define EXT4_FITS_IN_INODE(ext4_inode, field)		\
	((offsetof(typeof(*ext4_inode), field) +	\
	  sizeof((ext4_inode)->field))			\
	 <= ((size_t) EXT2_GOOD_OLD_INODE_SIZE +		\
	    (ext4_inode)->i_extra_isize))		\

static inline __u32 ext4_encode_extra_time(const struct timespec *time)
{
	__u32 extra = sizeof(time->tv_sec) > 4 ?
			((time->tv_sec - (__s32)time->tv_sec) >> 32) &
			EXT4_EPOCH_MASK : 0;
	return extra | (time->tv_nsec << EXT4_EPOCH_BITS);
}

static inline void ext4_decode_extra_time(struct timespec *time, __u32 extra)
{
	if (sizeof(time->tv_sec) > 4 && (extra & EXT4_EPOCH_MASK)) {
		__u64 extra_bits = extra & EXT4_EPOCH_MASK;
		/*
		 * Prior to kernel 3.14?, we had a broken decode function,
		 * wherein we effectively did this:
		 * if (extra_bits == 3)
		 *     extra_bits = 0;
		 */
		time->tv_sec += extra_bits << 32;
	}
	time->tv_nsec = ((extra) & EXT4_NSEC_MASK) >> EXT4_EPOCH_BITS;
}

#define EXT4_CLAMP_TIMESTAMP(xtime, timespec, raw_inode)		       \
do {									       \
	if ((timespec)->tv_sec < EXT4_TIMESTAMP_MIN)			       \
		(timespec)->tv_sec = EXT4_TIMESTAMP_MIN;		       \
	if ((timespec)->tv_sec < EXT4_TIMESTAMP_MIN)			       \
		(timespec)->tv_sec = EXT4_TIMESTAMP_MIN;		       \
									       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime ## _extra)) {		       \
		if ((timespec)->tv_sec > EXT4_EXTRA_TIMESTAMP_MAX)	       \
			(timespec)->tv_sec = EXT4_EXTRA_TIMESTAMP_MAX;	       \
	} else {							       \
		if ((timespec)->tv_sec > EXT4_NON_EXTRA_TIMESTAMP_MAX)	       \
			(timespec)->tv_sec = EXT4_NON_EXTRA_TIMESTAMP_MAX;     \
	}								       \
} while (0)

#define EXT4_INODE_SET_XTIME(xtime, timespec, raw_inode)		       \
do {									       \
	typeof(*(timespec)) _ts = *(timespec);				       \
									       \
	EXT4_CLAMP_TIMESTAMP(xtime, &_ts, raw_inode);			       \
	(raw_inode)->xtime = _ts.tv_sec;				       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime ## _extra))		       \
		(raw_inode)->xtime ## _extra =				       \
				ext4_encode_extra_time(&_ts);		       \
} while (0)

#define EXT4_EINODE_SET_XTIME(xtime, timespec, raw_inode)		       \
do {									       \
	typeof(*(timespec)) _ts = *(timespec);				       \
									       \
	EXT4_CLAMP_TIMESTAMP(xtime, &_ts, raw_inode);			       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime))			       \
		(raw_inode)->xtime = _ts.tv_sec;			       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime ## _extra))		       \
		(raw_inode)->xtime ## _extra =				       \
				ext4_encode_extra_time(&_ts);		       \
} while (0)

#define EXT4_INODE_GET_XTIME(xtime, timespec, raw_inode)		       \
do {									       \
	(timespec)->tv_sec = (signed)((raw_inode)->xtime);		       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime ## _extra))		       \
		ext4_decode_extra_time((timespec),			       \
				       (raw_inode)->xtime ## _extra);	       \
	else								       \
		(timespec)->tv_nsec = 0;				       \
} while (0)

#define EXT4_EINODE_GET_XTIME(xtime, timespec, raw_inode)		       \
do {									       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime))			       \
		(timespec)->tv_sec =					       \
			(signed)((raw_inode)->xtime);			       \
	if (EXT4_FITS_IN_INODE(raw_inode, xtime ## _extra))		       \
		ext4_decode_extra_time((timespec),			       \
				       raw_inode->xtime ## _extra);	       \
	else								       \
		(timespec)->tv_nsec = 0;				       \
} while (0)

static inline errcode_t fuse2fs_read_inode(ext2_filsys fs, ext2_ino_t ino,
					   struct ext2_inode_large *inode)
{
	memset(inode, 0, sizeof(*inode));
	return ext2fs_read_inode_full(fs, ino, EXT2_INODE(inode),
				      sizeof(*inode));
}

static inline errcode_t fuse2fs_write_inode(ext2_filsys fs, ext2_ino_t ino,
					    struct ext2_inode_large *inode)
{
	return ext2fs_write_inode_full(fs, ino, EXT2_INODE(inode),
				       sizeof(*inode));
}

static inline ext2_filsys fuse2fs_start(struct fuse2fs *ff)
{
	pthread_mutex_lock(&ff->bfl);
	return ff->fs;
}

static inline void __fuse2fs_finish(struct fuse2fs *ff, int ret,
				    const char *func)
{
	if (ret)
		dbg_printf(ff, "%s: libfuse ret=%d\n", func, ret);
	pthread_mutex_unlock(&ff->bfl);
}
#define fuse2fs_finish(ff, ret) __fuse2fs_finish((ff), (ret), __func__)

#ifdef HAVE_FUSE_IOMAP
static int fuse2fs_iomap_enabled(const struct fuse2fs *ff)
{
	return ff->iomap_state >= IOMAP_ENABLED;
}

static int fuse2fs_iomap_does_fileio(const struct fuse2fs *ff)
{
	return ff->iomap_state == IOMAP_FILEIO;
}
#else
# define fuse2fs_iomap_enabled(...)	(0)
# define fuse2fs_iomap_does_fileio(...)	(0)
#endif

static inline void fuse2fs_dump_extents(struct fuse2fs *ff, ext2_ino_t ino,
					struct ext2_inode_large *inode,
					const char *why)
{
	ext2_filsys fs = ff->fs;
	unsigned int nr = 0;
	blk64_t blockcount = 0;
	struct ext2_inode_large xinode;
	struct ext2fs_extent extent;
	ext2_extent_handle_t extents;
	int op = EXT2_EXTENT_ROOT;
	errcode_t retval;

	if (!inode) {
		inode = &xinode;

		retval = fuse2fs_read_inode(fs, ino, inode);
		if (retval) {
			com_err(__func__, retval, _("reading ino %u"), ino);
			return;
		}
	}

	if (!(inode->i_flags & EXT4_EXTENTS_FL))
		return;

	printf("%s: %s ino=%u isize %llu iblocks %llu\n", __func__, why, ino,
	       EXT2_I_SIZE(inode),
	       (ext2fs_get_stat_i_blocks(fs, EXT2_INODE(inode)) * 512) /
	        fs->blocksize);
	fflush(stdout);

	retval = ext2fs_extent_open(fs, ino, &extents);
	if (retval) {
		com_err(__func__, retval, _("opening extents of ino \"%u\""),
			ino);
		return;
	}

	while ((retval = ext2fs_extent_get(extents, op, &extent)) == 0) {
		op = EXT2_EXTENT_NEXT;

		if (extent.e_flags & EXT2_EXTENT_FLAGS_SECOND_VISIT)
			continue;

		printf("[%u]: %s ino=%u lblk 0x%llx pblk 0x%llx len 0x%x flags 0x%x\n",
		       nr++, why, ino, extent.e_lblk, extent.e_pblk,
		       extent.e_len, extent.e_flags);
		fflush(stdout);
		if (extent.e_flags & EXT2_EXTENT_FLAGS_LEAF)
			blockcount += extent.e_len;
		else
			blockcount++;
	}
	if (retval == EXT2_ET_EXTENT_NO_NEXT)
		retval = 0;
	if (retval) {
		com_err(__func__, retval, ("getting extents of ino %u"),
			ino);
	}
	if (inode->i_file_acl)
		blockcount++;
	printf("%s: %s sum(e_len) %llu\n", __func__, why, blockcount);
	fflush(stdout);

	ext2fs_extent_free(extents);
}

static void fuse2fs_get_now(struct fuse2fs *ff, struct timespec *now)
{
#ifdef CLOCK_REALTIME_COARSE
	/*
	 * In iomap mode, the kernel is responsible for maintaining timestamps
	 * because file writes don't upcall to fuse2fs.  The kernel's predicate
	 * for deciding if [cm]time should be updated bases its decisions off
	 * [cm]time being an exact match for the coarse clock (instead of
	 * checking that [cm]time < coarse_clock) which means that fuse2fs
	 * setting a fine-grained timestamp that is slightly ahead of the
	 * coarse clock can result in timestamps appearing to go backwards.
	 * generic/423 doesn't like seeing btime > ctime from statx, so we'll
	 * use the coarse clock in iomap mode.
	 */
	if (fuse2fs_iomap_does_fileio(ff) &&
	    !clock_gettime(CLOCK_REALTIME_COARSE, now))
		return;
#endif
#ifdef CLOCK_REALTIME
	if (!clock_gettime(CLOCK_REALTIME, now))
		return;
#endif

	now->tv_sec = time(NULL);
	now->tv_nsec = 0;
}

static void increment_version(struct ext2_inode_large *inode)
{
	__u64 ver;

	ver = inode->osd1.linux1.l_i_version;
	if (EXT4_FITS_IN_INODE(inode, i_version_hi))
		ver |= (__u64)inode->i_version_hi << 32;
	ver++;
	inode->osd1.linux1.l_i_version = ver;
	if (EXT4_FITS_IN_INODE(inode, i_version_hi))
		inode->i_version_hi = ver >> 32;
}

static void fuse2fs_init_timestamps(struct fuse2fs *ff, ext2_ino_t ino,
				    struct ext2_inode_large *inode)
{
	struct timespec now;

	fuse2fs_get_now(ff, &now);
	EXT4_INODE_SET_XTIME(i_atime, &now, inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, inode);
	EXT4_INODE_SET_XTIME(i_mtime, &now, inode);
	EXT4_EINODE_SET_XTIME(i_crtime, &now, inode);
	increment_version(inode);

	dbg_printf(ff, "%s: ino=%u time %ld:%lu\n", __func__, ino, now.tv_sec,
		   now.tv_nsec);
}

static int fuse2fs_update_ctime(struct fuse2fs *ff, ext2_ino_t ino,
				struct ext2_inode_large *pinode)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct timespec now;
	struct ext2_inode_large inode;

	fuse2fs_get_now(ff, &now);

	/* If user already has a inode buffer, just update that */
	if (pinode) {
		increment_version(pinode);
		EXT4_INODE_SET_XTIME(i_ctime, &now, pinode);

		dbg_printf(ff, "%s: ino=%u ctime %ld:%lu\n", __func__, ino,
			   now.tv_sec, now.tv_nsec);

		return 0;
	}

	/* Otherwise we have to read-modify-write the inode */
	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	increment_version(&inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, &inode);

	dbg_printf(ff, "%s: ino=%u ctime %ld:%lu\n", __func__, ino,
		   now.tv_sec, now.tv_nsec);

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse2fs_update_atime(struct fuse2fs *ff, ext2_ino_t ino)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode, *pinode;
	struct timespec atime, mtime, now;
	double datime, dmtime, dnow;

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	pinode = &inode;
	EXT4_INODE_GET_XTIME(i_atime, &atime, pinode);
	EXT4_INODE_GET_XTIME(i_mtime, &mtime, pinode);
	fuse2fs_get_now(ff, &now);

	datime = atime.tv_sec + ((double)atime.tv_nsec / NSEC_PER_SEC);
	dmtime = mtime.tv_sec + ((double)mtime.tv_nsec / NSEC_PER_SEC);
	dnow = now.tv_sec + ((double)now.tv_nsec / NSEC_PER_SEC);

	dbg_printf(ff, "%s: ino=%u atime %ld:%lu mtime %ld:%lu now %ld:%lu\n",
		   __func__, ino, atime.tv_sec, atime.tv_nsec, mtime.tv_sec,
		   mtime.tv_nsec, now.tv_sec, now.tv_nsec);

	/*
	 * If atime is newer than mtime and atime hasn't been updated in thirty
	 * seconds, skip the atime update.  Same idea as Linux "relatime".  Use
	 * doubles to account for nanosecond resolution.
	 */
	if (datime >= dmtime && datime >= dnow - 30)
		return 0;
	EXT4_INODE_SET_XTIME(i_atime, &now, &inode);

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse2fs_update_mtime(struct fuse2fs *ff, ext2_ino_t ino,
				struct ext2_inode_large *pinode)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	struct timespec now;

	if (pinode) {
		fuse2fs_get_now(ff, &now);
		EXT4_INODE_SET_XTIME(i_mtime, &now, pinode);
		EXT4_INODE_SET_XTIME(i_ctime, &now, pinode);
		increment_version(pinode);

		dbg_printf(ff, "%s: ino=%u mtime/ctime %ld:%lu\n",
			   __func__, ino, now.tv_sec, now.tv_nsec);

		return 0;
	}

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	fuse2fs_get_now(ff, &now);
	EXT4_INODE_SET_XTIME(i_mtime, &now, &inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, &inode);
	increment_version(&inode);

	dbg_printf(ff, "%s: ino=%u mtime/ctime %ld:%lu\n",
		   __func__, ino, now.tv_sec, now.tv_nsec);

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int ext2_file_type(unsigned int mode)
{
	if (LINUX_S_ISREG(mode))
		return EXT2_FT_REG_FILE;

	if (LINUX_S_ISDIR(mode))
		return EXT2_FT_DIR;

	if (LINUX_S_ISCHR(mode))
		return EXT2_FT_CHRDEV;

	if (LINUX_S_ISBLK(mode))
		return EXT2_FT_BLKDEV;

	if (LINUX_S_ISLNK(mode))
		return EXT2_FT_SYMLINK;

	if (LINUX_S_ISFIFO(mode))
		return EXT2_FT_FIFO;

	if (LINUX_S_ISSOCK(mode))
		return EXT2_FT_SOCK;

	return 0;
}

static int fs_can_allocate(struct fuse2fs *ff, blk64_t num)
{
	ext2_filsys fs = ff->fs;
	blk64_t reserved;

	dbg_printf(ff, "%s: Asking for %llu; alloc_all=%d total=%llu free=%llu "
		   "rsvd=%llu\n", __func__, num, ff->alloc_all_blocks,
		   ext2fs_blocks_count(fs->super),
		   ext2fs_free_blocks_count(fs->super),
		   ext2fs_r_blocks_count(fs->super));
	if (num > ext2fs_blocks_count(fs->super))
		return 0;

	if (ff->alloc_all_blocks)
		return 1;

	/*
	 * Different meaning for r_blocks -- libext2fs has bugs where the FS
	 * can get corrupted if it totally runs out of blocks.  Avoid this
	 * by refusing to allocate any of the reserve blocks to anybody.
	 */
	reserved = ext2fs_r_blocks_count(fs->super);
	if (reserved == 0)
		reserved = ext2fs_blocks_count(fs->super) / 10;
	return ext2fs_free_blocks_count(fs->super) > reserved + num;
}

static int fuse2fs_is_writeable(struct fuse2fs *ff)
{
	return ff->opstate == F2OP_WRITABLE &&
		(ff->fs->super->s_error_count == 0);
}

static inline int is_superuser(struct fuse2fs *ff, struct fuse_context *ctxt)
{
	if (ff->fakeroot)
		return 1;
	return ctxt->uid == 0;
}

static inline int want_check_owner(struct fuse2fs *ff,
				   struct fuse_context *ctxt)
{
	/*
	 * The kernel is responsible for access control, so we allow anything
	 * that the superuser can do.
	 */
	if (ff->kernel)
		return 0;
	return !is_superuser(ff, ctxt);
}

/* Test for append permission */
#define A_OK	16

static int check_iflags_access(struct fuse2fs *ff, ext2_ino_t ino,
			       const struct ext2_inode *inode, int mask)
{
	EXT2FS_BUILD_BUG_ON((A_OK & (R_OK | W_OK | X_OK | F_OK)) != 0);

	/* no writing or metadata changes to read-only or broken fs */
	if ((mask & (W_OK | A_OK)) && !fuse2fs_is_writeable(ff))
		return -EROFS;

	dbg_printf(ff, "access ino=%d mask=e%s%s%s%s iflags=0x%x\n",
		   ino,
		   (mask & R_OK ? "r" : ""),
		   (mask & W_OK ? "w" : ""),
		   (mask & X_OK ? "x" : ""),
		   (mask & A_OK ? "a" : ""),
		   inode->i_flags);

	/* is immutable? */
	if ((mask & W_OK) &&
	    (inode->i_flags & EXT2_IMMUTABLE_FL))
		return -EPERM;

	/* is append-only? */
	if ((inode->i_flags & EXT2_APPEND_FL) && (mask & W_OK) && !(mask & A_OK))
		return -EPERM;

	return 0;
}

static int check_inum_access(struct fuse2fs *ff, ext2_ino_t ino, int mask)
{
	struct fuse_context *ctxt = fuse_get_context();
	ext2_filsys fs = ff->fs;
	struct ext2_inode inode;
	mode_t perms;
	errcode_t err;
	int ret;

	/* no writing to read-only or broken fs */
	if ((mask & (W_OK | A_OK)) && !fuse2fs_is_writeable(ff))
		return -EROFS;

	err = ext2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);
	perms = inode.i_mode & 0777;

	dbg_printf(ff, "access ino=%d mask=e%s%s%s%s perms=0%o iflags=0x%x "
		   "fuid=%d fgid=%d uid=%d gid=%d\n", ino,
		   (mask & R_OK ? "r" : ""),
		   (mask & W_OK ? "w" : ""),
		   (mask & X_OK ? "x" : ""),
		   (mask & A_OK ? "a" : ""),
		   perms, inode.i_flags,
		   inode_uid(inode), inode_gid(inode),
		   ctxt->uid, ctxt->gid);

	/* existence check */
	if (mask == 0)
		return 0;

	ret = check_iflags_access(ff, ino, &inode, mask);
	if (ret)
		return ret;

	/* If kernel is responsible for mode and acl checks, we're done. */
	if (ff->kernel)
		return 0;

	/* Figure out what root's allowed to do */
	if (is_superuser(ff, ctxt)) {
		/* Non-file access always ok */
		if (!LINUX_S_ISREG(inode.i_mode))
			return 0;

		/* R/W access to a file always ok */
		if (!(mask & X_OK))
			return 0;

		/* X access to a file ok if a user/group/other can X */
		if (perms & 0111)
			return 0;

		/* Trying to execute a file that's not executable. BZZT! */
		return -EACCES;
	}

	/* Remove the O_APPEND flag before testing permissions */
	mask &= ~A_OK;

	/* allow owner, if perms match */
	if (inode_uid(inode) == ctxt->uid) {
		if ((mask & (perms >> 6)) == mask)
			return 0;
		return -EACCES;
	}

	/* allow group, if perms match */
	if (inode_gid(inode) == ctxt->gid) {
		if ((mask & (perms >> 3)) == mask)
			return 0;
		return -EACCES;
	}

	/* otherwise check other */
	if ((mask & perms) == mask)
		return 0;
	return -EACCES;
}

static errcode_t fuse2fs_acquire_lockfile(struct fuse2fs *ff)
{
	char *resolved;
	int lockfd;
	errcode_t err;

	lockfd = open(ff->lockfile, O_RDWR | O_CREAT | O_EXCL, 0400);
	if (lockfd < 0) {
		if (errno == EEXIST)
			err = EWOULDBLOCK;
		else
			err = errno;
		err_printf(ff, "%s: %s: %s\n", ff->lockfile,
			   _("opening lockfile failed"),
			   strerror(err));
		ff->lockfile = NULL;
		return err;
	}
	close(lockfd);

	resolved = realpath(ff->lockfile, NULL);
	if (!resolved) {
		err = errno;
		err_printf(ff, "%s: %s: %s\n", ff->lockfile,
			   _("resolving lockfile failed"),
			   strerror(err));
		unlink(ff->lockfile);
		ff->lockfile = NULL;
		return err;
	}
	free(ff->lockfile);
	ff->lockfile = resolved;

	return 0;
}

static void fuse2fs_release_lockfile(struct fuse2fs *ff)
{
	if (unlink(ff->lockfile)) {
		errcode_t err = errno;

		err_printf(ff, "%s: %s: %s\n", ff->lockfile,
			   _("removing lockfile failed"),
			   strerror(err));
	}
	free(ff->lockfile);
}

static void fuse2fs_unmount(struct fuse2fs *ff)
{
	errcode_t err;

	if (!ff->fs)
		return;

	err = ext2fs_close(ff->fs);
	if (err)
		err_printf(ff, "%s\n", error_message(err));

	ff->fs = NULL;

	if (ff->lockfile)
		fuse2fs_release_lockfile(ff);
}

static errcode_t fuse2fs_open(struct fuse2fs *ff, int libext2_flags)
{
	char options[128];
	int flags = EXT2_FLAG_64BITS | EXT2_FLAG_THREADS | EXT2_FLAG_RW |
		    EXT2_FLAG_WRITE_FULL_SUPER | libext2_flags;
	errcode_t err;

	if (ff->lockfile) {
		err = fuse2fs_acquire_lockfile(ff);
		if (err)
			return err;
	}

	snprintf(options, sizeof(options) - 1, "offset=%lu", ff->offset);
	ff->opstate = F2OP_READONLY;

	if (ff->directio)
		flags |= EXT2_FLAG_DIRECT_IO;

	dbg_printf(ff, "opening with flags=0x%x\n", flags);

	err = ext2fs_open2(ff->device, options, flags, 0, 0, unix_io_manager,
			   &ff->fs);
	if (err == EPERM) {
		err_printf(ff, "%s.\n",
			   _("read-only device, trying to mount norecovery"));
		flags &= ~EXT2_FLAG_RW;
		ff->ro = 1;
		ff->norecovery = 1;
		err = ext2fs_open2(ff->device, options, flags, 0, 0,
				   unix_io_manager, &ff->fs);
	}
	if (err) {
		err_printf(ff, "%s.\n", error_message(err));
		err_printf(ff, "%s\n", _("Please run e2fsck -fy."));
		return err;
	}

	ff->fs->priv_data = ff;
	ff->blocklog = u_log2(ff->fs->blocksize);
	ff->blockmask = ff->fs->blocksize - 1;
	return 0;
}

static inline bool fuse2fs_on_bdev(const struct fuse2fs *ff)
{
	return ff->fs->io->flags & CHANNEL_FLAGS_BLOCK_DEVICE;
}

static errcode_t fuse2fs_config_cache(struct fuse2fs *ff)
{
	char buf[128];
	errcode_t err;

	snprintf(buf, sizeof(buf), "cache_blocks=%llu",
		 FUSE2FS_B_TO_FSBT(ff, ff->cache_size));
	err = io_channel_set_options(ff->fs->io, buf);
	if (err) {
		err_printf(ff, "%s %lluk: %s\n",
			   _("cannot set disk cache size to"),
			   ff->cache_size >> 10,
			   error_message(err));
		return err;
	}

	return 0;
}

static errcode_t fuse2fs_check_support(struct fuse2fs *ff)
{
	ext2_filsys fs = ff->fs;

	if (ext2fs_has_feature_quota(fs->super)) {
		err_printf(ff, "%s\n", _("quotas not supported."));
		return EXT2_ET_UNSUPP_FEATURE;
	}
	if (ext2fs_has_feature_verity(fs->super)) {
		err_printf(ff, "%s\n", _("verity not supported."));
		return EXT2_ET_UNSUPP_FEATURE;
	}
	if (ext2fs_has_feature_encrypt(fs->super)) {
		err_printf(ff, "%s\n", _("encryption not supported."));
		return EXT2_ET_UNSUPP_FEATURE;
	}
	if (ext2fs_has_feature_casefold(fs->super)) {
		err_printf(ff, "%s\n", _("casefolding not supported."));
		return EXT2_ET_UNSUPP_FEATURE;
	}

	if (fs->super->s_state & EXT2_ERROR_FS) {
		err_printf(ff, "%s\n",
 _("Errors detected; running e2fsck is required."));
		return EXT2_ET_FILESYSTEM_CORRUPTED;
	}

	return 0;
}

static int fuse2fs_check_norecovery(struct fuse2fs *ff)
{
	if (ext2fs_has_feature_journal_needs_recovery(ff->fs->super) &&
	    !ff->ro) {
		log_printf(ff, "%s\n",
 _("Required journal recovery suppressed and not mounted read-only."));
		return 32;
	}

	/*
	 * Amazingly, norecovery allows a rw mount when there's a clean journal
	 * present.
	 */
	return 0;
}

static errcode_t fuse2fs_mount(struct fuse2fs *ff)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;

	if (ext2fs_has_feature_journal_needs_recovery(fs->super)) {
		if (ff->norecovery) {
			log_printf(ff, "%s\n",
 _("Mounting read-only without recovering journal."));
		} else {
			log_printf(ff, "%s\n", _("Recovering journal."));
			err = ext2fs_run_ext3_journal(&fs);
			if (err) {
				err_printf(ff, "%s.\n", error_message(err));
				err_printf(ff, "%s\n",
						_("Please run e2fsck -fy."));
				return err;
			}
			ext2fs_clear_feature_journal_needs_recovery(fs->super);
			ext2fs_mark_super_dirty(fs);

			err = fuse2fs_check_support(ff);
			if (err)
				return err;
		}
	}

	if (fs->flags & EXT2_FLAG_RW) {
		if (ext2fs_has_feature_journal(fs->super))
			log_printf(ff, "%s",
 _("Warning: fuse2fs does not support using the journal.\n"
   "There may be file system corruption or data loss if\n"
   "the file system is not gracefully unmounted.\n"));
		err = ext2fs_read_inode_bitmap(fs);
		if (err) {
			translate_error(fs, 0, err);
			return err;
		}
		err = ext2fs_read_block_bitmap(fs);
		if (err) {
			translate_error(fs, 0, err);
			return err;
		}
		ff->opstate = F2OP_WRITABLE;
	}

	if (!(fs->super->s_state & EXT2_VALID_FS))
		err_printf(ff, "%s\n",
 _("Warning: Mounting unchecked fs, running e2fsck is recommended."));
	if (fs->super->s_max_mnt_count > 0 &&
	    fs->super->s_mnt_count >= fs->super->s_max_mnt_count)
		err_printf(ff, "%s\n",
 _("Warning: Maximal mount count reached, running e2fsck is recommended."));
	if (fs->super->s_checkinterval > 0 &&
	    (time_t) (fs->super->s_lastcheck +
		      fs->super->s_checkinterval) <= time(0))
		err_printf(ff, "%s\n",
 _("Warning: Check time reached; running e2fsck is recommended."));
	if (fs->super->s_last_orphan)
		err_printf(ff, "%s\n",
 _("Orphans detected; running e2fsck is recommended."));

	if (!ff->errors_behavior)
		ff->errors_behavior = fs->super->s_errors;

	return 0;
}

static void op_destroy(void *p EXT2FS_ATTR((unused)))
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;

	/* Can be null if op_init is given an incorrect fuse2fs */
	if (!ff)
		return;

	FUSE2FS_CHECK_CONTEXT_NORET(ff);

	/* Can be null if opening the filesystem failed */
	fs = fuse2fs_start(ff);
	if (!fs)
		goto out_unlock;

	dbg_printf(ff, "%s: dev=%s\n", __func__, fs->device_name);
	if (ff->opstate == F2OP_WRITABLE) {
		fs->super->s_state |= EXT2_VALID_FS;
		if (fs->super->s_error_count)
			fs->super->s_state |= EXT2_ERROR_FS;
		ext2fs_mark_super_dirty(fs);
		if (ff->write_gdt_on_destroy) {
			err = ext2fs_set_gdt_csum(fs);
			if (err)
				translate_error(fs, 0, err);
		}

		err = ext2fs_flush2(fs, 0);
		if (err)
			translate_error(fs, 0, err);
	}

	if (ff->debug && fs->io->manager->get_stats) {
		io_stats stats = NULL;

		fs->io->manager->get_stats(fs->io, &stats);
		dbg_printf(ff, "read: %lluk\n",  stats->bytes_read >> 10);
		dbg_printf(ff, "write: %lluk\n", stats->bytes_written >> 10);
		dbg_printf(ff, "hits: %llu\n",   stats->cache_hits);
		dbg_printf(ff, "misses: %llu\n", stats->cache_misses);
		dbg_printf(ff, "hit_ratio: %.1f%%\n",
				(100.0 * stats->cache_hits) /
				(stats->cache_hits + stats->cache_misses));
	}

	if (ff->kernel) {
		char uuid[UUID_STR_SIZE];

		uuid_unparse(fs->super->s_uuid, uuid);
		log_printf(ff, "%s %s.\n", _("unmounting filesystem"), uuid);
	}

	if (ff->unmount_in_destroy)
		fuse2fs_unmount(ff);

out_unlock:
	fuse2fs_finish(ff, 0);
}

#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 17)
static inline int fuse_set_feature_flag(struct fuse_conn_info *conn,
					 uint64_t flag)
{
	if (conn->capable & flag) {
		conn->want |= flag;
		return 1;
	}

	return 0;
}
#endif

#ifdef HAVE_FUSE_IOMAP
static void fuse2fs_iomap_confirm(struct fuse_conn_info *conn,
				  struct fuse2fs *ff)
{
	switch (ff->iomap_state) {
	case IOMAP_UNKNOWN:
		ff->iomap_state = IOMAP_DISABLED;
		return;
	case IOMAP_DISABLED:
		return;
	case IOMAP_FILEIO:
	case IOMAP_ENABLED:
		break;
	}

	/* iomap only works with block devices */
	if (!fuse2fs_on_bdev(ff)) {
		fuse_unset_feature_flag(conn, FUSE_CAP_IOMAP);
		ff->iomap_state = IOMAP_DISABLED;
	}
}
#else
# define fuse2fs_iomap_confirm(...)	((void)0)
#endif

static void *op_init(struct fuse_conn_info *conn
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, struct fuse_config *cfg EXT2FS_ATTR((unused))
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs = ff->fs;
#ifdef HAVE_FUSE_IOMAP
	int was_directio = ff->directio;
#endif
	errcode_t err;
	int ret;

	FUSE2FS_CHECK_CONTEXT_NULL(ff);
	dbg_printf(ff, "%s: dev=%s\n", __func__, ff->device);
#ifdef FUSE_CAP_IOCTL_DIR
	fuse_set_feature_flag(conn, FUSE_CAP_IOCTL_DIR);
#endif
#ifdef FUSE_CAP_POSIX_ACL
	if (ff->acl)
		fuse_set_feature_flag(conn, FUSE_CAP_POSIX_ACL);
#endif
#ifdef FUSE_CAP_CACHE_SYMLINKS
	fuse_set_feature_flag(conn, FUSE_CAP_CACHE_SYMLINKS);
#endif
#ifdef FUSE_CAP_NO_EXPORT_SUPPORT
	fuse_set_feature_flag(conn, FUSE_CAP_NO_EXPORT_SUPPORT);
#endif
#ifdef HAVE_FUSE_IOMAP
	if (ff->iomap_state != IOMAP_DISABLED &&
	    fuse_set_feature_flag(conn, FUSE_CAP_IOMAP))
		ff->iomap_state = IOMAP_ENABLED;

	/*
	 * If iomap is turned on and the kernel advertises support for both
	 * direct and buffered IO, then that means the kernel handles all
	 * regular file data block IO for us.  That means we can turn off all
	 * of libext2fs' file data block handling except for inline data.
	 *
	 * XXX: kernel doesn't support inline data iomap
	 */
	if (fuse2fs_iomap_enabled(ff) &&
	    fuse_get_feature_flag(conn, FUSE_CAP_IOMAP_DIRECTIO) &&
	    fuse_get_feature_flag(conn, FUSE_CAP_IOMAP_FILEIO))
		ff->iomap_state = IOMAP_FILEIO;

	/*
	 * In iomap mode, the kernel writes file data directly to the block
	 * device and does not flush the bdev page cache.  We must open the
	 * filesystem in directio mode to avoid cache coherency issues when
	 * reading file data.  If we can't open the bdev in directio mode, we
	 * must not use iomap.
	 *
	 * If we know that the kernel can handle all regular file IO for us,
	 * then there is no cache coherency issue and we can use buffered reads
	 * for all IO, which will all be filesystem metadata.
	 */
	if (fuse2fs_iomap_enabled(ff) && !fuse2fs_iomap_does_fileio(ff))
		ff->directio = 1;
#endif

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
	conn->time_gran = 1;
	cfg->use_ino = 1;
	if (ff->debug)
		cfg->debug = 1;
	cfg->nullpath_ok = 1;
#endif

	/*
	 * If the ext2_filsys object is null, then we are operating in fuseblk
	 * mode and must reopen the filesystem.  If any of these steps fail,
	 * tough.
	 */
	if (!fs) {
		err = fuse2fs_open(ff, 0);
#ifdef HAVE_FUSE_IOMAP
		if (err && fuse2fs_iomap_enabled(ff) && !was_directio) {
			fuse_unset_feature_flag(conn, FUSE_CAP_IOMAP);
			ff->iomap_state = IOMAP_DISABLED;
			ff->directio = 0;
			err = fuse2fs_open(ff, 0);
		}
#endif
		if (err)
			goto mount_fail;
		fs = ff->fs;

		fuse2fs_iomap_confirm(conn, ff);

		if (ff->cache_size) {
			err = fuse2fs_config_cache(ff);
			if (err)
				goto mount_fail;
		}

		err = fuse2fs_check_support(ff);
		if (err)
			goto mount_fail;

		if (ext2fs_has_feature_shared_blocks(fs->super)) {
			log_printf(ff, "%s\n",
 _("shared file blocks, mounting filesystem read-only."));
			fs->flags &= ~EXT2_FLAG_RW;
		}

		if (ff->norecovery) {
			ret = fuse2fs_check_norecovery(ff);
			if (ret)
				goto mount_fail;
		}

		err = fuse2fs_mount(ff);
		if (err)
			goto mount_fail;
	} else {
		fuse2fs_iomap_confirm(conn, ff);
	}

#if defined(HAVE_FUSE_IOMAP)
	if (ff->iomap_want == FT_ENABLE && !fuse2fs_iomap_enabled(ff)) {
		err_printf(ff, "%s\n", _("could not enable iomap."));
		goto mount_fail;
	}
	if (ff->iomap_passthrough_options && !fuse2fs_iomap_enabled(ff)) {
		err_printf(ff, "%s\n", _("some mount options require iomap."));
		goto mount_fail;
	}
#endif
#if defined(HAVE_FUSE_IOMAP) && defined(FUSE_CAP_IOMAP_DIRECTIO)
	if (fuse2fs_iomap_enabled(ff))
		fuse_set_feature_flag(conn, FUSE_CAP_IOMAP_DIRECTIO);
#endif
#if defined(HAVE_FUSE_IOMAP) && defined(FUSE_CAP_IOMAP_FILEIO)
	if (fuse2fs_iomap_enabled(ff))
		fuse_set_feature_flag(conn, FUSE_CAP_IOMAP_FILEIO);
#endif

	/*
	 * If we're mounting in iomap mode, we need to unmount in op_destroy
	 * so that the block device will be released before umount(2) returns.
	 *
	 * XXX: It turns out that fuse2fs creates internal node ids that have
	 * nothing to do with the ext2_ino_t that we give it.  These internal
	 * node ids are what actually gets igetted in the kernel, which means
	 * that there can be multiple fuse_inode objects for the same fuse2fs
	 * inode.
	 *
	 * What this means, horrifyingly, is that on a fuse filesystem that
	 * supports hard links, the in-kernel i_rwsem does not protect against
	 * concurrent writes between files that point to the same inode.  That
	 * in turn means that the file mode and size can get desynchronized
	 * between the multiple fuse_inode objects.  This also means that we
	 * cannot cache iomaps in the kernel AT ALL because the caches will
	 * get out of sync, leading to WARN_ONs from the iomap zeroing code and
	 * probably data corruption after that.
	 *
	 * So for now we just disable hardlinking on iomap to see if the weird
	 * fstests failures (particularly g/476) go away.  Long term it means
	 * we probably have to find a way around this, like porting fuse2fs
	 * to be a low level fuse driver.
	 */
	if (fuse2fs_iomap_enabled(ff)) {
		ff->unmount_in_destroy = 1;
		ff->can_hardlink = 0;

		/*
		 * XXX: inline data file io depends on op_read/write being fed
		 * a path, so we have to slow everyone down to look up the path
		 * from the nodeid
		 */
		if (ext2fs_has_feature_inline_data(ff->fs->super))
			cfg->nullpath_ok = 0;
	}

	/* Clear the valid flag so that an unclean shutdown forces a fsck */
	if (ff->opstate == F2OP_WRITABLE) {
		fs->super->s_mnt_count++;
		ext2fs_set_tstamp(fs->super, s_mtime, time(NULL));
		fs->super->s_state &= ~EXT2_VALID_FS;
		ext2fs_mark_super_dirty(fs);
		err = ext2fs_flush2(fs, 0);
		if (err)
			translate_error(fs, 0, err);
	}

	if (ff->kernel) {
		char uuid[UUID_STR_SIZE];

		uuid_unparse(fs->super->s_uuid, uuid);
		log_printf(ff, "%s %s.\n", _("mounted filesystem"), uuid);
	}
out:
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 17)
	/*
	 * THIS MUST GO LAST!
	 *
	 * The high-level libfuse code has a strange bug: it sets feature flags
	 * in conn->want_ext, and later copies the lower 32 bits to conn->want.
	 * If we in turn change some bits in want_ext without updating want,
	 * the lower level library to observe that both want and want_ext have
	 * gotten out of sync, and refuses to mount.  Therefore, synchronize
	 * the two.
	 */
	conn->want = conn->want_ext & 0xFFFFFFFF;
#endif
	return ff;
mount_fail:
	ff->retcode = 32;
	/* Tear down the mount immediately. */
	fuse_exit(ctxt->fuse);
	goto out;
}

static int fuse2fs_stat(struct fuse2fs *ff, ext2_ino_t ino,
			struct stat *statbuf)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	dev_t fakedev = 0;
	errcode_t err;
	int ret = 0;
	struct timespec tv;

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	memcpy(&fakedev, fs->super->s_uuid, sizeof(fakedev));
	statbuf->st_dev = fakedev;
	statbuf->st_ino = ino;
	statbuf->st_mode = inode.i_mode;
	statbuf->st_nlink = inode.i_links_count;
	statbuf->st_uid = inode_uid(inode);
	statbuf->st_gid = inode_gid(inode);
	statbuf->st_size = EXT2_I_SIZE(&inode);
	statbuf->st_blksize = fs->blocksize;
	statbuf->st_blocks = ext2fs_get_stat_i_blocks(fs,
						EXT2_INODE(&inode));
	EXT4_INODE_GET_XTIME(i_atime, &tv, &inode);
#if HAVE_STRUCT_STAT_ST_ATIM
	statbuf->st_atim = tv;
#else
	statbuf->st_atime = tv.tv_sec;
#endif
	EXT4_INODE_GET_XTIME(i_mtime, &tv, &inode);
#if HAVE_STRUCT_STAT_ST_ATIM
	statbuf->st_mtim = tv;
#else
	statbuf->st_mtime = tv.tv_sec;
#endif
	EXT4_INODE_GET_XTIME(i_ctime, &tv, &inode);
#if HAVE_STRUCT_STAT_ST_ATIM
	statbuf->st_ctim = tv;
#else
	statbuf->st_ctime = tv.tv_sec;
#endif

	dbg_printf(ff, "%s: ino=%d atime=%lld.%ld mtime=%lld.%ld ctime=%lld.%ld\n",
		   __func__, ino,
		   (long long int)statbuf->st_atim.tv_sec, statbuf->st_atim.tv_nsec,
		   (long long int)statbuf->st_mtim.tv_sec, statbuf->st_mtim.tv_nsec,
		   (long long int)statbuf->st_ctim.tv_sec, statbuf->st_ctim.tv_nsec);

	if (LINUX_S_ISCHR(inode.i_mode) ||
	    LINUX_S_ISBLK(inode.i_mode)) {
		if (inode.i_block[0])
			statbuf->st_rdev = inode.i_block[0];
		else
			statbuf->st_rdev = inode.i_block[1];
	}

	return ret;
}

static int __fuse2fs_file_ino(struct fuse2fs *ff, const char *path,
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			      struct fuse_file_info *fp EXT2FS_ATTR((unused)),
#endif
			      ext2_ino_t *inop,
			      const char *func,
			      int line)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
	if (fp) {
		struct fuse2fs_file_handle *fh =
			(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;

		if (fh->ino == 0)
			return -ESTALE;

		*inop = fh->ino;
		dbg_printf(ff, "%s: get ino=%d\n", func, fh->ino);
		return 0;
	}
#endif
	dbg_printf(ff, "%s: get path=%s\n", func, path);
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, inop);
	if (err)
		return __translate_error(fs, 0, err, func, line);

	return 0;
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
# define fuse2fs_file_ino(ff, path, fp, inop) \
	__fuse2fs_file_ino((ff), (path), (fp), (inop), __func__, __LINE__)
#else
# define fuse2fs_file_ino(ff, path, fp, inop) \
	__fuse2fs_file_ino((ff), (path), NULL, (inop), __func__, __LINE__)
#endif

static int op_getattr(const char *path, struct stat *statbuf
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, struct fuse_file_info *fi
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_ino_t ino;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fuse2fs_start(ff);
	ret = fuse2fs_file_ino(ff, path, fi, &ino);
	if (ret)
		goto out;
	ret = fuse2fs_stat(ff, ino, statbuf);
out:
	fuse2fs_finish(ff, ret);
	return ret;
}

#ifdef HAVE_FUSE_IOMAP
static int op_getattr_iflags(const char *path, struct stat *statbuf,
			     unsigned int *iflags, struct fuse_file_info *fi)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	int ret = op_getattr(path, statbuf, fi);

	if (ret)
		return ret;

	if (fuse2fs_iomap_does_fileio(ff))
		*iflags |= FUSE_IFLAG_IOMAP_DIRECTIO | FUSE_IFLAG_IOMAP_FILEIO;

	return 0;
}
#endif

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 18) && defined(STATX_BASIC_STATS)
static inline void fuse2fs_set_statx_attr(struct statx *stx,
					  uint64_t statx_flag, int set)
{
	if (set)
		stx->stx_attributes |= statx_flag;
	stx->stx_attributes_mask |= statx_flag;
}

static int fuse2fs_statx(struct fuse2fs *ff, ext2_ino_t ino,
			 uint32_t statx_mask, struct statx *stx, size_t size)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;;
	dev_t fakedev = 0;
	errcode_t err;
	struct timespec tv;

	if (size < sizeof(struct statx))
		return translate_error(fs, ino, EOPNOTSUPP);

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	memcpy(&fakedev, fs->super->s_uuid, sizeof(fakedev));
	stx->stx_mask = STATX_BASIC_STATS | STATX_BTIME;
	stx->stx_dev_major = major(fakedev);
	stx->stx_dev_minor = minor(fakedev);
	stx->stx_ino = ino;
	stx->stx_mode = inode.i_mode;
	stx->stx_nlink = inode.i_links_count;
	stx->stx_uid = inode_uid(inode);
	stx->stx_gid = inode_gid(inode);
	stx->stx_size = EXT2_I_SIZE(&inode);
	stx->stx_blksize = fs->blocksize;
	stx->stx_blocks = ext2fs_get_stat_i_blocks(fs,
						EXT2_INODE(&inode));
	EXT4_INODE_GET_XTIME(i_atime, &tv, &inode);
	stx->stx_atime.tv_sec = tv.tv_sec;
	stx->stx_atime.tv_nsec = tv.tv_nsec;

	EXT4_INODE_GET_XTIME(i_mtime, &tv, &inode);
	stx->stx_mtime.tv_sec = tv.tv_sec;
	stx->stx_mtime.tv_nsec = tv.tv_nsec;

	EXT4_INODE_GET_XTIME(i_ctime, &tv, &inode);
	stx->stx_ctime.tv_sec = tv.tv_sec;
	stx->stx_ctime.tv_nsec = tv.tv_nsec;

	EXT4_INODE_GET_XTIME(i_crtime, &tv, &inode);
	stx->stx_btime.tv_sec = tv.tv_sec;
	stx->stx_btime.tv_nsec = tv.tv_nsec;

	dbg_printf(ff, "%s: ino=%d atime=%lld.%d mtime=%lld.%d ctime=%lld.%d btime=%lld.%d\n",
		   __func__, ino,
		   (long long int)stx->stx_atime.tv_sec, stx->stx_atime.tv_nsec,
		   (long long int)stx->stx_mtime.tv_sec, stx->stx_mtime.tv_nsec,
		   (long long int)stx->stx_ctime.tv_sec, stx->stx_ctime.tv_nsec,
		   (long long int)stx->stx_btime.tv_sec, stx->stx_btime.tv_nsec);

	if (LINUX_S_ISCHR(inode.i_mode) ||
	    LINUX_S_ISBLK(inode.i_mode)) {
		if (inode.i_block[0]) {
			stx->stx_rdev_major = major(inode.i_block[0]);
			stx->stx_rdev_minor = minor(inode.i_block[0]);
		} else {
			stx->stx_rdev_major = major(inode.i_block[1]);
			stx->stx_rdev_minor = minor(inode.i_block[1]);
		}
	}

	fuse2fs_set_statx_attr(stx, STATX_ATTR_COMPRESSED,
			       inode.i_flags & EXT2_COMPR_FL);
	fuse2fs_set_statx_attr(stx, STATX_ATTR_IMMUTABLE,
			       inode.i_flags & EXT2_IMMUTABLE_FL);
	fuse2fs_set_statx_attr(stx, STATX_ATTR_APPEND,
			       inode.i_flags & EXT2_APPEND_FL);
	fuse2fs_set_statx_attr(stx, STATX_ATTR_NODUMP,
			       inode.i_flags & EXT2_NODUMP_FL);

	return 0;
}

static int op_statx(const char *path, uint32_t statx_flags, uint32_t statx_mask,
		    struct statx *stx, size_t size, struct fuse_file_info *fi)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_ino_t ino;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fuse2fs_start(ff);
	ret = fuse2fs_file_ino(ff, path, fi, &ino);
	if (ret)
		goto out;
	ret = fuse2fs_statx(ff, ino, statx_mask, stx, size);
out:
	fuse2fs_finish(ff, ret);
	return ret;
}
#else
# define op_statx		NULL
#endif

static int op_readlink(const char *path, char *buf, size_t len)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t ino;
	struct ext2_inode inode;
	unsigned int got;
	ext2_file_t file;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s\n", __func__, path);
	fs = fuse2fs_start(ff);
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	err = ext2fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	if (!LINUX_S_ISLNK(inode.i_mode)) {
		ret = -EINVAL;
		goto out;
	}

	len--;
	if (inode.i_size < len)
		len = inode.i_size;
	if (ext2fs_is_fast_symlink(&inode))
		memcpy(buf, (char *)inode.i_block, len);
	else {
		/* big/inline symlink */

		err = ext2fs_file_open(fs, ino, 0, &file);
		if (err) {
			ret = translate_error(fs, ino, err);
			goto out;
		}

		err = ext2fs_file_read(file, buf, len, &got);
		if (err || got != len) {
			ext2fs_file_close(file);
			ret = translate_error(fs, ino, err);
			goto out2;
		}

out2:
		err = ext2fs_file_close(file);
		if (ret)
			goto out;
		if (err) {
			ret = translate_error(fs, ino, err);
			goto out;
		}
	}
	buf[len] = 0;

	if (fuse2fs_is_writeable(ff)) {
		ret = fuse2fs_update_atime(ff, ino);
		if (ret)
			goto out;
	}

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int __getxattr(struct fuse2fs *ff, ext2_ino_t ino, const char *name,
		      void **value, size_t *value_len)
{
	ext2_filsys fs = ff->fs;
	struct ext2_xattr_handle *h;
	errcode_t err;
	int ret = 0;

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_xattrs_read(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

	err = ext2fs_xattr_get(h, name, value, value_len);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

out_close:
	err = ext2fs_xattrs_close(&h);
	if (err && !ret)
		ret = translate_error(fs, ino, err);
	return ret;
}

static int __setxattr(struct fuse2fs *ff, ext2_ino_t ino, const char *name,
		      void *value, size_t valuelen)
{
	ext2_filsys fs = ff->fs;
	struct ext2_xattr_handle *h;
	errcode_t err;
	int ret = 0;

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_xattrs_read(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

	err = ext2fs_xattr_set(h, name, value, valuelen);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

out_close:
	err = ext2fs_xattrs_close(&h);
	if (err && !ret)
		ret = translate_error(fs, ino, err);
	return ret;
}

static int propagate_default_acls(struct fuse2fs *ff, ext2_ino_t parent,
				  ext2_ino_t child)
{
	void *def;
	size_t deflen;
	int ret;

	if (!ff->acl || fuse2fs_iomap_does_fileio(ff))
		return 0;

	ret = __getxattr(ff, parent, XATTR_NAME_POSIX_ACL_DEFAULT, &def,
			 &deflen);
	switch (ret) {
	case -ENODATA:
	case -ENOENT:
		/* no default acl */
		return 0;
	case 0:
		break;
	default:
		return ret;
	}

	ret = __setxattr(ff, child, XATTR_NAME_POSIX_ACL_DEFAULT, def, deflen);
	ext2fs_free_mem(&def);
	return ret;
}

static inline void fuse2fs_set_uid(struct ext2_inode_large *inode, uid_t uid)
{
	inode->i_uid = uid;
	ext2fs_set_i_uid_high(*inode, uid >> 16);
}

static inline void fuse2fs_set_gid(struct ext2_inode_large *inode, gid_t gid)
{
	inode->i_gid = gid;
	ext2fs_set_i_gid_high(*inode, gid >> 16);
}

static int fuse2fs_new_child_gid(struct fuse2fs *ff, ext2_ino_t parent,
				 gid_t *gid, int *parent_sgid)
{
	struct ext2_inode_large inode;
	struct fuse_context *ctxt = fuse_get_context();
	errcode_t err;

	err = fuse2fs_read_inode(ff->fs, parent, &inode);
	if (err)
		return translate_error(ff->fs, parent, err);

	if (inode.i_mode & S_ISGID) {
		if (parent_sgid)
			*parent_sgid = 1;
		*gid = inode.i_gid;
	} else {
		if (parent_sgid)
			*parent_sgid = 0;
		*gid = ctxt->gid;
	}

	return 0;
}

static inline int fuse2fs_dirsync_flush(struct fuse2fs *ff, ext2_ino_t ino,
					struct ext2_inode_large *pinode)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	errcode_t err;

	if (!ff->dirsync)
		return 0;

	if (!pinode) {
		err = fuse2fs_read_inode(fs, ino, &inode);
		if (err)
			return err;

		pinode = &inode;
	}

	if (!(pinode->i_flags & EXT2_DIRSYNC_FL))
		return 0;

	err = ext2fs_flush2(fs, 0);
	if (err)
		return translate_error(fs, 0, err);

	return 0;
}

static void fuse2fs_set_extra_isize(struct fuse2fs *ff, ext2_ino_t ino,
				    struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	size_t extra = sizeof(struct ext2_inode_large) -
		EXT2_GOOD_OLD_INODE_SIZE;

	if (ext2fs_has_feature_extra_isize(fs->super)) {
		dbg_printf(ff, "%s: ino=%u extra=%zu want=%u min=%u\n",
			   __func__, ino, extra, fs->super->s_want_extra_isize,
			   fs->super->s_min_extra_isize);

		if (fs->super->s_want_extra_isize > extra)
			extra = fs->super->s_want_extra_isize;
		if (fs->super->s_min_extra_isize > extra)
			extra = fs->super->s_min_extra_isize;
	}

	inode->i_extra_isize = extra;
}

static int op_mknod(const char *path, mode_t mode, dev_t dev)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	ext2_ino_t parent, child;
	char *temp_path;
	errcode_t err;
	char *node_name, a;
	int filetype;
	struct ext2_inode_large inode;
	gid_t gid;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s mode=0%o dev=0x%x\n", __func__, path, mode,
		   (unsigned int)dev);
	temp_path = strdup(path);
	if (!temp_path) {
		ret = -ENOMEM;
		goto out;
	}
	node_name = strrchr(temp_path, '/');
	if (!node_name) {
		ret = -ENOMEM;
		goto out;
	}
	node_name++;
	a = *node_name;
	*node_name = 0;

	fs = fuse2fs_start(ff);
	if (!fs_can_allocate(ff, 2)) {
		ret = -ENOSPC;
		goto out2;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &parent);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	ret = check_inum_access(ff, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	*node_name = a;

	if (LINUX_S_ISCHR(mode))
		filetype = EXT2_FT_CHRDEV;
	else if (LINUX_S_ISBLK(mode))
		filetype = EXT2_FT_BLKDEV;
	else if (LINUX_S_ISFIFO(mode))
		filetype = EXT2_FT_FIFO;
	else if (LINUX_S_ISSOCK(mode))
		filetype = EXT2_FT_SOCK;
	else {
		ret = -EINVAL;
		goto out2;
	}

	err = fuse2fs_new_child_gid(ff, parent, &gid, NULL);
	if (err)
		goto out2;

	err = ext2fs_new_inode(fs, parent, mode, 0, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	dbg_printf(ff, "%s: create ino=%d/name=%s in dir=%d\n", __func__, child,
		   node_name, parent);
	err = ext2fs_link(fs, parent, node_name, child,
			  filetype | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse2fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	memset(&inode, 0, sizeof(inode));
	inode.i_mode = mode;

	if (dev & ~0xFFFF)
		inode.i_block[1] = dev;
	else
		inode.i_block[0] = dev;
	inode.i_links_count = 1;
	fuse2fs_set_extra_isize(ff, child, &inode);
	fuse2fs_set_uid(&inode, ctxt->uid);
	fuse2fs_set_gid(&inode, gid);

	err = ext2fs_write_new_inode(fs, child, EXT2_INODE(&inode));
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	inode.i_generation = ff->next_generation++;
	fuse2fs_init_timestamps(ff, child, &inode);
	err = fuse2fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ext2fs_inode_alloc_stats2(fs, child, 1, 0);

	ret = propagate_default_acls(ff, parent, child);
	if (ret)
		goto out2;

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse2fs_finish(ff, ret);
out:
	free(temp_path);
	return ret;
}

static int op_mkdir(const char *path, mode_t mode)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	ext2_ino_t parent, child;
	char *temp_path;
	errcode_t err;
	char *node_name, a;
	struct ext2_inode_large inode;
	char *block;
	blk64_t blk;
	int ret = 0;
	gid_t gid;
	int parent_sgid;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s mode=0%o\n", __func__, path, mode);
	temp_path = strdup(path);
	if (!temp_path) {
		ret = -ENOMEM;
		goto out;
	}
	node_name = strrchr(temp_path, '/');
	if (!node_name) {
		ret = -ENOMEM;
		goto out;
	}
	node_name++;
	a = *node_name;
	*node_name = 0;

	fs = fuse2fs_start(ff);
	if (!fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out2;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &parent);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	ret = check_inum_access(ff, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse2fs_new_child_gid(ff, parent, &gid, &parent_sgid);
	if (err)
		goto out2;

	*node_name = a;

	err = ext2fs_mkdir2(fs, parent, 0, 0, EXT2FS_LINK_EXPAND,
			    node_name, NULL);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse2fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	/* Still have to update the uid/gid of the dir */
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}
	dbg_printf(ff, "%s: created ino=%d/path=%s in dir=%d\n", __func__, child,
		   node_name, parent);

	err = fuse2fs_read_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	fuse2fs_set_extra_isize(ff, child, &inode);
	fuse2fs_set_uid(&inode, ctxt->uid);
	fuse2fs_set_gid(&inode, gid);
	inode.i_mode = LINUX_S_IFDIR | (mode & ~S_ISUID);
	if (parent_sgid)
		inode.i_mode |= S_ISGID;
	inode.i_generation = ff->next_generation++;
	fuse2fs_init_timestamps(ff, child, &inode);

	err = fuse2fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	/* Rewrite the directory block checksum, having set i_generation */
	if ((inode.i_flags & EXT4_INLINE_DATA_FL) ||
	    !ext2fs_has_feature_metadata_csum(fs->super))
		goto out2;
	err = ext2fs_new_dir_block(fs, child, parent, &block);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}
	err = ext2fs_bmap2(fs, child, EXT2_INODE(&inode), NULL, 0, 0,
			   NULL, &blk);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out3;
	}
	err = ext2fs_write_dir_block4(fs, blk, block, 0, child);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out3;
	}

	ret = propagate_default_acls(ff, parent, child);
	if (ret)
		goto out3;

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out3;

out3:
	ext2fs_free_mem(&block);
out2:
	fuse2fs_finish(ff, ret);
out:
	free(temp_path);
	return ret;
}

static int fuse2fs_unlink(struct fuse2fs *ff, const char *path,
			  ext2_ino_t *parent)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	ext2_ino_t dir;
	char *filename = strdup(path);
	char *base_name;
	int ret;

	base_name = strrchr(filename, '/');
	if (base_name) {
		*base_name++ = '\0';
		err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, filename,
				   &dir);
		if (err) {
			free(filename);
			return translate_error(fs, 0, err);
		}
	} else {
		dir = EXT2_ROOT_INO;
		base_name = filename;
	}

	ret = check_inum_access(ff, dir, W_OK);
	if (ret) {
		free(filename);
		return ret;
	}

	dbg_printf(ff, "%s: unlinking name=%s from dir=%d\n", __func__,
		   base_name, dir);
	err = ext2fs_unlink(fs, dir, base_name, 0, 0);
	free(filename);
	if (err)
		return translate_error(fs, dir, err);

	ret = fuse2fs_update_mtime(ff, dir, NULL);
	if (ret)
		return ret;

	if (parent)
		*parent = dir;
	return 0;
}

static errcode_t remove_ea_inodes(struct fuse2fs *ff, ext2_ino_t ino,
				  struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	struct ext2_xattr_handle *h;
	errcode_t err;

	/*
	 * The xattr handle maintains its own private copy of the inode, so
	 * write ours to disk so that we can read it.
	 */
	err = fuse2fs_write_inode(fs, ino, inode);
	if (err)
		return err;

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err)
		return err;

	err = ext2fs_xattrs_read(h);
	if (err)
		goto out_close;

	err = ext2fs_xattr_remove_all(h);
	if (err)
		goto out_close;

out_close:
	ext2fs_xattrs_close(&h);

	/* Now read the inode back in. */
	return fuse2fs_read_inode(fs, ino, inode);
}

static int remove_inode(struct fuse2fs *ff, ext2_ino_t ino)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret = 0;

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}
	dbg_printf(ff, "%s: put ino=%d links=%d\n", __func__, ino,
		   inode.i_links_count);

	switch (inode.i_links_count) {
	case 0:
		return 0; /* XXX: already done? */
	case 1:
		inode.i_links_count--;
		ext2fs_set_dtime(fs, EXT2_INODE(&inode));
		break;
	default:
		inode.i_links_count--;
	}

	ret = fuse2fs_update_ctime(ff, ino, &inode);
	if (ret)
		goto out;

	if (inode.i_links_count)
		goto write_out;

	if (ext2fs_has_feature_ea_inode(fs->super)) {
		err = remove_ea_inodes(ff, ino, &inode);
		if (err)
			goto write_out;
	}

	/* Nobody holds this file; free its blocks! */
	err = ext2fs_free_ext_attr(fs, ino, &inode);
	if (err)
		goto write_out;

	if (ext2fs_inode_has_valid_blocks2(fs, EXT2_INODE(&inode))) {
		err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), NULL,
				   0, ~0ULL);
		if (err) {
			ret = translate_error(fs, ino, err);
			goto write_out;
		}
	}

	ext2fs_inode_alloc_stats2(fs, ino, -1,
				  LINUX_S_ISDIR(inode.i_mode));

write_out:
	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}
out:
	return ret;
}

static int __op_unlink(struct fuse2fs *ff, const char *path)
{
	ext2_filsys fs = ff->fs;
	ext2_ino_t parent, ino;
	errcode_t err;
	int ret = 0;

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	ret = check_inum_access(ff, ino, W_OK);
	if (ret)
		goto out;

	ret = fuse2fs_unlink(ff, path, &parent);
	if (ret)
		goto out;

	ret = remove_inode(ff, ino);
	if (ret)
		goto out;

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out;

out:
	return ret;
}

static int op_unlink(const char *path)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	int ret;

	FUSE2FS_CHECK_CONTEXT(ff);
	fuse2fs_start(ff);
	ret = __op_unlink(ff, path);
	fuse2fs_finish(ff, ret);
	return ret;
}

struct rd_struct {
	ext2_ino_t	parent;
	int		empty;
};

static int rmdir_proc(ext2_ino_t dir EXT2FS_ATTR((unused)),
		      int	entry EXT2FS_ATTR((unused)),
		      struct ext2_dir_entry *dirent,
		      int	offset EXT2FS_ATTR((unused)),
		      int	blocksize EXT2FS_ATTR((unused)),
		      char	*buf EXT2FS_ATTR((unused)),
		      void	*private)
{
	struct rd_struct *rds = (struct rd_struct *) private;

	if (dirent->inode == 0)
		return 0;
	if (((dirent->name_len & 0xFF) == 1) && (dirent->name[0] == '.'))
		return 0;
	if (((dirent->name_len & 0xFF) == 2) && (dirent->name[0] == '.') &&
	    (dirent->name[1] == '.')) {
		rds->parent = dirent->inode;
		return 0;
	}
	rds->empty = 0;
	return 0;
}

static int __op_rmdir(struct fuse2fs *ff, const char *path)
{
	ext2_filsys fs = ff->fs;
	ext2_ino_t parent, child;
	errcode_t err;
	struct ext2_inode_large inode;
	struct rd_struct rds;
	int ret = 0;

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	dbg_printf(ff, "%s: rmdir path=%s ino=%d\n", __func__, path, child);

	ret = check_inum_access(ff, child, W_OK);
	if (ret)
		goto out;

	rds.parent = 0;
	rds.empty = 1;

	err = ext2fs_dir_iterate2(fs, child, 0, 0, rmdir_proc, &rds);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out;
	}

	/* the kernel checks parent permissions before emptiness */
	if (rds.parent == 0) {
		ret = translate_error(fs, child, EXT2_ET_FILESYSTEM_CORRUPTED);
		goto out;
	}

	ret = check_inum_access(ff, rds.parent, W_OK);
	if (ret)
		goto out;

	if (rds.empty == 0) {
		ret = -ENOTEMPTY;
		goto out;
	}

	ret = fuse2fs_unlink(ff, path, &parent);
	if (ret)
		goto out;
	/* Directories have to be "removed" twice. */
	ret = remove_inode(ff, child);
	if (ret)
		goto out;
	ret = remove_inode(ff, child);
	if (ret)
		goto out;

	if (rds.parent) {
		dbg_printf(ff, "%s: decr dir=%d link count\n", __func__,
			   rds.parent);
		err = fuse2fs_read_inode(fs, rds.parent, &inode);
		if (err) {
			ret = translate_error(fs, rds.parent, err);
			goto out;
		}
		if (inode.i_links_count > 1)
			inode.i_links_count--;
		ret = fuse2fs_update_mtime(ff, rds.parent, &inode);
		if (ret)
			goto out;
		err = fuse2fs_write_inode(fs, rds.parent, &inode);
		if (err) {
			ret = translate_error(fs, rds.parent, err);
			goto out;
		}
	}

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out;

out:
	return ret;
}

static int op_rmdir(const char *path)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	int ret;

	FUSE2FS_CHECK_CONTEXT(ff);
	fuse2fs_start(ff);
	ret = __op_rmdir(ff, path);
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_symlink(const char *src, const char *dest)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	ext2_ino_t parent, child;
	char *temp_path;
	errcode_t err;
	char *node_name, a;
	struct ext2_inode_large inode;
	gid_t gid;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: symlink %s to %s\n", __func__, src, dest);
	temp_path = strdup(dest);
	if (!temp_path) {
		ret = -ENOMEM;
		goto out;
	}
	node_name = strrchr(temp_path, '/');
	if (!node_name) {
		ret = -ENOMEM;
		goto out;
	}
	node_name++;
	a = *node_name;
	*node_name = 0;

	fs = fuse2fs_start(ff);
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &parent);
	*node_name = a;
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	ret = check_inum_access(ff, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse2fs_new_child_gid(ff, parent, &gid, NULL);
	if (err)
		goto out2;

	/* Create symlink */
	err = ext2fs_symlink(fs, parent, 0, node_name, src);
	if (err == EXT2_ET_DIR_NO_SPACE) {
		err = ext2fs_expand_dir(fs, parent);
		if (err) {
			ret = translate_error(fs, parent, err);
			goto out2;
		}

		err = ext2fs_symlink(fs, parent, 0, node_name, src);
	}
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	/* Update parent dir's mtime */
	ret = fuse2fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	/* Still have to update the uid/gid of the symlink */
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}
	dbg_printf(ff, "%s: symlinking ino=%d/name=%s to dir=%d\n", __func__,
		   child, node_name, parent);

	err = fuse2fs_read_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	fuse2fs_set_extra_isize(ff, child, &inode);
	fuse2fs_set_uid(&inode, ctxt->uid);
	fuse2fs_set_gid(&inode, gid);
	inode.i_generation = ff->next_generation++;
	fuse2fs_init_timestamps(ff, child, &inode);

	err = fuse2fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse2fs_finish(ff, ret);
out:
	free(temp_path);
	return ret;
}

struct update_dotdot {
	ext2_ino_t new_dotdot;
};

static int update_dotdot_helper(ext2_ino_t dir EXT2FS_ATTR((unused)),
				int entry EXT2FS_ATTR((unused)),
				struct ext2_dir_entry *dirent,
				int offset EXT2FS_ATTR((unused)),
				int blocksize EXT2FS_ATTR((unused)),
				char *buf EXT2FS_ATTR((unused)),
				void *priv_data)
{
	struct update_dotdot *ud = priv_data;

	if (ext2fs_dirent_name_len(dirent) == 2 &&
	    dirent->name[0] == '.' && dirent->name[1] == '.') {
		dirent->inode = ud->new_dotdot;
		return DIRENT_CHANGED | DIRENT_ABORT;
	}

	return 0;
}

static int op_rename(const char *from, const char *to
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, unsigned int flags EXT2FS_ATTR((unused))
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t from_ino, to_ino, to_dir_ino, from_dir_ino;
	char *temp_to = NULL, *temp_from = NULL;
	char *cp, a;
	struct ext2_inode inode;
	struct update_dotdot ud;
	int ret = 0;

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
	/* renameat2 is not supported */
	if (flags)
		return -ENOSYS;
#endif

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: renaming %s to %s\n", __func__, from, to);
	fs = fuse2fs_start(ff);
	if (!fs_can_allocate(ff, 5)) {
		ret = -ENOSPC;
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, from, &from_ino);
	if (err || from_ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, to, &to_ino);
	if (err && err != EXT2_ET_FILE_NOT_FOUND) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	if (err == EXT2_ET_FILE_NOT_FOUND)
		to_ino = 0;

	/* Already the same file? */
	if (to_ino != 0 && to_ino == from_ino) {
		ret = 0;
		goto out;
	}

	ret = check_inum_access(ff, from_ino, W_OK);
	if (ret)
		goto out;

	if (to_ino) {
		ret = check_inum_access(ff, to_ino, W_OK);
		if (ret)
			goto out;
	}

	temp_to = strdup(to);
	if (!temp_to) {
		ret = -ENOMEM;
		goto out;
	}

	temp_from = strdup(from);
	if (!temp_from) {
		ret = -ENOMEM;
		goto out2;
	}

	/* Find parent dir of the source and check write access */
	cp = strrchr(temp_from, '/');
	if (!cp) {
		ret = -EINVAL;
		goto out2;
	}

	a = *(cp + 1);
	*(cp + 1) = 0;
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_from,
			   &from_dir_ino);
	*(cp + 1) = a;
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}
	if (from_dir_ino == 0) {
		ret = -ENOENT;
		goto out2;
	}

	ret = check_inum_access(ff, from_dir_ino, W_OK);
	if (ret)
		goto out2;

	/* Find parent dir of the destination and check write access */
	cp = strrchr(temp_to, '/');
	if (!cp) {
		ret = -EINVAL;
		goto out2;
	}

	a = *(cp + 1);
	*(cp + 1) = 0;
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_to,
			   &to_dir_ino);
	*(cp + 1) = a;
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}
	if (to_dir_ino == 0) {
		ret = -ENOENT;
		goto out2;
	}

	ret = check_inum_access(ff, to_dir_ino, W_OK);
	if (ret)
		goto out2;

	/* If the target exists, unlink it first */
	if (to_ino != 0) {
		err = ext2fs_read_inode(fs, to_ino, &inode);
		if (err) {
			ret = translate_error(fs, to_ino, err);
			goto out2;
		}

		dbg_printf(ff, "%s: unlinking %s ino=%d\n", __func__,
			   LINUX_S_ISDIR(inode.i_mode) ? "dir" : "file",
			   to_ino);
		if (LINUX_S_ISDIR(inode.i_mode))
			ret = __op_rmdir(ff, to);
		else
			ret = __op_unlink(ff, to);
		if (ret)
			goto out2;
	}

	/* Get ready to do the move */
	err = ext2fs_read_inode(fs, from_ino, &inode);
	if (err) {
		ret = translate_error(fs, from_ino, err);
		goto out2;
	}

	/* Link in the new file */
	dbg_printf(ff, "%s: linking ino=%d/path=%s to dir=%d\n", __func__,
		   from_ino, cp + 1, to_dir_ino);
	err = ext2fs_link(fs, to_dir_ino, cp + 1, from_ino,
			  ext2_file_type(inode.i_mode) | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, to_dir_ino, err);
		goto out2;
	}

	/* Update '..' pointer if dir */
	err = ext2fs_read_inode(fs, from_ino, &inode);
	if (err) {
		ret = translate_error(fs, from_ino, err);
		goto out2;
	}

	if (LINUX_S_ISDIR(inode.i_mode)) {
		ud.new_dotdot = to_dir_ino;
		dbg_printf(ff, "%s: updating .. entry for dir=%d\n", __func__,
			   to_dir_ino);
		err = ext2fs_dir_iterate2(fs, from_ino, 0, NULL,
					  update_dotdot_helper, &ud);
		if (err) {
			ret = translate_error(fs, from_ino, err);
			goto out2;
		}

		/* Decrease from_dir_ino's links_count */
		dbg_printf(ff, "%s: moving linkcount from dir=%d to dir=%d\n",
			   __func__, from_dir_ino, to_dir_ino);
		err = ext2fs_read_inode(fs, from_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, from_dir_ino, err);
			goto out2;
		}
		inode.i_links_count--;
		err = ext2fs_write_inode(fs, from_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, from_dir_ino, err);
			goto out2;
		}

		/* Increase to_dir_ino's links_count */
		err = ext2fs_read_inode(fs, to_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, to_dir_ino, err);
			goto out2;
		}
		inode.i_links_count++;
		err = ext2fs_write_inode(fs, to_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, to_dir_ino, err);
			goto out2;
		}
	}

	/* Update timestamps */
	ret = fuse2fs_update_ctime(ff, from_ino, NULL);
	if (ret)
		goto out2;

	ret = fuse2fs_update_mtime(ff, to_dir_ino, NULL);
	if (ret)
		goto out2;

	/* Remove the old file */
	ret = fuse2fs_unlink(ff, from, NULL);
	if (ret)
		goto out2;

	ret = fuse2fs_dirsync_flush(ff, from_dir_ino, NULL);
	if (ret)
		goto out2;

	if (from_dir_ino != to_dir_ino) {
		ret = fuse2fs_dirsync_flush(ff, to_dir_ino, NULL);
		if (ret)
			goto out2;
	}

out2:
	free(temp_from);
	free(temp_to);
out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_link(const char *src, const char *dest)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	char *temp_path;
	errcode_t err;
	char *node_name, a;
	ext2_ino_t parent, ino;
	struct ext2_inode_large inode;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	if (!ff->can_hardlink)
		return -ENOSYS;

	dbg_printf(ff, "%s: src=%s dest=%s\n", __func__, src, dest);
	temp_path = strdup(dest);
	if (!temp_path) {
		ret = -ENOMEM;
		goto out;
	}
	node_name = strrchr(temp_path, '/');
	if (!node_name) {
		ret = -ENOMEM;
		goto out;
	}
	node_name++;
	a = *node_name;
	*node_name = 0;

	fs = fuse2fs_start(ff);
	if (!fs_can_allocate(ff, 2)) {
		ret = -ENOSPC;
		goto out2;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &parent);
	*node_name = a;
	if (err) {
		err = -ENOENT;
		goto out2;
	}

	ret = check_inum_access(ff, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, src, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	ret = check_iflags_access(ff, ino, EXT2_INODE(&inode), W_OK);
	if (ret)
		goto out2;

	inode.i_links_count++;
	ret = fuse2fs_update_ctime(ff, ino, &inode);
	if (ret)
		goto out2;

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	dbg_printf(ff, "%s: linking ino=%d/name=%s to dir=%d\n", __func__, ino,
		   node_name, parent);
	err = ext2fs_link(fs, parent, node_name, ino,
			  ext2_file_type(inode.i_mode) | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse2fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse2fs_finish(ff, ret);
out:
	free(temp_path);
	return ret;
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
/* Obtain group ids of the process that sent us a command(?) */
static int get_req_groups(struct fuse2fs *ff, gid_t **gids, size_t *nr_gids)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	gid_t *array;
	int nr = 32;	/* nobody has more than 32 groups right? */
	int ret;

	do {
		err = ext2fs_get_array(nr, sizeof(gid_t), &array);
		if (err)
			return translate_error(fs, 0, err);

		ret = fuse_getgroups(nr, array);
		if (ret < 0)
			return ret;

		if (ret <= nr) {
			*gids = array;
			*nr_gids = ret;
			return 0;
		}

		ext2fs_free_mem(&array);
		nr = ret;
	} while (0);

	/* shut up gcc */
	return -ENOMEM;
}

/*
 * Is this file's group id in the set of groups associated with the process
 * that initiated the fuse request?  Returns 1 for yes, 0 for no, or a negative
 * errno.
 */
static int in_file_group(struct fuse_context *ctxt,
			 const struct ext2_inode_large *inode)
{
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	gid_t *gids = NULL;
	size_t i, nr_gids = 0;
	gid_t gid = inode_gid(*inode);
	int ret;

	ret = get_req_groups(ff, &gids, &nr_gids);
	if (ret < 0)
		return ret;

	for (i = 0; i < nr_gids; i++)
		if (gids[i] == gid)
			return 1;
	return 0;
}
#else
static int in_file_group(struct fuse_context *ctxt,
			 const struct ext2_inode_large *inode)
{
	return ctxt->gid == inode_gid(*inode);
}
#endif

static int op_chmod(const char *path, mode_t mode
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, struct fuse_file_info *fi
#endif
			)
{
	struct ext2_inode_large inode;
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t ino;
	mode_t new_mode;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	ret = fuse2fs_file_ino(ff, path, fi, &ino);
	if (ret)
		goto out;
	dbg_printf(ff, "%s: path=%s mode=0%o ino=%d\n", __func__, path, mode, ino);

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	ret = check_iflags_access(ff, ino, EXT2_INODE(&inode), W_OK);
	if (ret)
		goto out;

	if (want_check_owner(ff, ctxt) && ctxt->uid != inode_uid(inode)) {
		ret = -EPERM;
		goto out;
	}

	/*
	 * XXX: We should really check that the inode gid is not in /any/
	 * of the user's groups, but FUSE only tells us about the primary
	 * group.
	 */
	if (!fuse2fs_iomap_does_fileio(ff) && !is_superuser(ff, ctxt)) {
		ret = in_file_group(ctxt, &inode);
		if (ret < 0)
			goto out;

		if (!ret)
			mode &= ~S_ISGID;
	}

	new_mode = (inode.i_mode & ~0xFFF) | (mode & 0xFFF);

	dbg_printf(ff, "%s: path=%s old_mode=0%o new_mode=0%o ino=%d\n",
		   __func__, path, inode.i_mode, new_mode, ino);

	inode.i_mode = new_mode;

	ret = fuse2fs_update_ctime(ff, ino, &inode);
	if (ret)
		goto out;

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_chown(const char *path, uid_t owner, gid_t group
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, struct fuse_file_info *fi
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t ino;
	struct ext2_inode_large inode;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	ret = fuse2fs_file_ino(ff, path, fi, &ino);
	if (ret)
		goto out;
	dbg_printf(ff, "%s: path=%s owner=%d group=%d ino=%d\n", __func__,
		   path, owner, group, ino);

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	ret = check_iflags_access(ff, ino, EXT2_INODE(&inode), W_OK);
	if (ret)
		goto out;

	/* FUSE seems to feed us ~0 to mean "don't change" */
	if (owner != (uid_t) ~0) {
		/* Only root gets to change UID. */
		if (want_check_owner(ff, ctxt) &&
		    !(inode_uid(inode) == ctxt->uid && owner == ctxt->uid)) {
			ret = -EPERM;
			goto out;
		}
		fuse2fs_set_uid(&inode, owner);
	}

	if (group != (gid_t) ~0) {
		/* Only root or the owner get to change GID. */
		if (want_check_owner(ff, ctxt) &&
		    inode_uid(inode) != ctxt->uid) {
			ret = -EPERM;
			goto out;
		}

		/* XXX: We /should/ check group membership but FUSE */
		fuse2fs_set_gid(&inode, group);
	}

	ret = fuse2fs_update_ctime(ff, ino, &inode);
	if (ret)
		goto out;

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int punch_posteof(struct fuse2fs *ff, ext2_ino_t ino, off_t new_size)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	blk64_t truncate_block = FUSE2FS_B_TO_FSB(ff, new_size);
	errcode_t err;

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), 0, truncate_block,
			   ~0ULL);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int truncate_helper(struct fuse2fs *ff, ext2_ino_t ino, off_t new_size)
{
	ext2_filsys fs = ff->fs;
	ext2_file_t file;
	__u64 old_isize;
	errcode_t err;
	int flags = EXT2_FILE_WRITE;
	int ret = 0;

	/* the kernel handles all eof zeroing for us in iomap mode */
	if (fuse2fs_iomap_does_fileio(ff))
		flags |= EXT2_FILE_NOBLOCKIO;

	err = ext2fs_file_open(fs, ino, flags, &file);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_file_get_lsize(file, &old_isize);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

	dbg_printf(ff, "%s: ino=%u isize=0x%llx new_size=0x%llx\n", __func__,
		   ino,
		   (unsigned long long)old_isize,
		   (unsigned long long)new_size);

	err = ext2fs_file_set_size2(file, new_size);
	if (err)
		ret = translate_error(fs, ino, err);

out_close:
	err = ext2fs_file_close(file);
	if (ret)
		return ret;
	if (err)
		return translate_error(fs, ino, err);

	ret = fuse2fs_update_mtime(ff, ino, NULL);
	if (ret)
		return ret;

	/*
	 * Truncating to the current size is usually understood to mean that
	 * we should clear out post-EOF preallocations.
	 */
	if (new_size == old_isize)
		return punch_posteof(ff, ino, new_size);

	return 0;
}

static int op_truncate(const char *path, off_t len
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, struct fuse_file_info *fi
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_ino_t ino;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fuse2fs_start(ff);
	ret = fuse2fs_file_ino(ff, path, fi, &ino);
	if (ret)
		goto out;
	dbg_printf(ff, "%s: ino=%d len=%jd\n", __func__, ino, (intmax_t) len);

	ret = check_inum_access(ff, ino, W_OK);
	if (ret)
		goto out;

	ret = truncate_helper(ff, ino, len);
	if (ret)
		goto out;

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

#ifdef __linux__
static void detect_linux_executable_open(int kernel_flags, int *access_check,
				  int *e2fs_open_flags)
{
	/*
	 * On Linux, execve will bleed __FMODE_EXEC into the file mode flags,
	 * and FUSE is more than happy to let that slip through.
	 */
	if (kernel_flags & 0x20) {
		*access_check = X_OK;
		*e2fs_open_flags &= ~EXT2_FILE_WRITE;
	}
}
#else
static void detect_linux_executable_open(int kernel_flags, int *access_check,
				  int *e2fs_open_flags)
{
	/* empty */
}
#endif /* __linux__ */

static int __op_open(struct fuse2fs *ff, const char *path,
		     struct fuse_file_info *fp)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct fuse2fs_file_handle *file;
	int check = 0, ret = 0;

	dbg_printf(ff, "%s: path=%s oflags=0o%o\n", __func__, path, fp->flags);
	err = ext2fs_get_mem(sizeof(*file), &file);
	if (err)
		return translate_error(fs, 0, err);
	file->magic = FUSE2FS_FILE_MAGIC;

	file->open_flags = 0;
	switch (fp->flags & O_ACCMODE) {
	case O_RDONLY:
		check = R_OK;
		break;
	case O_WRONLY:
		check = W_OK;
		file->open_flags |= EXT2_FILE_WRITE;
		break;
	case O_RDWR:
		check = R_OK | W_OK;
		file->open_flags |= EXT2_FILE_WRITE;
		break;
	}
	/* the kernel handles all block IO for us in iomap mode */
	if (fuse2fs_iomap_does_fileio(ff))
		file->open_flags |= EXT2_FILE_NOBLOCKIO;
	if (fp->flags & O_APPEND)
		check |= A_OK;

	detect_linux_executable_open(fp->flags, &check, &file->open_flags);

	if (fp->flags & O_CREAT)
		file->open_flags |= EXT2_FILE_CREATE;

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &file->ino);
	if (err || file->ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	dbg_printf(ff, "%s: ino=%d\n", __func__, file->ino);

	ret = check_inum_access(ff, file->ino, check);
	if (ret) {
		/*
		 * In a regular (Linux) fs driver, the kernel will open
		 * binaries for reading if the user has --x privileges (i.e.
		 * execute without read).  Since the kernel doesn't have any
		 * way to tell us if it's opening a file via execve, we'll
		 * just assume that allowing access is ok if asking for ro mode
		 * fails but asking for x mode succeeds.  Of course we can
		 * also employ undocumented hacks (see above).
		 */
		if (check == R_OK) {
			ret = check_inum_access(ff, file->ino, X_OK);
			if (ret)
				goto out;
		} else
			goto out;
	}

	if (fp->flags & O_TRUNC) {
		ret = truncate_helper(ff, file->ino, 0);
		if (ret)
			goto out;
	}

	fp->fh = (uintptr_t)file;

out:
	if (ret)
		ext2fs_free_mem(&file);
	return ret;
}

static int op_open(const char *path, struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	int ret;

	FUSE2FS_CHECK_CONTEXT(ff);
	fuse2fs_start(ff);
	ret = __op_open(ff, path, fp);
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_read(const char *path EXT2FS_ATTR((unused)), char *buf,
		   size_t len, off_t offset,
		   struct fuse_file_info *fp)
{
	struct fuse2fs_file_handle fhurk = {
		.magic = FUSE2FS_FILE_MAGIC,
	};
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	ext2_filsys fs;
	ext2_file_t efp;
	errcode_t err;
	unsigned int got = 0;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	if (!fh)
		fh = &fhurk;

	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d off=%jd len=%jd\n", __func__, fh->ino,
		   (intmax_t) offset, len);
	fs = fuse2fs_start(ff);

	if (fh == &fhurk) {
		ret = fuse2fs_file_ino(ff, path, NULL, &fhurk.ino);
		if (ret)
			goto out;
	}

	err = ext2fs_file_open(fs, fh->ino, fh->open_flags, &efp);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out;
	}

	err = ext2fs_file_llseek(efp, offset, SEEK_SET, NULL);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out2;
	}

	err = ext2fs_file_read(efp, buf, len, &got);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out2;
	}

out2:
	err = ext2fs_file_close(efp);
	if (ret)
		goto out;
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out;
	}

	if (fuse2fs_is_writeable(ff)) {
		ret = fuse2fs_update_atime(ff, fh->ino);
		if (ret)
			goto out;
	}
out:
	fuse2fs_finish(ff, ret);
	return got ? (int) got : ret;
}

static int op_write(const char *path EXT2FS_ATTR((unused)),
		    const char *buf, size_t len, off_t offset,
		    struct fuse_file_info *fp)
{
	struct fuse2fs_file_handle fhurk = {
		.magic = FUSE2FS_FILE_MAGIC,
		.open_flags = EXT2_FILE_WRITE,
	};
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	ext2_filsys fs;
	ext2_file_t efp;
	errcode_t err;
	unsigned int got = 0;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	if (!fh)
		fh = &fhurk;

	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d off=%jd len=%jd\n", __func__, fh->ino,
		   (intmax_t) offset, (intmax_t) len);
	fs = fuse2fs_start(ff);
	if (!fuse2fs_is_writeable(ff)) {
		ret = -EROFS;
		goto out;
	}

	if (!fs_can_allocate(ff, FUSE2FS_B_TO_FSB(ff, len))) {
		ret = -ENOSPC;
		goto out;
	}

	if (fh == &fhurk) {
		ret = fuse2fs_file_ino(ff, path, NULL, &fhurk.ino);
		if (ret)
			goto out;
	}

	err = ext2fs_file_open(fs, fh->ino, fh->open_flags, &efp);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out;
	}

	err = ext2fs_file_llseek(efp, offset, SEEK_SET, NULL);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out2;
	}

	err = ext2fs_file_write(efp, buf, len, &got);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out2;
	}

	err = ext2fs_file_flush(efp);
	if (err) {
		got = 0;
		ret = translate_error(fs, fh->ino, err);
		goto out2;
	}

out2:
	err = ext2fs_file_close(efp);
	if (ret)
		goto out;
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out;
	}

	ret = fuse2fs_update_mtime(ff, fh->ino, NULL);
	if (ret)
		goto out;

out:
	fuse2fs_finish(ff, ret);
	return got ? (int) got : ret;
}

static int op_release(const char *path EXT2FS_ATTR((unused)),
		      struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	ext2_filsys fs;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	fs = fuse2fs_start(ff);

	if ((fp->flags & O_SYNC) &&
	    fuse2fs_is_writeable(ff) &&
	    (fh->open_flags & EXT2_FILE_WRITE)) {
		err = ext2fs_flush2(fs, EXT2_FLAG_FLUSH_NO_SYNC);
		if (err)
			ret = translate_error(fs, fh->ino, err);
	}

	fp->fh = 0;
	fuse2fs_finish(ff, ret);

	ext2fs_free_mem(&fh);

	return ret;
}

static int op_fsync(const char *path EXT2FS_ATTR((unused)),
		    int datasync EXT2FS_ATTR((unused)),
		    struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	ext2_filsys fs;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	fs = fuse2fs_start(ff);
	/* For now, flush everything, even if it's slow */
	if (fuse2fs_is_writeable(ff) && fh->open_flags & EXT2_FILE_WRITE) {
		err = ext2fs_flush2(fs, 0);
		if (err)
			ret = translate_error(fs, fh->ino, err);
	}
	fuse2fs_finish(ff, ret);

	return ret;
}

static int op_statfs(const char *path EXT2FS_ATTR((unused)),
		     struct statvfs *buf)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	uint64_t fsid, *f;
	blk64_t overhead, reserved, free;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s\n", __func__, path);
	fs = fuse2fs_start(ff);
	buf->f_bsize = fs->blocksize;
	buf->f_frsize = 0;

	if (ff->minixdf)
		overhead = 0;
	else
		overhead = fs->desc_blocks +
			   (blk64_t)fs->group_desc_count *
			   (fs->inode_blocks_per_group + 2);
	reserved = ext2fs_r_blocks_count(fs->super);
	if (!reserved)
		reserved = ext2fs_blocks_count(fs->super) / 10;
	free = ext2fs_free_blocks_count(fs->super);

	buf->f_blocks = ext2fs_blocks_count(fs->super) - overhead;
	buf->f_bfree = free;
	if (free < reserved)
		buf->f_bavail = 0;
	else
		buf->f_bavail = free - reserved;
	buf->f_files = fs->super->s_inodes_count;
	buf->f_ffree = fs->super->s_free_inodes_count;
	buf->f_favail = fs->super->s_free_inodes_count;
	f = (uint64_t *)fs->super->s_uuid;
	fsid = *f;
	f++;
	fsid ^= *f;
	buf->f_fsid = fsid;
	buf->f_flag = 0;
	if (ff->opstate != F2OP_WRITABLE)
		buf->f_flag |= ST_RDONLY;
	buf->f_namemax = EXT2_NAME_LEN;
	fuse2fs_finish(ff, 0);

	return 0;
}

static const char *valid_xattr_prefixes[] = {
	"user.",
	"trusted.",
	"security.",
	"gnu.",
	"system.",
};

static int validate_xattr_name(const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(valid_xattr_prefixes); i++) {
		if (!strncmp(name, valid_xattr_prefixes[i],
					strlen(valid_xattr_prefixes[i])))
			return 1;
	}

	return 0;
}

static int op_getxattr(const char *path, const char *key, char *value,
		       size_t len)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	void *ptr;
	size_t plen;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	if (!validate_xattr_name(key))
		return -ENODATA;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	dbg_printf(ff, "%s: ino=%d name=%s\n", __func__, ino, key);

	ret = check_inum_access(ff, ino, R_OK);
	if (ret)
		goto out;

	ret = __getxattr(ff, ino, key, &ptr, &plen);
	if (ret)
		goto out;

	if (!len) {
		ret = plen;
	} else if (len < plen) {
		ret = -ERANGE;
	} else {
		memcpy(value, ptr, plen);
		ret = plen;
	}

	ext2fs_free_mem(&ptr);
out:
	fuse2fs_finish(ff, ret);

	return ret;
}

static int count_buffer_space(char *name, char *value EXT2FS_ATTR((unused)),
			      size_t value_len EXT2FS_ATTR((unused)),
			      void *data)
{
	unsigned int *x = data;

	*x = *x + strlen(name) + 1;
	return 0;
}

static int copy_names(char *name, char *value EXT2FS_ATTR((unused)),
		      size_t value_len EXT2FS_ATTR((unused)), void *data)
{
	char **b = data;
	size_t name_len = strlen(name);

	memcpy(*b, name, name_len + 1);
	*b = *b + name_len + 1;

	return 0;
}

static int op_listxattr(const char *path, char *names, size_t len)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	struct ext2_xattr_handle *h;
	unsigned int bufsz;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, ino, err);
		goto out;
	}
	dbg_printf(ff, "%s: ino=%d\n", __func__, ino);

	ret = check_inum_access(ff, ino, R_OK);
	if (ret)
		goto out;

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	err = ext2fs_xattrs_read(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	/* Count buffer space needed for names */
	bufsz = 0;
	err = ext2fs_xattrs_iterate(h, count_buffer_space, &bufsz);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	if (len == 0) {
		ret = bufsz;
		goto out2;
	} else if (len < bufsz) {
		ret = -ERANGE;
		goto out2;
	}

	/* Copy names out */
	memset(names, 0, len);
	err = ext2fs_xattrs_iterate(h, copy_names, &names);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}
	ret = bufsz;
out2:
	err = ext2fs_xattrs_close(&h);
	if (err && !ret)
		ret = translate_error(fs, ino, err);
out:
	fuse2fs_finish(ff, ret);

	return ret;
}

static int op_setxattr(const char *path EXT2FS_ATTR((unused)),
		       const char *key, const char *value,
		       size_t len, int flags)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	struct ext2_xattr_handle *h;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	if (flags & ~(XATTR_CREATE | XATTR_REPLACE))
		return -EOPNOTSUPP;

	if (!validate_xattr_name(key))
		return -EINVAL;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	dbg_printf(ff, "%s: ino=%d name=%s\n", __func__, ino, key);

	ret = check_inum_access(ff, ino, W_OK);
	if (ret == -EACCES) {
		ret = -EPERM;
		goto out;
	} else if (ret)
		goto out;

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	err = ext2fs_xattrs_read(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	if (flags & (XATTR_CREATE | XATTR_REPLACE)) {
		void *buf;
		size_t buflen;

		err = ext2fs_xattr_get(h, key, &buf, &buflen);
		switch (err) {
		case EXT2_ET_EA_KEY_NOT_FOUND:
			if (flags & XATTR_REPLACE) {
				ret = -ENODATA;
				goto out2;
			}
			break;
		case 0:
			ext2fs_free_mem(&buf);
			if (flags & XATTR_CREATE) {
				ret = -EEXIST;
				goto out2;
			}
			break;
		default:
			ret = translate_error(fs, ino, err);
			goto out2;
		}
	}

	err = ext2fs_xattr_set(h, key, value, len);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	ret = fuse2fs_update_ctime(ff, ino, NULL);
out2:
	err = ext2fs_xattrs_close(&h);
	if (!ret && err)
		ret = translate_error(fs, ino, err);
out:
	fuse2fs_finish(ff, ret);

	return ret;
}

static int op_removexattr(const char *path, const char *key)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	struct ext2_xattr_handle *h;
	void *buf;
	size_t buflen;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	/*
	 * Once in a while libfuse gives us a no-name xattr to delete as part
	 * of clearing ACLs.  Just pretend we cleared them.
	 */
	if (key[0] == 0)
		return 0;

	if (!validate_xattr_name(key))
		return -ENODATA;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	if (!fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	dbg_printf(ff, "%s: ino=%d name=%s\n", __func__, ino, key);

	ret = check_inum_access(ff, ino, W_OK);
	if (ret)
		goto out;

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	err = ext2fs_xattrs_read(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	err = ext2fs_xattr_get(h, key, &buf, &buflen);
	switch (err) {
	case EXT2_ET_EA_KEY_NOT_FOUND:
		/*
		 * ACLs are special snowflakes that require a 0 return when
		 * the ACL never existed in the first place.
		 */
		if (!strncmp(XATTR_SECURITY_PREFIX, key,
			     XATTR_SECURITY_PREFIX_LEN))
			ret = 0;
		else
			ret = -ENODATA;
		goto out2;
	case 0:
		ext2fs_free_mem(&buf);
		break;
	default:
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	err = ext2fs_xattr_remove(h, key);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

	ret = fuse2fs_update_ctime(ff, ino, NULL);
out2:
	err = ext2fs_xattrs_close(&h);
	if (err && !ret)
		ret = translate_error(fs, ino, err);
out:
	fuse2fs_finish(ff, ret);

	return ret;
}

struct readdir_iter {
	void *buf;
	ext2_filsys fs;
	fuse_fill_dir_t func;

	struct fuse2fs *ff;
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
	enum fuse_readdir_flags flags;
#endif
	unsigned int nr;
	off_t startpos;
	off_t dirpos;
};

static inline mode_t dirent_fmode(ext2_filsys fs,
				   const struct ext2_dir_entry *dirent)
{
	if (!ext2fs_has_feature_filetype(fs->super))
		return 0;

	switch (ext2fs_dirent_file_type(dirent)) {
	case EXT2_FT_REG_FILE:
		return S_IFREG;
	case EXT2_FT_DIR:
		return S_IFDIR;
	case EXT2_FT_CHRDEV:
		return S_IFCHR;
	case EXT2_FT_BLKDEV:
		return S_IFBLK;
	case EXT2_FT_FIFO:
		return S_IFIFO;
	case EXT2_FT_SOCK:
		return S_IFSOCK;
	case EXT2_FT_SYMLINK:
		return S_IFLNK;
	}

	return 0;
}

static int op_readdir_iter(ext2_ino_t dir EXT2FS_ATTR((unused)),
			   int entry EXT2FS_ATTR((unused)),
			   struct ext2_dir_entry *dirent,
			   int offset EXT2FS_ATTR((unused)),
			   int blocksize EXT2FS_ATTR((unused)),
			   char *buf EXT2FS_ATTR((unused)), void *data)
{
	struct readdir_iter *i = data;
	char namebuf[EXT2_NAME_LEN + 1];
	struct stat stat = {
		.st_ino = dirent->inode,
		.st_mode = dirent_fmode(i->fs, dirent),
	};
	int ret;

	i->dirpos++;
	if (i->startpos >= i->dirpos)
		return 0;

	dbg_printf(i->ff, "READDIR%s %u dirpos %llu\n",
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			i->flags == FUSE_READDIR_PLUS ? "PLUS" : "",
#else
			"",
#endif
			i->nr++,
			(unsigned long long)i->dirpos);

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
	if (i->flags == FUSE_READDIR_PLUS) {
		ret = fuse2fs_stat(i->ff, dirent->inode, &stat);
		if (ret)
			return DIRENT_ABORT;
	}
#endif

	memcpy(namebuf, dirent->name, dirent->name_len & 0xFF);
	namebuf[dirent->name_len & 0xFF] = 0;
	ret = i->func(i->buf, namebuf, &stat, i->dirpos
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, 0
#endif
			);
	if (ret)
		return DIRENT_ABORT;

	return 0;
}

static int op_readdir(const char *path EXT2FS_ATTR((unused)),
		      void *buf, fuse_fill_dir_t fill_func,
		      off_t offset,
		      struct fuse_file_info *fp
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, enum fuse_readdir_flags flags
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	errcode_t err;
	struct readdir_iter i = {
		.ff = ff,
		.dirpos = 0,
		.startpos = offset,
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
		.flags = flags,
#endif
	};
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d offset=%llu\n", __func__, fh->ino,
			(unsigned long long)offset);
	i.fs = fuse2fs_start(ff);
	i.buf = buf;
	i.func = fill_func;
	err = ext2fs_dir_iterate2(i.fs, fh->ino, 0, NULL, op_readdir_iter, &i);
	if (err) {
		ret = translate_error(i.fs, fh->ino, err);
		goto out;
	}

	if (fuse2fs_is_writeable(ff)) {
		ret = fuse2fs_update_atime(ff, fh->ino);
		if (ret)
			goto out;
	}
out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_access(const char *path, int mask)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t ino;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s mask=0x%x\n", __func__, path, mask);
	fs = fuse2fs_start(ff);
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err || ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	ret = check_inum_access(ff, ino, mask);
	if (ret)
		goto out;

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_create(const char *path, mode_t mode, struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	ext2_ino_t parent, child;
	char *temp_path;
	errcode_t err;
	char *node_name, a;
	int filetype;
	struct ext2_inode_large inode;
	gid_t gid;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s mode=0%o\n", __func__, path, mode);
	temp_path = strdup(path);
	if (!temp_path) {
		ret = -ENOMEM;
		goto out;
	}
	node_name = strrchr(temp_path, '/');
	if (!node_name) {
		ret = -ENOMEM;
		goto out;
	}
	node_name++;
	a = *node_name;
	*node_name = 0;

	fs = fuse2fs_start(ff);
	if (!fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out2;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, temp_path,
			   &parent);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	ret = check_inum_access(ff, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse2fs_new_child_gid(ff, parent, &gid, NULL);
	if (err)
		goto out2;

	*node_name = a;

	filetype = ext2_file_type(mode);

	err = ext2fs_new_inode(fs, parent, mode, 0, &child);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	dbg_printf(ff, "%s: creating ino=%d/name=%s in dir=%d\n", __func__, child,
		   node_name, parent);
	err = ext2fs_link(fs, parent, node_name, child,
			  filetype | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse2fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	memset(&inode, 0, sizeof(inode));
	inode.i_mode = mode;
	inode.i_links_count = 1;
	fuse2fs_set_extra_isize(ff, child, &inode);
	fuse2fs_set_uid(&inode, ctxt->uid);
	fuse2fs_set_gid(&inode, gid);
	if (ext2fs_has_feature_extents(fs->super)) {
		ext2_extent_handle_t handle;

		inode.i_flags &= ~EXT4_EXTENTS_FL;
		ret = ext2fs_extent_open2(fs, child,
					  EXT2_INODE(&inode), &handle);
		if (ret) {
			ret = translate_error(fs, child, err);
			goto out2;
		}

		ext2fs_extent_free(handle);
	}

	err = ext2fs_write_new_inode(fs, child, EXT2_INODE(&inode));
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	inode.i_generation = ff->next_generation++;
	fuse2fs_init_timestamps(ff, child, &inode);
	err = fuse2fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ext2fs_inode_alloc_stats2(fs, child, 1, 0);

	ret = propagate_default_acls(ff, parent, child);
	if (ret)
		goto out2;

	fp->flags &= ~O_TRUNC;
	ret = __op_open(ff, path, fp);
	if (ret)
		goto out2;

	ret = fuse2fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse2fs_finish(ff, ret);
out:
	free(temp_path);
	return ret;
}

#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 0)
static int op_ftruncate(const char *path EXT2FS_ATTR((unused)),
			off_t len, struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	ext2_filsys fs;
	ext2_file_t efp;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d len=%jd\n", __func__, fh->ino,
		   (intmax_t) len);
	fs = fuse2fs_start(ff);
	if (!fuse2fs_is_writeable(ff)) {
		ret = -EROFS;
		goto out;
	}

	err = ext2fs_file_open(fs, fh->ino, fh->open_flags, &efp);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out;
	}

	err = ext2fs_file_set_size2(efp, len);
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out2;
	}

out2:
	err = ext2fs_file_close(efp);
	if (ret)
		goto out;
	if (err) {
		ret = translate_error(fs, fh->ino, err);
		goto out;
	}

	ret = fuse2fs_update_mtime(ff, fh->ino, NULL);
	if (ret)
		goto out;

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int op_fgetattr(const char *path EXT2FS_ATTR((unused)),
		       struct stat *statbuf,
		       struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	fs = fuse2fs_start(ff);
	ret = fuse2fs_stat(ff, fh->ino, statbuf);
	fuse2fs_finish(ff, ret);

	return ret;
}
#endif /* FUSE_VERSION < FUSE_MAKE_VERSION(3, 0) */

static int op_utimens(const char *path, const struct timespec ctv[2]
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
			, struct fuse_file_info *fi
#endif
			)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct timespec tv[2];
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t ino;
	struct ext2_inode_large inode;
	int access = W_OK;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	ret = fuse2fs_file_ino(ff, path, fi, &ino);
	if (ret)
		goto out;
	dbg_printf(ff, "%s: ino=%d atime=%lld.%ld mtime=%lld.%ld\n", __func__,
			ino,
			(long long int)ctv[0].tv_sec, ctv[0].tv_nsec,
			(long long int)ctv[1].tv_sec, ctv[1].tv_nsec);

	/*
	 * ext4 allows timestamp updates of append-only files but only if we're
	 * setting to current time.  If iomap is enabled, the kernel does the
	 * permission checking for timestamp updates and we can skip the check.
	 */
	if (ctv[0].tv_nsec == UTIME_NOW && ctv[1].tv_nsec == UTIME_NOW)
		access |= A_OK;
	ret = fuse2fs_iomap_enabled(ff) ? 0 : check_inum_access(ff, ino, access);
	if (ret)
		goto out;

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	tv[0] = ctv[0];
	tv[1] = ctv[1];
#ifdef UTIME_NOW
	if (tv[0].tv_nsec == UTIME_NOW)
		fuse2fs_get_now(ff, tv);
	if (tv[1].tv_nsec == UTIME_NOW)
		fuse2fs_get_now(ff, tv + 1);
#endif /* UTIME_NOW */
#ifdef UTIME_OMIT
	if (tv[0].tv_nsec != UTIME_OMIT)
		EXT4_INODE_SET_XTIME(i_atime, &tv[0], &inode);
	if (tv[1].tv_nsec != UTIME_OMIT)
		EXT4_INODE_SET_XTIME(i_mtime, &tv[1], &inode);
#endif /* UTIME_OMIT */
	ret = fuse2fs_update_ctime(ff, ino, &inode);
	if (ret)
		goto out;

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

#define FUSE2FS_MODIFIABLE_IFLAGS \
	(EXT2_FL_USER_MODIFIABLE & ~(EXT4_EXTENTS_FL | EXT4_CASEFOLD_FL | \
				     EXT3_JOURNAL_DATA_FL))

static inline int set_iflags(struct ext2_inode_large *inode, __u32 iflags)
{
	if ((inode->i_flags ^ iflags) & ~FUSE2FS_MODIFIABLE_IFLAGS)
		return -EINVAL;

	inode->i_flags = (inode->i_flags & ~FUSE2FS_MODIFIABLE_IFLAGS) |
			 (iflags & FUSE2FS_MODIFIABLE_IFLAGS);
	return 0;
}

#ifdef SUPPORT_I_FLAGS
static int ioctl_getflags(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			  void *data)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	*(__u32 *)data = inode.i_flags & EXT2_FL_USER_VISIBLE;
	return 0;
}

static int ioctl_setflags(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			  void *data)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret;
	__u32 flags = *(__u32 *)data;
	struct fuse_context *ctxt = fuse_get_context();

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	if (want_check_owner(ff, ctxt) && inode_uid(inode) != ctxt->uid)
		return -EPERM;

	ret = set_iflags(&inode, flags);
	if (ret)
		return ret;

	ret = fuse2fs_update_ctime(ff, fh->ino, &inode);
	if (ret)
		return ret;

	err = fuse2fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}

static int ioctl_getversion(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			    void *data)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	*(__u32 *)data = inode.i_generation;
	return 0;
}

static int ioctl_setversion(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			    void *data)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret;
	__u32 generation = *(__u32 *)data;
	struct fuse_context *ctxt = fuse_get_context();

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	if (want_check_owner(ff, ctxt) && inode_uid(inode) != ctxt->uid)
		return -EPERM;

	inode.i_generation = generation;

	ret = fuse2fs_update_ctime(ff, fh->ino, &inode);
	if (ret)
		return ret;

	err = fuse2fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}
#endif /* SUPPORT_I_FLAGS */

#ifdef FS_IOC_FSGETXATTR
static __u32 iflags_to_fsxflags(__u32 iflags)
{
	__u32 xflags = 0;

	if (iflags & FS_SYNC_FL)
		xflags |= FS_XFLAG_SYNC;
	if (iflags & FS_IMMUTABLE_FL)
		xflags |= FS_XFLAG_IMMUTABLE;
	if (iflags & FS_APPEND_FL)
		xflags |= FS_XFLAG_APPEND;
	if (iflags & FS_NODUMP_FL)
		xflags |= FS_XFLAG_NODUMP;
	if (iflags & FS_NOATIME_FL)
		xflags |= FS_XFLAG_NOATIME;
	if (iflags & FS_DAX_FL)
		xflags |= FS_XFLAG_DAX;
	if (iflags & FS_PROJINHERIT_FL)
		xflags |= FS_XFLAG_PROJINHERIT;
	return xflags;
}

static int ioctl_fsgetxattr(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			    void *data)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	struct fsxattr *fsx = data;
	unsigned int inode_size;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	memset(fsx, 0, sizeof(*fsx));
	inode_size = EXT2_GOOD_OLD_INODE_SIZE + inode.i_extra_isize;
	if (ext2fs_inode_includes(inode_size, i_projid))
		fsx->fsx_projid = inode_projid(inode);
	fsx->fsx_xflags = iflags_to_fsxflags(inode.i_flags);
	return 0;
}

static __u32 fsxflags_to_iflags(__u32 xflags)
{
	__u32 iflags = 0;

	if (xflags & FS_XFLAG_IMMUTABLE)
		iflags |= FS_IMMUTABLE_FL;
	if (xflags & FS_XFLAG_APPEND)
		iflags |= FS_APPEND_FL;
	if (xflags & FS_XFLAG_SYNC)
		iflags |= FS_SYNC_FL;
	if (xflags & FS_XFLAG_NOATIME)
		iflags |= FS_NOATIME_FL;
	if (xflags & FS_XFLAG_NODUMP)
		iflags |= FS_NODUMP_FL;
	if (xflags & FS_XFLAG_DAX)
		iflags |= FS_DAX_FL;
	if (xflags & FS_XFLAG_PROJINHERIT)
		iflags |= FS_PROJINHERIT_FL;
	return iflags;
}

static int ioctl_fssetxattr(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			    void *data)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret;
	struct fuse_context *ctxt = fuse_get_context();
	struct fsxattr *fsx = data;
	__u32 flags = fsxflags_to_iflags(fsx->fsx_xflags);
	unsigned int inode_size;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	if (want_check_owner(ff, ctxt) && inode_uid(inode) != ctxt->uid)
		return -EPERM;

	ret = set_iflags(&inode, flags);
	if (ret)
		return ret;

	inode_size = EXT2_GOOD_OLD_INODE_SIZE + inode.i_extra_isize;
	if (ext2fs_inode_includes(inode_size, i_projid))
		inode.i_projid = fsx->fsx_projid;

	ret = fuse2fs_update_ctime(ff, fh->ino, &inode);
	if (ret)
		return ret;

	err = fuse2fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}
#endif /* FS_IOC_FSGETXATTR */

#ifdef FITRIM
static int ioctl_fitrim(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			void *data)
{
	ext2_filsys fs = ff->fs;
	struct fstrim_range *fr = data;
	blk64_t start, end, max_blocks, b, cleared, minlen;
	blk64_t max_blks = ext2fs_blocks_count(fs->super);
	errcode_t err = 0;

	if (!fuse2fs_is_writeable(ff))
		return -EROFS;

	start = FUSE2FS_B_TO_FSBT(ff, fr->start);
	if (fr->len == -1ULL)
		end = -1ULL;
	else
		end = FUSE2FS_B_TO_FSBT(ff, fr->start + fr->len - 1);
	minlen = FUSE2FS_B_TO_FSBT(ff, fr->minlen);

	if (EXT2FS_NUM_B2C(fs, minlen) > EXT2_CLUSTERS_PER_GROUP(fs->super) ||
	    start >= max_blks ||
	    fr->len < fs->blocksize)
		return -EINVAL;

	dbg_printf(ff, "%s: start=%llu end=%llu minlen=%llu\n", __func__,
		   start, end, minlen);

	if (start < fs->super->s_first_data_block)
		start = fs->super->s_first_data_block;

	if (end < fs->super->s_first_data_block)
		end = fs->super->s_first_data_block;
	if (end >= ext2fs_blocks_count(fs->super))
		end = ext2fs_blocks_count(fs->super) - 1;

	cleared = 0;
	max_blocks = FUSE2FS_B_TO_FSBT(ff, 2048ULL * 1024 * 1024);

	fr->len = 0;
	while (start <= end) {
		err = ext2fs_find_first_zero_block_bitmap2(fs->block_map,
							   start, end, &start);
		switch (err) {
		case 0:
			break;
		case ENOENT:
			/* no free blocks found, so we're done */
			err = 0;
			goto out;
		default:
			return translate_error(fs, fh->ino, err);
		}

		b = start + max_blocks < end ? start + max_blocks : end;
		err =  ext2fs_find_first_set_block_bitmap2(fs->block_map,
							   start, b, &b);
		if (err && err != ENOENT)
			return translate_error(fs, fh->ino, err);
		if (b - start >= minlen) {
			err = io_channel_discard(fs->io, start, b - start);
			if (err)
				return translate_error(fs, fh->ino, err);
			cleared += b - start;
			fr->len = FUSE2FS_FSB_TO_B(ff, cleared);
		}
		start = b + 1;
	}

out:
	fr->len = FUSE2FS_FSB_TO_B(ff, cleared);
	dbg_printf(ff, "%s: len=%llu err=%ld\n", __func__, fr->len, err);
	return err;
}
#endif /* FITRIM */

#ifndef EXT4_IOC_SHUTDOWN
# define EXT4_IOC_SHUTDOWN	_IOR('X', 125, __u32)
#endif

static int ioctl_shutdown(struct fuse2fs *ff, struct fuse2fs_file_handle *fh,
			  void *data)
{
	ext2_filsys fs = ff->fs;

	err_printf(ff, "%s.\n", _("shut down requested"));

	/*
	 * EXT4_IOC_SHUTDOWN inherited the inverted polarity on the ioctl
	 * direction from XFS.  Unfortunately, that means we can't implement
	 * any of the flags.  Flush whatever is dirty and shut down.
	 */
	if (ff->opstate == F2OP_WRITABLE)
		ext2fs_flush2(fs, 0);
	ff->opstate = F2OP_SHUTDOWN;

	return 0;
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
static int op_ioctl(const char *path EXT2FS_ATTR((unused)),
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 0)
		    unsigned int cmd,
#else
		    int cmd,
#endif
		    void *arg EXT2FS_ATTR((unused)),
		    struct fuse_file_info *fp,
		    unsigned int flags EXT2FS_ATTR((unused)), void *data)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	fuse2fs_start(ff);
	switch ((unsigned long) cmd) {
#ifdef SUPPORT_I_FLAGS
	case EXT2_IOC_GETFLAGS:
		ret = ioctl_getflags(ff, fh, data);
		break;
	case EXT2_IOC_SETFLAGS:
		ret = ioctl_setflags(ff, fh, data);
		break;
	case EXT2_IOC_GETVERSION:
		ret = ioctl_getversion(ff, fh, data);
		break;
	case EXT2_IOC_SETVERSION:
		ret = ioctl_setversion(ff, fh, data);
		break;
#endif
#ifdef FS_IOC_FSGETXATTR
	case FS_IOC_FSGETXATTR:
		ret = ioctl_fsgetxattr(ff, fh, data);
		break;
	case FS_IOC_FSSETXATTR:
		ret = ioctl_fssetxattr(ff, fh, data);
		break;
#endif
#ifdef FITRIM
	case FITRIM:
		ret = ioctl_fitrim(ff, fh, data);
		break;
#endif
	case EXT4_IOC_SHUTDOWN:
		ret = ioctl_shutdown(ff, fh, data);
		break;
	default:
		dbg_printf(ff, "%s: Unknown ioctl %d\n", __func__, cmd);
		ret = -ENOTTY;
	}
	fuse2fs_finish(ff, ret);

	return ret;
}
#endif /* FUSE 28 */

static int op_bmap(const char *path, size_t blocksize EXT2FS_ATTR((unused)),
		   uint64_t *idx)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	fs = fuse2fs_start(ff);
	err = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, path, &ino);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	dbg_printf(ff, "%s: ino=%d blk=%"PRIu64"\n", __func__, ino, *idx);

	err = ext2fs_bmap2(fs, ino, NULL, NULL, 0, *idx, 0, (blk64_t *)idx);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

out:
	fuse2fs_finish(ff, ret);
	return ret;
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 9)
# ifdef SUPPORT_FALLOCATE
static int fuse2fs_allocate_range(struct fuse2fs *ff,
				  struct fuse2fs_file_handle *fh, int mode,
				  off_t offset, off_t len)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	blk64_t start, end;
	__u64 fsize;
	errcode_t err;
	int flags;

	start = FUSE2FS_B_TO_FSBT(ff, offset);
	end = FUSE2FS_B_TO_FSBT(ff, offset + len - 1);
	dbg_printf(ff, "%s: ino=%d mode=0x%x offset=0x%jx len=0x%jx start=%llu end=%llu\n",
		   __func__, fh->ino, mode, offset, len, start, end);
	if (!fs_can_allocate(ff, FUSE2FS_B_TO_FSB(ff, len)))
		return -ENOSPC;

	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return err;
	fsize = EXT2_I_SIZE(&inode);

	/* Allocate a bunch of blocks */
	flags = (mode & FL_KEEP_SIZE_FLAG ? 0 :
			EXT2_FALLOCATE_INIT_BEYOND_EOF);
	err = ext2fs_fallocate(fs, flags, fh->ino,
			       EXT2_INODE(&inode),
			       ~0ULL, start, end - start + 1);
	if (err && err != EXT2_ET_BLOCK_ALLOC_FAIL)
		return translate_error(fs, fh->ino, err);

	/* Update i_size */
	if (!(mode & FL_KEEP_SIZE_FLAG)) {
		if ((__u64) offset + len > fsize) {
			err = ext2fs_inode_size_set(fs,
						EXT2_INODE(&inode),
						offset + len);
			if (err)
				return translate_error(fs, fh->ino, err);
		}
	}

	err = fuse2fs_update_mtime(ff, fh->ino, &inode);
	if (err)
		return err;

	err = fuse2fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return err;
}

static errcode_t clean_block_middle(struct fuse2fs *ff, ext2_ino_t ino,
				    struct ext2_inode_large *inode,
				    off_t offset, off_t len, char **buf)
{
	ext2_filsys fs = ff->fs;
	blk64_t blk;
	off_t residue = FUSE2FS_OFF_IN_FSB(ff, offset);
	int retflags;
	errcode_t err;

	/* the kernel does this for us in iomap mode */
	if (fuse2fs_iomap_does_fileio(ff))
		return 0;

	if (!*buf) {
		err = ext2fs_get_mem(fs->blocksize, buf);
		if (err)
			return err;
	}

	err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), *buf, 0,
			   FUSE2FS_B_TO_FSBT(ff, offset), &retflags, &blk);
	if (err)
		return err;
	if (!blk || (retflags & BMAP_RET_UNINIT))
		return 0;

	err = io_channel_read_tagblk(fs->io, ino, blk, 1, *buf);
	if (err)
		return err;

	dbg_printf(ff, "%s: ino=%d offset=0x%jx len=0x%jx\n", __func__, ino, offset + residue, len);
	memset(*buf + residue, 0, len);

	return io_channel_write_tagblk(fs->io, ino, blk, 1, *buf);
}

static errcode_t clean_block_edge(struct fuse2fs *ff, ext2_ino_t ino,
				  struct ext2_inode_large *inode, off_t offset,
				  int clean_before, char **buf)
{
	ext2_filsys fs = ff->fs;
	blk64_t blk;
	int retflags;
	off_t residue;
	errcode_t err;

	/* the kernel does this for us in iomap mode */
	if (fuse2fs_iomap_does_fileio(ff))
		return 0;

	residue = FUSE2FS_OFF_IN_FSB(ff, offset);
	if (residue == 0)
		return 0;

	if (!*buf) {
		err = ext2fs_get_mem(fs->blocksize, buf);
		if (err)
			return err;
	}

	err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), *buf, 0,
			   FUSE2FS_B_TO_FSBT(ff, offset), &retflags, &blk);
	if (err)
		return err;

	err = io_channel_read_tagblk(fs->io, ino, blk, 1, *buf);
	if (err)
		return err;
	if (!blk || (retflags & BMAP_RET_UNINIT))
		return 0;

	if (clean_before) {
		dbg_printf(ff, "%s: ino=%d before offset=0x%jx len=0x%jx\n",
			   __func__, ino, offset, residue);
		memset(*buf, 0, residue);
	} else {
		dbg_printf(ff, "%s: ino=%d after offset=0x%jx len=0x%jx\n",
			   __func__, ino, offset, fs->blocksize - residue);
		memset(*buf + residue, 0, fs->blocksize - residue);
	}

	return io_channel_write_tagblk(fs->io, ino, blk, 1, *buf);
}

static int fuse2fs_punch_range(struct fuse2fs *ff,
			       struct fuse2fs_file_handle *fh, int mode,
			       off_t offset, off_t len)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	blk64_t start, end;
	errcode_t err;
	char *buf = NULL;

	/* kernel ext4 punch requires this flag to be set */
	if (!(mode & FL_KEEP_SIZE_FLAG))
		return -EINVAL;

	/*
	 * Unmap out all full blocks in the middle of the range being punched.
	 * The start of the unmap range should be the first byte of the first
	 * fsblock that starts within the range.  The end of the range should
	 * be the next byte after the last fsblock to end in the range.
	 */
	start = FUSE2FS_B_TO_FSBT(ff, round_up(offset, fs->blocksize));
	end = FUSE2FS_B_TO_FSBT(ff, round_down(offset + len, fs->blocksize));

	dbg_printf(ff,
 "%s: ino=%d mode=0x%x offset=0x%jx len=0x%jx start=0x%llx end=0x%llx\n",
		   __func__, fh->ino, mode, offset, len, start, end);

	err = fuse2fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	/* Zero everything before the first block and after the last block */
	if (FUSE2FS_B_TO_FSBT(ff, offset) == FUSE2FS_B_TO_FSBT(ff, offset + len))
		err = clean_block_middle(ff, fh->ino, &inode, offset,
					 len, &buf);
	else {
		err = clean_block_edge(ff, fh->ino, &inode, offset, 0, &buf);
		if (!err)
			err = clean_block_edge(ff, fh->ino, &inode,
					       offset + len, 1, &buf);
	}
	if (buf)
		ext2fs_free_mem(&buf);
	if (err)
		return translate_error(fs, fh->ino, err);

	/*
	 * Unmap full blocks in the middle, which is to say that start - end
	 * must be at least one fsblock.  ext2fs_punch takes a closed interval
	 * as its argument, so we pass [start, end - 1].
	 */
	if (start < end) {
		err = ext2fs_punch(fs, fh->ino, EXT2_INODE(&inode),
				   NULL, start, end - 1);
		if (err)
			return translate_error(fs, fh->ino, err);
	}

	err = fuse2fs_update_mtime(ff, fh->ino, &inode);
	if (err)
		return err;

	err = fuse2fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}

static int fuse2fs_zero_range(struct fuse2fs *ff,
			      struct fuse2fs_file_handle *fh, int mode,
			      off_t offset, off_t len)
{
	int ret = fuse2fs_punch_range(ff, fh, mode | FL_KEEP_SIZE_FLAG, offset,
				      len);

	if (!ret)
		ret = fuse2fs_allocate_range(ff, fh, mode, offset, len);
	return ret;
}

static int op_fallocate(const char *path EXT2FS_ATTR((unused)), int mode,
			off_t offset, off_t len,
			struct fuse_file_info *fp)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse2fs_file_handle *fh =
		(struct fuse2fs_file_handle *)(uintptr_t)fp->fh;
	int ret;

	/* Catch unknown flags */
	if (mode & ~(FL_ZERO_RANGE_FLAG | FL_PUNCH_HOLE_FLAG | FL_KEEP_SIZE_FLAG))
		return -EOPNOTSUPP;

	FUSE2FS_CHECK_CONTEXT(ff);
	FUSE2FS_CHECK_HANDLE(ff, fh);
	fuse2fs_start(ff);
	if (!fuse2fs_is_writeable(ff)) {
		ret = -EROFS;
		goto out;
	}

	dbg_printf(ff, "%s: ino=%d mode=0x%x start=0x%llx end=0x%llx\n", __func__,
		   fh->ino, mode,
		   (unsigned long long)offset,
		   (unsigned long long)offset + len);

	if (mode & FL_ZERO_RANGE_FLAG)
		ret = fuse2fs_zero_range(ff, fh, mode, offset, len);
	else if (mode & FL_PUNCH_HOLE_FLAG)
		ret = fuse2fs_punch_range(ff, fh, mode, offset, len);
	else
		ret = fuse2fs_allocate_range(ff, fh, mode, offset, len);
out:
	fuse2fs_finish(ff, ret);

	return ret;
}
# endif /* SUPPORT_FALLOCATE */
#endif /* FUSE 29 */

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 18)
static int op_syncfs(const char *path)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);
	dbg_printf(ff, "%s: path=%s\n", __func__, path);
	fs = fuse2fs_start(ff);

	if (ff->opstate == F2OP_WRITABLE) {
		if (fs->super->s_error_count)
			fs->super->s_state |= EXT2_ERROR_FS;
		ext2fs_mark_super_dirty(fs);
		err = ext2fs_set_gdt_csum(fs);
		if (err) {
			ret = translate_error(fs, 0, err);
			goto out_unlock;
		}

		err = ext2fs_flush2(fs, 0);
		if (err) {
			ret = translate_error(fs, 0, err);
			goto out_unlock;
		}
	}

	/*
	 * When iomap is enabled, the kernel will call syncfs right before
	 * calling the destroy method.  If any syncfs succeeds, then we know
	 * that there will be a last syncfs and that it will write the GDT, so
	 * destroy doesn't need to waste time doing that.
	 */
	if (fuse2fs_iomap_enabled(ff))
		ff->write_gdt_on_destroy = 0;

out_unlock:
	fuse2fs_finish(ff, ret);
	return ret;
}
#endif

#ifdef HAVE_FUSE_IOMAP
static void fuse2fs_iomap_hole(struct fuse2fs *ff, struct fuse_iomap *iomap,
			       off_t pos, uint64_t count)
{
	iomap->dev = FUSE_IOMAP_DEV_NULL;
	iomap->addr = FUSE_IOMAP_NULL_ADDR;
	iomap->offset = pos;
	iomap->length = count;
	iomap->type = FUSE_IOMAP_TYPE_HOLE;
}

static void fuse2fs_iomap_hole_to_eof(struct fuse2fs *ff,
				      struct fuse_iomap *iomap, off_t pos,
				      off_t count,
				      const struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	uint64_t isize = EXT2_I_SIZE(inode);

	/*
	 * We have to be careful about handling a hole to the right of the
	 * entire mapping tree.  First, the mapping must start and end on a
	 * block boundary because they must be aligned to at least an LBA for
	 * the block layer; and to the fsblock for smoother operation.
	 *
	 * As for the length -- we could return a mapping all the way to
	 * i_size, but i_size could be less than pos/count if we're zeroing the
	 * EOF block in anticipation of a truncate operation.  Similarly, we
	 * don't want to end the mapping at pos+count because we know there's
	 * nothing mapped byeond here.
	 */
	uint64_t startoff = round_down(pos, fs->blocksize);
	uint64_t eofoff = round_up(max(pos + count, isize), fs->blocksize);

	dbg_printf(ff,
 "pos=0x%llx count=0x%llx isize=0x%llx startoff=0x%llx eofoff=0x%llx\n",
		   (unsigned long long)pos,
		   (unsigned long long)count,
		   (unsigned long long)isize,
		   (unsigned long long)startoff,
		   (unsigned long long)eofoff);

	fuse2fs_iomap_hole(ff, iomap, startoff, eofoff - startoff);
}

#define DEBUG_IOMAP
#ifdef DEBUG_IOMAP
# define __DUMP_EXTENT(ff, func, tag, startoff, err, extent) \
	do { \
		dbg_printf((ff), \
 "%s: %s startoff 0x%llx err %ld lblk 0x%llx pblk 0x%llx len 0x%x flags 0x%x\n", \
			   (func), (tag), (startoff), (err), (extent)->e_lblk, \
			   (extent)->e_pblk, (extent)->e_len, \
			   (extent)->e_flags & EXT2_EXTENT_FLAGS_UNINIT); \
	} while(0)
# define DUMP_EXTENT(ff, tag, startoff, err, extent) \
	__DUMP_EXTENT((ff), __func__, (tag), (startoff), (err), (extent))
#else
# define __DUMP_EXTENT(...)	((void)0)
# define DUMP_EXTENT(...)	((void)0)
#endif

static inline errcode_t __fuse2fs_get_mapping_at(struct fuse2fs *ff,
						 ext2_extent_handle_t handle,
						 blk64_t startoff,
						 struct ext2fs_extent *bmap,
						 const char *func)
{
	errcode_t err;

	/*
	 * Find the file mapping at startoff.  We don't check the return value
	 * of _goto because _get will error out if _goto failed.  There's a
	 * subtlety to the outcome of _goto when startoff falls in a sparse
	 * hole however:
	 *
	 * Most of the time, _goto points the cursor at the mapping whose lblk
	 * is just to the left of startoff.  The mapping may or may not overlap
	 * startoff; this is ok.  In other words, the tree lookup behaves as if
	 * we asked it to use a less than or equals comparison.
	 *
	 * However, if startoff is to the left of the first mapping in the
	 * extent tree, _goto points the cursor at that first mapping because
	 * it doesn't know how to deal with this situation.  In this case,
	 * the tree lookup behaves as if we asked it to use a greater than
	 * or equals comparison.
	 *
	 * Note: If _get() returns 'no current node', that means that there
	 * aren't any mappings at all.
	 */
	ext2fs_extent_goto(handle, startoff);
	err = ext2fs_extent_get(handle, EXT2_EXTENT_CURRENT, bmap);
	__DUMP_EXTENT(ff, func, "lookup", startoff, err, bmap);
	if (err == EXT2_ET_NO_CURRENT_NODE)
		err = EXT2_ET_EXTENT_NOT_FOUND;
	return err;
}

static inline errcode_t __fuse2fs_get_next_mapping(struct fuse2fs *ff,
						   ext2_extent_handle_t handle,
						   blk64_t startoff,
						   struct ext2fs_extent *bmap,
						   const char *func)
{
	struct ext2fs_extent newex, errex;
	errcode_t err;

	err = ext2fs_extent_get(handle, EXT2_EXTENT_NEXT_LEAF, &newex);
	DUMP_EXTENT(ff, "NEXT", startoff, err, &newex);
	if (err == EXT2_ET_EXTENT_NO_NEXT)
		return EXT2_ET_EXTENT_NOT_FOUND;
	if (err)
		return err;

	/*
	 * Try to get the next leaf mapping.  There's a weird and longstanding
	 * "feature" of EXT2_EXTENT_NEXT_LEAF where walking off the end of the
	 * mapping recordset causes it to wrap around to the beginning of the
	 * extent map and we end up with a mapping to the left of the one that
	 * was passed in.
	 *
	 * However, a corrupt extent tree could also have such a record.  The
	 * only way to be sure is to retrieve the mapping for the extreme right
	 * edge of the tree and compare it to the mapping that the caller gave
	 * us.  If they match, then we've hit the end.  If not, something is
	 * corrupt in the ondisk metadata.
	 */
	if (newex.e_lblk <= bmap->e_lblk + bmap->e_len) {
		err = __fuse2fs_get_mapping_at(ff, handle, ~0U, &errex, func);
		if (err)
			return err;

		if (memcmp(bmap, &errex, sizeof(errex)) != 0)
			return EXT2_ET_INODE_CORRUPTED;

		return EXT2_ET_EXTENT_NOT_FOUND;
	}

	*bmap = newex;
	return 0;
}

#define fuse2fs_get_mapping_at(ff, handle, startoff, bmap) \
	__fuse2fs_get_mapping_at((ff), (handle), (startoff), (bmap), __func__)
#define fuse2fs_get_next_mapping(ff, handle, startoff, bmap) \
	__fuse2fs_get_next_mapping((ff), (handle), (startoff), (bmap), __func__)

static errcode_t fuse2fs_iomap_begin_extent(struct fuse2fs *ff, uint64_t ino,
					    struct ext2_inode_large *inode,
					    off_t pos, uint64_t count,
					    uint32_t opflags,
					    struct fuse_iomap *iomap)
{
	ext2_extent_handle_t handle;
	struct ext2fs_extent extent;
	ext2_filsys fs = ff->fs;
	const blk64_t startoff = FUSE2FS_B_TO_FSBT(ff, pos);
	errcode_t err;
	int ret = 0;

	err = ext2fs_extent_open2(fs, ino, EXT2_INODE(inode), &handle);
	if (err)
		return translate_error(fs, ino, err);

	err = fuse2fs_get_mapping_at(ff, handle, startoff, &extent);
	if (err == EXT2_ET_EXTENT_NOT_FOUND) {
		/* No mappings at all; the whole range is a hole. */
		fuse2fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);
		goto out_handle;
	}
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_handle;
	}

	if (startoff < extent.e_lblk) {
		/*
		 * Mapping starts to the right of the current position.
		 * Synthesize a hole going to that next extent.
		 */
		fuse2fs_iomap_hole(ff, iomap, FUSE2FS_FSB_TO_B(ff, startoff),
				FUSE2FS_FSB_TO_B(ff, extent.e_lblk - startoff));
		goto out_handle;
	}

	if (startoff >= extent.e_lblk + extent.e_len) {
		/*
		 * Mapping ends to the left of the current position.  Try to
		 * find the next mapping.  If there is no next mapping, the
		 * whole range is in a hole.
		 */
		err = fuse2fs_get_next_mapping(ff, handle, startoff, &extent);
		if (err == EXT2_ET_EXTENT_NOT_FOUND) {
			fuse2fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);
			goto out_handle;
		}

		/*
		 * If the new mapping starts to the right of startoff, there's
		 * a hole from startoff to the start of the new mapping.
		 */
		if (startoff < extent.e_lblk) {
			fuse2fs_iomap_hole(ff, iomap,
				FUSE2FS_FSB_TO_B(ff, startoff),
				FUSE2FS_FSB_TO_B(ff, extent.e_lblk - startoff));
			goto out_handle;
		}

		/*
		 * The new mapping starts at startoff.  Something weird
		 * happened in the extent tree lookup, but we found a valid
		 * mapping so we'll run with it.
		 */
	}

	/* Mapping overlaps startoff, report this. */
	iomap->dev = ff->iomap_dev;
	iomap->addr = FUSE2FS_FSB_TO_B(ff, extent.e_pblk);
	iomap->offset = FUSE2FS_FSB_TO_B(ff, extent.e_lblk);
	iomap->length = FUSE2FS_FSB_TO_B(ff, extent.e_len);
	if (extent.e_flags & EXT2_EXTENT_FLAGS_UNINIT)
		iomap->type = FUSE_IOMAP_TYPE_UNWRITTEN;
	else
		iomap->type = FUSE_IOMAP_TYPE_MAPPED;

out_handle:
	ext2fs_extent_free(handle);
	return ret;
}

static int fuse2fs_iomap_begin_indirect(struct fuse2fs *ff, uint64_t ino,
					struct ext2_inode_large *inode,
					off_t pos, uint64_t count,
					uint32_t opflags,
					struct fuse_iomap *iomap)
{
	ext2_filsys fs = ff->fs;
	blk64_t startoff = FUSE2FS_B_TO_FSBT(ff, pos);
	uint64_t real_count = min(count, 131072);
	const blk64_t endoff = FUSE2FS_B_TO_FSB(ff, pos + real_count);
	blk64_t startblock;
	errcode_t err;

	err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), NULL, 0, startoff, NULL,
			   &startblock);
	if (err)
		return translate_error(fs, ino, err);

	iomap->offset = pos;
	iomap->flags |= FUSE_IOMAP_F_MERGED;
	if (startblock) {
		iomap->dev = ff->iomap_dev;
		iomap->addr = FUSE2FS_FSB_TO_B(ff, startblock);
		iomap->type = FUSE_IOMAP_TYPE_MAPPED;
	} else {
		iomap->dev = FUSE_IOMAP_DEV_NULL;
		iomap->addr = FUSE_IOMAP_NULL_ADDR;
		iomap->type = FUSE_IOMAP_TYPE_HOLE;
	}
	iomap->length = fs->blocksize;

	/* See how long the mapping goes for. */
	for (startoff++; startoff < endoff; startoff++) {
		blk64_t prev_startblock = startblock;

		err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), NULL, 0,
				   startoff, NULL, &startblock);
		if (err)
			break;

		if (iomap->type == FUSE_IOMAP_TYPE_MAPPED) {
			if (startblock == prev_startblock + 1)
				iomap->length += fs->blocksize;
			else
				break;
		} else {
			if (startblock != 0)
				break;
		}
	}

	return 0;
}

static int fuse2fs_iomap_begin_inline(struct fuse2fs *ff, ext2_ino_t ino,
				      struct ext2_inode_large *inode, off_t pos,
				      uint64_t count, struct fuse_iomap *iomap)
{
	uint64_t one_fsb = FUSE2FS_FSB_TO_B(ff, 1);

	if (pos >= one_fsb) {
		fuse2fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);
	} else {
		/* ext4 only supports inline data files up to 1 fsb */
		iomap->dev = FUSE_IOMAP_DEV_NULL;
		iomap->addr = FUSE_IOMAP_NULL_ADDR;
		iomap->offset = 0;
		iomap->length = one_fsb;
		iomap->type = FUSE_IOMAP_TYPE_INLINE;
	}

	return 0;
}

static int fuse2fs_iomap_begin_report(struct fuse2fs *ff, ext2_ino_t ino,
				      struct ext2_inode_large *inode,
				      off_t pos, uint64_t count,
				      uint32_t opflags,
				      struct fuse_iomap *read_iomap)
{
	if (inode->i_flags & EXT4_INLINE_DATA_FL)
		return fuse2fs_iomap_begin_inline(ff, ino, inode, pos, count,
						  read_iomap);

	if (inode->i_flags & EXT4_EXTENTS_FL)
		return fuse2fs_iomap_begin_extent(ff, ino, inode, pos, count,
						  opflags, read_iomap);

	return fuse2fs_iomap_begin_indirect(ff, ino, inode, pos, count,
					    opflags, read_iomap);
}

static int fuse2fs_iomap_begin_read(struct fuse2fs *ff, ext2_ino_t ino,
				    struct ext2_inode_large *inode, off_t pos,
				    uint64_t count, uint32_t opflags,
				    struct fuse_iomap *read_iomap)
{
	errcode_t err;

	/* fall back to slow path for inline data reads */
	if (inode->i_flags & EXT4_INLINE_DATA_FL)
		return fuse2fs_iomap_begin_inline(ff, ino, inode, pos, count,
						  read_iomap);

	/* flush dirty io_channel buffers to disk before iomap reads them */
	if (!fuse2fs_iomap_does_fileio(ff)) {
		err = io_channel_flush_tag(ff->fs->io, ino);
		if (err)
			return translate_error(ff->fs, ino, err);
	}

	if (inode->i_flags & EXT4_EXTENTS_FL)
		return fuse2fs_iomap_begin_extent(ff, ino, inode, pos, count,
						  opflags, read_iomap);

	return fuse2fs_iomap_begin_indirect(ff, ino, inode, pos, count,
					    opflags, read_iomap);
}

static int fuse2fs_iomap_write_allocate(struct fuse2fs *ff, ext2_ino_t ino,
				     struct ext2_inode_large *inode, off_t pos,
				     uint64_t count, uint32_t opflags, struct
				     fuse_iomap *read_iomap, bool *dirty)
{
	ext2_filsys fs = ff->fs;
	blk64_t startoff = FUSE2FS_B_TO_FSBT(ff, pos);
	blk64_t stopoff = FUSE2FS_B_TO_FSB(ff, pos + count);
	errcode_t err;
	int ret;

	dbg_printf(ff, "%s: write_alloc ino=%u startoff 0x%llx blockcount 0x%llx\n",
		   __func__, ino, startoff, stopoff - startoff);

	if (!fs_can_allocate(ff, stopoff - startoff))
		return -ENOSPC;

	err = ext2fs_fallocate(fs, EXT2_FALLOCATE_FORCE_UNINIT, ino,
			       EXT2_INODE(inode), ~0ULL, startoff,
			       stopoff - startoff);
	if (err)
		return translate_error(fs, ino, err);

	/* pick up the newly allocated mapping */
	ret = fuse2fs_iomap_begin_read(ff, ino, inode, pos, count, opflags,
				       read_iomap);
	if (ret)
		return ret;

	read_iomap->flags |= FUSE_IOMAP_F_DIRTY;
	*dirty = true;
	return 0;
}

static off_t fuse2fs_max_file_size(const struct fuse2fs *ff,
				   const struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	blk64_t addr_per_block, max_map_block;

	if (inode->i_flags & EXT4_EXTENTS_FL) {
		max_map_block = (1ULL << 32) - 1;
	} else {
		addr_per_block = fs->blocksize >> 2;
		max_map_block = addr_per_block;
		max_map_block += addr_per_block * addr_per_block;
		max_map_block += addr_per_block * addr_per_block * addr_per_block;
		max_map_block += 12;
	}

	return FUSE2FS_FSB_TO_B(ff, max_map_block) + (fs->blocksize - 1);
}

static int fuse2fs_iomap_begin_write(struct fuse2fs *ff, ext2_ino_t ino,
				     struct ext2_inode_large *inode, off_t pos,
				     uint64_t count, uint32_t opflags,
				     struct fuse_iomap *read_iomap,
				     bool *dirty)
{
	off_t max_size = fuse2fs_max_file_size(ff, inode);
	errcode_t err;
	int ret;

	if (pos >= max_size)
		return -EFBIG;

	if (pos >= max_size - count)
		count = max_size - pos;

	ret = fuse2fs_iomap_begin_read(ff, ino, inode, pos, count, opflags,
				       read_iomap);
	if (ret)
		return ret;

	if (read_iomap->type == FUSE_IOMAP_TYPE_HOLE &&
	    !(opflags & FUSE_IOMAP_OP_ZERO)) {
		ret = fuse2fs_iomap_write_allocate(ff, ino, inode, pos, count,
						   opflags, read_iomap, dirty);
		if (ret)
			return ret;
	}

	/*
	 * flush and invalidate the file's io_channel buffers before iomap
	 * writes them
	 */
	if (!fuse2fs_iomap_does_fileio(ff)) {
		err = io_channel_invalidate_tag(ff->fs->io, ino);
		if (err)
			return translate_error(ff->fs, ino, err);
	}

	return 0;
}

static int op_iomap_begin(const char *path, uint64_t nodeid, uint64_t attr_ino,
			  off_t pos, uint64_t count, uint32_t opflags,
			  struct fuse_iomap *read_iomap,
			  struct fuse_iomap *write_iomap)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct fuse_session *se = fuse_get_session(ctxt->fuse);
	struct ext2_inode_large inode;
	ext2_filsys fs;
	errcode_t err;
	bool dirty = false;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	dbg_printf(ff,
 "%s: path=%s nodeid=%llu attr_ino=%llu pos=0x%llx count=0x%llx opflags=0x%x\n",
		   __func__, path,
		   (unsigned long long)nodeid,
		   (unsigned long long)attr_ino,
		   (unsigned long long)pos,
		   (unsigned long long)count,
		   opflags);

	fs = fuse2fs_start(ff);
	err = fuse2fs_read_inode(fs, attr_ino, &inode);
	if (err) {
		ret = translate_error(fs, attr_ino, err);
		goto out_unlock;
	}

	if (opflags & FUSE_IOMAP_OP_REPORT)
		ret = fuse2fs_iomap_begin_report(ff, attr_ino, &inode, pos,
						 count, opflags, read_iomap);
	else if (opflags & (FUSE_IOMAP_OP_WRITE | FUSE_IOMAP_OP_ZERO))
		ret = fuse2fs_iomap_begin_write(ff, attr_ino, &inode, pos,
						count, opflags, read_iomap,
						&dirty);
	else
		ret = fuse2fs_iomap_begin_read(ff, attr_ino, &inode, pos,
					       count, opflags, read_iomap);
	if (ret)
		goto out_unlock;

	dbg_printf(ff, "%s: nodeid=%llu attr_ino=%llu pos=0x%llx -> addr=0x%llx offset=0x%llx length=0x%llx type=%u\n",
		   __func__,
		   (unsigned long long)nodeid,
		   (unsigned long long)attr_ino,
		   (unsigned long long)pos,
		   (unsigned long long)read_iomap->addr,
		   (unsigned long long)read_iomap->offset,
		   (unsigned long long)read_iomap->length,
		   read_iomap->type);

	if (dirty) {
		err = fuse2fs_write_inode(fs, attr_ino, &inode);
		if (err) {
			ret = translate_error(fs, attr_ino, err);
			goto out_unlock;
		}
	}

	/*
	 * Cache the mapping in the kernel so that we can reuse them for
	 * subsequent IO.  Note that we have to return NULL mappings to the
	 * kernel to prompt it to re-try the cache.
	 */
	write_iomap->type = FUSE_IOMAP_TYPE_NULL;
	err = fuse_lowlevel_notify_iomap_upsert(se, nodeid, attr_ino,
						read_iomap, write_iomap);
	if (err) {
		ret = translate_error(fs, attr_ino, err);
		goto out_unlock;
	}

	/* Null out the read mapping to encourage a retry. */
	read_iomap->type = FUSE_IOMAP_TYPE_NULL;
	read_iomap->dev = FUSE_IOMAP_DEV_NULL;
	read_iomap->addr = FUSE_IOMAP_NULL_ADDR;

out_unlock:
	fuse2fs_finish(ff, ret);
	return ret;
}

static int fuse2fs_iomap_append_setsize(struct fuse2fs *ff, ext2_ino_t ino,
					loff_t newsize)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	ext2_off64_t isize;
	errcode_t err;

	dbg_printf(ff, "%s: ino=%u newsize=%llu\n", __func__, ino,
		   (unsigned long long)newsize);

	err = fuse2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	isize = EXT2_I_SIZE(&inode);
	if (newsize <= isize)
		return 0;

	dbg_printf(ff, "%s: ino=%u oldsize=%llu newsize=%llu\n", __func__, ino,
		   (unsigned long long)isize,
		   (unsigned long long)newsize);

	/*
	 * XXX cheesily update the ondisk size even though we only want to do
	 * the incore size until writeback happens
	 */
	err = ext2fs_inode_size_set(fs, EXT2_INODE(&inode), newsize);
	if (err)
		return translate_error(fs, ino, err);

	err = fuse2fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int op_iomap_end(const char *path, uint64_t nodeid, uint64_t attr_ino,
			off_t pos, uint64_t count, uint32_t opflags,
			ssize_t written, const struct fuse_iomap *iomap)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	dbg_printf(ff,
 "%s: path=%s nodeid=%llu attr_ino=%llu pos=0x%llx count=0x%llx opflags=0x%x written=0x%zx mapflags 0x%x\n",
		   __func__, path,
		   (unsigned long long)nodeid,
		   (unsigned long long)attr_ino,
		   (unsigned long long)pos,
		   (unsigned long long)count,
		   opflags,
		   written,
		   iomap->flags);

	fuse2fs_start(ff);

	/* XXX is this really necessary? */
	if ((opflags & FUSE_IOMAP_OP_WRITE) &&
	    !(opflags & FUSE_IOMAP_OP_DIRECT) &&
	    (iomap->flags & FUSE_IOMAP_F_SIZE_CHANGED) &&
	    written > 0) {
		ret = fuse2fs_iomap_append_setsize(ff, attr_ino, pos + written);
		if (ret)
			goto out_unlock;
	}

out_unlock:
	fuse2fs_finish(ff, ret);
	return ret;
}

/*
 * Maximal extent format file size.
 * Resulting logical blkno at s_maxbytes must fit in our on-disk
 * extent format containers, within a sector_t, and within i_blocks
 * in the vfs.  ext4 inode has 48 bits of i_block in fsblock units,
 * so that won't be a limiting factor.
 *
 * However there is other limiting factor. We do store extents in the form
 * of starting block and length, hence the resulting length of the extent
 * covering maximum file size must fit into on-disk format containers as
 * well. Given that length is always by 1 unit bigger than max unit (because
 * we count 0 as well) we have to lower the s_maxbytes by one fs block.
 *
 * Note, this does *not* consider any metadata overhead for vfs i_blocks.
 */
static off_t fuse2fs_max_size(struct fuse2fs *ff, off_t upper_limit)
{
	off_t res;

	if (!ext2fs_has_feature_huge_file(ff->fs->super)) {
		upper_limit = (1LL << 32) - 1;

		/* total blocks in file system block size */
		upper_limit >>= (ff->blocklog - 9);
		upper_limit <<= ff->blocklog;
	}

	/*
	 * 32-bit extent-start container, ee_block. We lower the maxbytes
	 * by one fs block, so ee_len can cover the extent of maximum file
	 * size
	 */
	res = (1LL << 32) - 1;
	res <<= ff->blocklog;

	/* Sanity check against vm- & vfs- imposed limits */
	if (res > upper_limit)
		res = upper_limit;

	return res;
}

/*
 * Set the block device's blocksize to the fs blocksize.
 *
 * This is required to avoid creating uptodate bdev pagecache that aliases file
 * data blocks because iomap reads and writes directly to file data blocks.
 */
static int fuse2fs_set_bdev_blocksize(struct fuse2fs *ff, int fd)
{
	int blocksize = ff->fs->blocksize;
	int set_error;
	int ret;

	ret = ioctl(fd, BLKBSZSET, &blocksize);
	if (!ret)
		return 0;

	/*
	 * Save the original errno so we can report that if the block device
	 * blocksize isn't set in an agreeable way.
	 */
	set_error = errno;

	ret = ioctl(fd, BLKBSZGET, &blocksize);
	if (ret)
		goto out_bad;

	if (blocksize > ff->fs->blocksize)
		set_error = -EINVAL;

	return 0;
out_bad:
	err_printf(ff, "%s: cannot set blocksize %u: %s\n", __func__,
		   blocksize, strerror(set_error));
	return -EIO;
}

static errcode_t fuse2fs_iomap_config_devices(struct fuse_context *ctxt,
					      struct fuse2fs *ff)
{
	struct fuse_session *se = fuse_get_session(ctxt->fuse);
	errcode_t err;
	int fd;
	int ret;

	err = io_channel_fd(ff->fs->io, &fd);
	if (err)
		return err;

	ret = fuse2fs_set_bdev_blocksize(ff, fd);
	if (ret)
		return ret;

	ret = fuse_iomap_add_device(se, fd, 0);

	dbg_printf(ff, "%s: registering iomap dev fd=%d ret=%d iomap_dev=%u\n",
		   __func__, fd, ret, ff->iomap_dev);

	if (ret < 1)
		return -EIO;

	ff->iomap_dev = ret;
	return 0;
}

static int op_iomap_config(uint32_t flags, off_t maxbytes,
			   struct fuse_iomap_config *cfg)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	ext2_filsys fs;
	errcode_t err;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	dbg_printf(ff, "%s: flags=0x%x maxbytes=0x%llx\n", __func__, flags,
		   (long long)maxbytes);
	fs = fuse2fs_start(ff);

	cfg->flags |= FUSE_IOMAP_CONFIG_UUID;
	memcpy(cfg->s_uuid, fs->super->s_uuid, sizeof(cfg->s_uuid));
	cfg->s_uuid_len = sizeof(fs->super->s_uuid);

	cfg->flags |= FUSE_IOMAP_CONFIG_BLOCKSIZE;
	cfg->s_blocksize = FUSE2FS_FSB_TO_B(ff, 1);

	/*
	 * If there inode is large enough to house i_[acm]time_extra then we
	 * can turn on nanosecond timestamps; i_crtime was the next field added
	 * after i_atime_extra.
	 */
	cfg->flags |= FUSE_IOMAP_CONFIG_TIME;
	if (fs->super->s_inode_size >=
	    offsetof(struct ext2_inode_large, i_crtime)) {
		cfg->s_time_gran = 1;
		cfg->s_time_max = EXT4_EXTRA_TIMESTAMP_MAX;
	} else {
		cfg->s_time_gran = NSEC_PER_SEC;
		cfg->s_time_max = EXT4_NON_EXTRA_TIMESTAMP_MAX;
	}
	cfg->s_time_min = EXT4_TIMESTAMP_MIN;

	cfg->flags |= FUSE_IOMAP_CONFIG_MAXBYTES;
	cfg->s_maxbytes = fuse2fs_max_size(ff, maxbytes);

	err = fuse2fs_iomap_config_devices(ctxt, ff);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out_unlock;
	}

out_unlock:
	fuse2fs_finish(ff, ret);
	return ret;
}

static inline bool fuse2fs_can_merge_mappings(const struct ext2fs_extent *left,
					      const struct ext2fs_extent *right)
{
	uint64_t max_len = (left->e_flags & EXT2_EXTENT_FLAGS_UNINIT) ?
				EXT_UNINIT_MAX_LEN : EXT_INIT_MAX_LEN;

	return left->e_lblk + left->e_len == right->e_lblk &&
	       left->e_pblk + left->e_len == right->e_pblk &&
	       (left->e_flags & EXT2_EXTENT_FLAGS_UNINIT) ==
	        (right->e_flags & EXT2_EXTENT_FLAGS_UNINIT) &&
	       (uint64_t)left->e_len + right->e_len <= max_len;
}

static int fuse2fs_try_merge_mappings(struct fuse2fs *ff, ext2_ino_t ino,
				      ext2_extent_handle_t handle,
				      blk64_t startoff)
{
	ext2_filsys fs = ff->fs;
	struct ext2fs_extent left, right;
	errcode_t err;

	/* Look up the mappings before startoff */
	err = fuse2fs_get_mapping_at(ff, handle, startoff - 1, &left);
	if (err == EXT2_ET_EXTENT_NOT_FOUND)
		return 0;
	if (err)
		return translate_error(fs, ino, err);

	/* Look up the mapping at startoff */
	err = fuse2fs_get_mapping_at(ff, handle, startoff, &right);
	if (err == EXT2_ET_EXTENT_NOT_FOUND)
		return 0;
	if (err)
		return translate_error(fs, ino, err);

	/* Can we combine them? */
	if (!fuse2fs_can_merge_mappings(&left, &right))
		return 0;

	/*
	 * Delete the mapping after startoff because libext2fs cannot handle
	 * overlapping mappings.
	 */
	err = ext2fs_extent_delete(handle, 0);
	DUMP_EXTENT(ff, "remover", startoff, err, &right);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_extent_fix_parents(handle);
	DUMP_EXTENT(ff, "fixremover", startoff, err, &right);
	if (err)
		return translate_error(fs, ino, err);

	/* Move back and lengthen the mapping before startoff */
	err = ext2fs_extent_goto(handle, left.e_lblk);
	DUMP_EXTENT(ff, "movel", startoff - 1, err, &left);
	if (err)
		return translate_error(fs, ino, err);

	left.e_len += right.e_len;
	err = ext2fs_extent_replace(handle, 0, &left);
	DUMP_EXTENT(ff, "replacel", startoff - 1, err, &left);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_extent_fix_parents(handle);
	DUMP_EXTENT(ff, "fixreplacel", startoff - 1, err, &left);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse2fs_convert_unwritten_mapping(struct fuse2fs *ff,
					     ext2_ino_t ino,
					     struct ext2_inode_large *inode,
					     ext2_extent_handle_t handle,
					     blk64_t *cursor, blk64_t stopoff)
{
	ext2_filsys fs = ff->fs;
	struct ext2fs_extent extent;
	blk64_t startoff = *cursor;
	errcode_t err;

	/*
	 * Find the mapping at startoff.  Note that we can find holes because
	 * the mapping data can change due to racing writes.
	 */
	err = fuse2fs_get_mapping_at(ff, handle, startoff, &extent);
	if (err == EXT2_ET_EXTENT_NOT_FOUND) {
		/*
		 * If we didn't find any mappings at all then the file is
		 * completely sparse.  There's nothing to convert.
		 */
		*cursor = stopoff;
		return 0;
	}
	if (err)
		return translate_error(fs, ino, err);

	/*
	 * The mapping is completely to the left of the range that we want.
	 * Let's see what's in the next extent, if there is one.
	 */
	if (startoff >= extent.e_lblk + extent.e_len) {
		/*
		 * Mapping ends to the left of the current position.  Try to
		 * find the next mapping.  If there is no next mapping, then
		 * we're done.
		 */
		err = fuse2fs_get_next_mapping(ff, handle, startoff, &extent);
		if (err == EXT2_ET_EXTENT_NOT_FOUND) {
			*cursor = stopoff;
			return 0;
		}
		if (err)
			return translate_error(fs, ino, err);
	}

	/*
	 * The mapping is completely to the right of the range that we want,
	 * so we're done.
	 */
	if (extent.e_lblk >= stopoff) {
		*cursor = stopoff;
		return 0;
	}

	/*
	 * At this point, we have a mapping that overlaps (startoff, stopoff].
	 * If the mapping is already written, move on to the next one.
	 */
	if (!(extent.e_flags & EXT2_EXTENT_FLAGS_UNINIT))
		goto next;

	if (startoff > extent.e_lblk) {
		struct ext2fs_extent newex = extent;

		/*
		 * Unwritten mapping starts before startoff.  Shorten
		 * the previous mapping...
		 */
		newex.e_len = startoff - extent.e_lblk;
		err = ext2fs_extent_replace(handle, 0, &newex);
		DUMP_EXTENT(ff, "shortenp", startoff, err, &newex);
		if (err)
			return translate_error(fs, ino, err);

		err = ext2fs_extent_fix_parents(handle);
		DUMP_EXTENT(ff, "fixshortenp", startoff, err, &newex);
		if (err)
			return translate_error(fs, ino, err);

		/* ...and create new written mapping at startoff. */
		extent.e_len -= newex.e_len;
		extent.e_lblk += newex.e_len;
		extent.e_pblk += newex.e_len;
		extent.e_flags = newex.e_flags & ~EXT2_EXTENT_FLAGS_UNINIT;

		err = ext2fs_extent_insert(handle,
					   EXT2_EXTENT_INSERT_AFTER,
					   &extent);
		DUMP_EXTENT(ff, "insertx", startoff, err, &extent);
		if (err)
			return translate_error(fs, ino, err);

		err = ext2fs_extent_fix_parents(handle);
		DUMP_EXTENT(ff, "fixinsertx", startoff, err, &extent);
		if (err)
			return translate_error(fs, ino, err);
	}

	if (extent.e_lblk + extent.e_len > stopoff) {
		struct ext2fs_extent newex = extent;

		/*
		 * Unwritten mapping ends after stopoff.  Shorten the current
		 * mapping...
		 */
		extent.e_len = stopoff - extent.e_lblk;
		extent.e_flags &= ~EXT2_EXTENT_FLAGS_UNINIT;

		err = ext2fs_extent_replace(handle, 0, &extent);
		DUMP_EXTENT(ff, "shortenn", startoff, err, &extent);
		if (err)
			return translate_error(fs, ino, err);

		err = ext2fs_extent_fix_parents(handle);
		DUMP_EXTENT(ff, "fixshortenn", startoff, err, &extent);
		if (err)
			return translate_error(fs, ino, err);

		/* ..and create a new unwritten mapping at stopoff. */
		newex.e_pblk += extent.e_len;
		newex.e_lblk += extent.e_len;
		newex.e_len -= extent.e_len;
		newex.e_flags |= EXT2_EXTENT_FLAGS_UNINIT;

		err = ext2fs_extent_insert(handle,
					   EXT2_EXTENT_INSERT_AFTER,
					   &newex);
		DUMP_EXTENT(ff, "insertn", startoff, err, &newex);
		if (err)
			return translate_error(fs, ino, err);

		err = ext2fs_extent_fix_parents(handle);
		DUMP_EXTENT(ff, "fixinsertn", startoff, err, &newex);
		if (err)
			return translate_error(fs, ino, err);
	}

	/* Still unwritten?  Update the state. */
	if (extent.e_flags & EXT2_EXTENT_FLAGS_UNINIT) {
		extent.e_flags &= ~EXT2_EXTENT_FLAGS_UNINIT;

		err = ext2fs_extent_replace(handle, 0, &extent);
		DUMP_EXTENT(ff, "replacex", startoff, err, &extent);
		if (err)
			return translate_error(fs, ino, err);

		err = ext2fs_extent_fix_parents(handle);
		DUMP_EXTENT(ff, "fixreplacex", startoff, err, &extent);
		if (err)
			return translate_error(fs, ino, err);
	}

next:
	/* Try to merge with the previous extent */
	if (startoff > 0) {
		err = fuse2fs_try_merge_mappings(ff, ino, handle, startoff);
		if (err)
			return translate_error(fs, ino, err);
	}

	*cursor = extent.e_lblk + extent.e_len;
	return 0;
}

static int fuse2fs_convert_unwritten_mappings(struct fuse2fs *ff,
					      ext2_ino_t ino,
					      struct ext2_inode_large *inode,
					      off_t pos, size_t written)
{
	ext2_extent_handle_t handle;
	ext2_filsys fs = ff->fs;
	blk64_t startoff = FUSE2FS_B_TO_FSBT(ff, pos);
	const blk64_t stopoff = FUSE2FS_B_TO_FSB(ff, pos + written);
	errcode_t err;
	int ret;

	err = ext2fs_extent_open2(fs, ino, EXT2_INODE(inode), &handle);
	if (err)
		return translate_error(fs, ino, err);

	/* Walk every mapping in the range, converting them. */
	while (startoff < stopoff) {
		blk64_t old_startoff = startoff;

		ret = fuse2fs_convert_unwritten_mapping(ff, ino, inode, handle,
							&startoff, stopoff);
		if (ret)
			goto out_handle;
		if (startoff <= old_startoff) {
			/* Do not go backwards. */
			ret = translate_error(fs, ino, EXT2_ET_INODE_CORRUPTED);
			goto out_handle;
		}
	}

	/* Try to merge the right edge */
	ret = fuse2fs_try_merge_mappings(ff, ino, handle, stopoff);
out_handle:
	ext2fs_extent_free(handle);
	return ret;
}

static int op_iomap_ioend(const char *path, uint64_t nodeid, uint64_t attr_ino,
			  off_t pos, size_t written, uint32_t ioendflags,
			  int error, uint64_t new_addr)
{
	struct fuse_context *ctxt = fuse_get_context();
	struct fuse2fs *ff = (struct fuse2fs *)ctxt->private_data;
	struct ext2_inode_large inode;
	ext2_filsys fs;
	errcode_t err;
	bool dirty = false;
	int ret = 0;

	FUSE2FS_CHECK_CONTEXT(ff);

	dbg_printf(ff,
 "%s: path=%s nodeid=%llu attr_ino=%llu pos=0x%llx written=0x%zx ioendflags=0x%x error=%d new_addr=%llu\n",
		   __func__, path,
		   (unsigned long long)nodeid,
		   (unsigned long long)attr_ino,
		   (unsigned long long)pos,
		   written,
		   ioendflags,
		   error,
		   (unsigned long long)new_addr);

	fs = fuse2fs_start(ff);
	if (error) {
		ret = error;
		goto out_unlock;
	}

	/*
	 * flush and invalidate the file's io_channel buffers again now that
	 * iomap wrote them
	 */
	if (written > 0 && !fuse2fs_iomap_does_fileio(ff)) {
		err = io_channel_invalidate_tag(ff->fs->io, attr_ino);
		if (err) {
			ret = translate_error(ff->fs, attr_ino, err);
			goto out_unlock;
		}
	}

	/* should never see these ioend types */
	if ((ioendflags & FUSE_IOMAP_IOEND_SHARED) ||
	    new_addr != FUSE_IOMAP_NULL_ADDR) {
		ret = translate_error(fs, attr_ino,
				      EXT2_ET_FILESYSTEM_CORRUPTED);
		goto out_unlock;
	}

	err = fuse2fs_read_inode(fs, attr_ino, &inode);
	if (err) {
		ret = translate_error(fs, attr_ino, err);
		goto out_unlock;
	}

	if (ioendflags & FUSE_IOMAP_IOEND_UNWRITTEN) {
		/* unwritten extents are only supported on extents files */
		if (!(inode.i_flags & EXT4_EXTENTS_FL)) {
			ret = translate_error(fs, attr_ino,
					      EXT2_ET_FILESYSTEM_CORRUPTED);
			goto out_unlock;
		}

		ret = fuse2fs_convert_unwritten_mappings(ff, attr_ino, &inode,
							 pos, written);
		if (ret)
			goto out_unlock;

		dirty = true;
	}

	if (ioendflags & FUSE_IOMAP_IOEND_APPEND) {
		ext2_off64_t isize = EXT2_I_SIZE(&inode);

		if (pos + written > isize) {
			err = ext2fs_inode_size_set(fs, EXT2_INODE(&inode),
						    pos + written);
			if (err) {
				ret = translate_error(fs, attr_ino, err);
				goto out_unlock;
			}

			dirty = true;
		}
	}

	if (dirty) {
		err = fuse2fs_write_inode(fs, attr_ino, &inode);
		if (err) {
			ret = translate_error(fs, attr_ino, err);
			goto out_unlock;
		}
	}

out_unlock:
	fuse2fs_finish(ff, ret);
	return ret;
}
#endif /* HAVE_FUSE_IOMAP */

static struct fuse_operations fs_ops = {
	.init = op_init,
	.destroy = op_destroy,
	.getattr = op_getattr,
	.readlink = op_readlink,
	.mknod = op_mknod,
	.mkdir = op_mkdir,
	.unlink = op_unlink,
	.rmdir = op_rmdir,
	.symlink = op_symlink,
	.rename = op_rename,
	.link = op_link,
	.chmod = op_chmod,
	.chown = op_chown,
	.truncate = op_truncate,
	.open = op_open,
	.read = op_read,
	.write = op_write,
	.statfs = op_statfs,
	.release = op_release,
	.fsync = op_fsync,
	.setxattr = op_setxattr,
	.getxattr = op_getxattr,
	.listxattr = op_listxattr,
	.removexattr = op_removexattr,
	.opendir = op_open,
	.readdir = op_readdir,
	.releasedir = op_release,
	.fsyncdir = op_fsync,
	.access = op_access,
	.create = op_create,
#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 0)
	.ftruncate = op_ftruncate,
	.fgetattr = op_fgetattr,
#endif
	.utimens = op_utimens,
#if (FUSE_VERSION >= FUSE_MAKE_VERSION(2, 9)) && (FUSE_VERSION < FUSE_MAKE_VERSION(3, 0))
# if defined(UTIME_NOW) || defined(UTIME_OMIT)
	.flag_utime_omit_ok = 1,
# endif
#endif
	.bmap = op_bmap,
#ifdef SUPERFLUOUS
	.lock = op_lock,
	.poll = op_poll,
#endif
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 8)
	.ioctl = op_ioctl,
#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 0)
	.flag_nullpath_ok = 1,
#endif
#endif
#if FUSE_VERSION >= FUSE_MAKE_VERSION(2, 9)
#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 0)
	.flag_nopath = 1,
#endif
# ifdef SUPPORT_FALLOCATE
	.fallocate = op_fallocate,
# endif
#endif
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 18)
	.syncfs = op_syncfs,
	.statx = op_statx,
#endif
#ifdef HAVE_FUSE_IOMAP
	.iomap_begin = op_iomap_begin,
	.iomap_end = op_iomap_end,
	.iomap_config = op_iomap_config,
	.iomap_ioend = op_iomap_ioend,
	.getattr_iflags = op_getattr_iflags,
#endif /* HAVE_FUSE_IOMAP */
};

static int get_random_bytes(void *p, size_t sz)
{
	int fd;
	ssize_t r;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) {
		perror("/dev/urandom");
		return 0;
	}

	r = read(fd, p, sz);

	close(fd);
	return (size_t) r == sz;
}

enum {
	FUSE2FS_IGNORED,
	FUSE2FS_VERSION,
	FUSE2FS_HELP,
	FUSE2FS_HELPFULL,
	FUSE2FS_CACHE_SIZE,
	FUSE2FS_DIRSYNC,
	FUSE2FS_ERRORS_BEHAVIOR,
#ifdef HAVE_FUSE_IOMAP
	FUSE2FS_IOMAP,
	FUSE2FS_IOMAP_PASSTHROUGH,
#endif
};

#define FUSE2FS_OPT(t, p, v) { t, offsetof(struct fuse2fs, p), v }

static struct fuse_opt fuse2fs_opts[] = {
	FUSE2FS_OPT("ro",		ro,			1),
	FUSE2FS_OPT("rw",		ro,			0),
	FUSE2FS_OPT("minixdf",		minixdf,		1),
	FUSE2FS_OPT("bsddf",		minixdf,		0),
	FUSE2FS_OPT("fakeroot",		fakeroot,		1),
	FUSE2FS_OPT("fuse2fs_debug",	debug,			1),
	FUSE2FS_OPT("no_default_opts",	no_default_opts,	1),
	FUSE2FS_OPT("norecovery",	norecovery,		1),
	FUSE2FS_OPT("noload",		norecovery,		1),
	FUSE2FS_OPT("offset=%lu",	offset,			0),
	FUSE2FS_OPT("kernel",		kernel,			1),
	FUSE2FS_OPT("directio",		directio,		1),
	FUSE2FS_OPT("acl",		acl,			1),
	FUSE2FS_OPT("noacl",		acl,			0),
	FUSE2FS_OPT("lockfile=%s",	lockfile,		0),
	FUSE2FS_OPT("noblkdev",		noblkdev,		1),

#ifdef HAVE_FUSE_IOMAP
#ifdef MS_LAZYTIME
	FUSE_OPT_KEY("lazytime",	FUSE2FS_IOMAP_PASSTHROUGH),
	FUSE_OPT_KEY("nolazytime",	FUSE2FS_IOMAP_PASSTHROUGH),
#endif
#ifdef MS_STRICTATIME
	FUSE_OPT_KEY("strictatime",	FUSE2FS_IOMAP_PASSTHROUGH),
	FUSE_OPT_KEY("nostrictatime",	FUSE2FS_IOMAP_PASSTHROUGH),
#endif
#endif

	FUSE_OPT_KEY("user_xattr",	FUSE2FS_IGNORED),
	FUSE_OPT_KEY("noblock_validity", FUSE2FS_IGNORED),
	FUSE_OPT_KEY("nodelalloc",	FUSE2FS_IGNORED),
	FUSE_OPT_KEY("cache_size=%s",	FUSE2FS_CACHE_SIZE),
	FUSE_OPT_KEY("dirsync",		FUSE2FS_DIRSYNC),
	FUSE_OPT_KEY("errors=%s",	FUSE2FS_ERRORS_BEHAVIOR),
#ifdef HAVE_FUSE_IOMAP
	FUSE_OPT_KEY("iomap=%s",	FUSE2FS_IOMAP),
	FUSE_OPT_KEY("iomap",		FUSE2FS_IOMAP),
#endif

	FUSE_OPT_KEY("-V",             FUSE2FS_VERSION),
	FUSE_OPT_KEY("--version",      FUSE2FS_VERSION),
	FUSE_OPT_KEY("-h",             FUSE2FS_HELP),
	FUSE_OPT_KEY("--help",         FUSE2FS_HELP),
	FUSE_OPT_KEY("--helpfull",     FUSE2FS_HELPFULL),
	FUSE_OPT_END
};


static int fuse2fs_opt_proc(void *data, const char *arg,
			    int key, struct fuse_args *outargs)
{
	struct fuse2fs *ff = data;

	switch (key) {
#ifdef HAVE_FUSE_IOMAP
	case FUSE2FS_IOMAP_PASSTHROUGH:
		ff->iomap_passthrough_options = 1;
		/* pass through to libfuse */
		return 1;
#endif
	case FUSE2FS_DIRSYNC:
		ff->dirsync = 1;
		/* pass through to libfuse */
		return 1;
	case FUSE_OPT_KEY_NONOPT:
		if (!ff->device) {
			ff->device = strdup(arg);
			return 0;
		}
		return 1;
	case FUSE2FS_CACHE_SIZE:
		ff->cache_size = parse_num_blocks2(arg + 11, -1);
		if (ff->cache_size < 1 || ff->cache_size > INT32_MAX) {
			fprintf(stderr, "%s: %s\n", arg,
 _("cache size must be between 1 block and 2GB."));
			return -1;
		}

		/* do not pass through to libfuse */
		return 0;
	case FUSE2FS_ERRORS_BEHAVIOR:
		if (strcmp(arg + 7, "continue") == 0)
			ff->errors_behavior = EXT2_ERRORS_CONTINUE;
		else if (strcmp(arg + 7, "remount-ro") == 0)
			ff->errors_behavior = EXT2_ERRORS_RO;
		else if (strcmp(arg + 7, "panic") == 0)
			ff->errors_behavior = EXT2_ERRORS_PANIC;
		else {
			fprintf(stderr, "%s: %s\n", arg,
 _("unknown errors behavior."));
			return -1;
		}

		/* do not pass through to libfuse */
		return 0;
#ifdef HAVE_FUSE_IOMAP
	case FUSE2FS_IOMAP:
		if (strcmp(arg, "iomap") == 0 || strcmp(arg + 6, "1") == 0)
			ff->iomap_want = FT_ENABLE;
		else if (strcmp(arg + 6, "0") == 0)
			ff->iomap_want = FT_DISABLE;
		else if (strcmp(arg + 6, "default") == 0)
			ff->iomap_want = FT_DEFAULT;
		else {
			fprintf(stderr, "%s: %s\n", arg,
 _("unknown iomap= behavior."));
			return -1;
		}

		/* do not pass through to libfuse */
		return 0;
#endif
	case FUSE2FS_IGNORED:
		return 0;
	case FUSE2FS_HELP:
	case FUSE2FS_HELPFULL:
		fprintf(stderr,
	"usage: %s device/image mountpoint [options]\n"
	"\n"
	"general options:\n"
	"    -o opt,[opt...]  mount options\n"
	"    -h   --help      print help\n"
	"    -V   --version   print version\n"
	"\n"
	"fuse2fs options:\n"
	"    -o errors=panic        dump core on error\n"
	"    -o minixdf             minix-style df\n"
	"    -o fakeroot            pretend to be root for permission checks\n"
	"    -o no_default_opts     do not include default fuse options\n"
	"    -o offset=<bytes>      similar to mount -o offset=<bytes>, mount the partition starting at <bytes>\n"
	"    -o norecovery          don't replay the journal\n"
	"    -o fuse2fs_debug       enable fuse2fs debugging\n"
	"    -o lockfile=<file>     file to show that fuse is still using the file system image\n"
	"    -o kernel              run this as if it were the kernel, which sets:\n"
	"                           allow_others,default_permissions,suid,dev\n"
	"    -o directio            use O_DIRECT to read and write the disk\n"
	"    -o cache_size=N[KMG]   use a disk cache of this size\n"
	"    -o errors=             behavior when an error is encountered:\n"
	"                           continue|remount-ro|panic\n"
#ifdef HAVE_FUSE_IOMAP
	"    -o iomap=              0 to disable iomap, 1 to enable iomap\n"
#endif
	"\n",
			outargs->argv[0]);
		if (key == FUSE2FS_HELPFULL) {
			fuse_opt_add_arg(outargs, "-h");
			fuse_main(outargs->argc, outargs->argv, &fs_ops, NULL);
		} else {
			fprintf(stderr, "Try --helpfull to get a list of "
				"all flags, including the FUSE options.\n");
		}
		exit(1);

	case FUSE2FS_VERSION:
		fprintf(stderr, "fuse2fs %s (%s)\n", E2FSPROGS_VERSION,
			E2FSPROGS_DATE);
		fuse_opt_add_arg(outargs, "--version");
		fuse_main(outargs->argc, outargs->argv, &fs_ops, NULL);
		exit(0);
	}
	return 1;
}

static const char *get_subtype(const char *argv0)
{
	size_t argvlen = strlen(argv0);

	if (argvlen < 4)
		goto out_default;

	if (argv0[argvlen - 4] == 'e' &&
	    argv0[argvlen - 3] == 'x' &&
	    argv0[argvlen - 2] == 't' &&
	    isdigit(argv0[argvlen - 1]))
		return &argv0[argvlen - 4];

out_default:
	return "ext4";
}

/* Figure out a reasonable default size for the disk cache */
static unsigned long long default_cache_size(void)
{
	long pages = 0, pagesize = 0;
	unsigned long long max_cache;
	unsigned long long ret = 32ULL << 20; /* 32 MB */

#ifdef _SC_PHYS_PAGES
	pages = sysconf(_SC_PHYS_PAGES);
#endif
#ifdef _SC_PAGESIZE
	pagesize = sysconf(_SC_PAGESIZE);
#endif
	if (pages > 0 && pagesize > 0) {
		max_cache = (unsigned long long)pagesize * pages / 20;

		if (max_cache > 0 && ret > max_cache)
			ret = max_cache;
	}
	return ret;
}

#ifdef HAVE_FUSE_IOMAP
static inline bool fuse2fs_discover_iomap(const struct fuse2fs *ff)
{
	if (ff->iomap_want == FT_DISABLE)
		return false;

	return fuse_discover_iomap();
}
#else
# define fuse2fs_discover_iomap(...)	(false)
#endif

static inline bool fuse2fs_want_fuseblk(const struct fuse2fs *ff)
{
	if (ff->noblkdev)
		return false;
	if (fuse2fs_discover_iomap(ff))
		return false;

	return fuse2fs_on_bdev(ff);
}

static void fuse2fs_com_err_proc(const char *whoami, errcode_t code,
				 const char *fmt, va_list args)
{
	fprintf(stderr, "FUSE2FS (%s): ", err_shortdev ? err_shortdev : "?");
	if (whoami)
		fprintf(stderr, "%s: ", whoami);
	fprintf(stderr, "%s ", error_message(code));
        vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	fflush(stderr);
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse2fs fctx = {
		.magic = FUSE2FS_MAGIC,
		.opstate = F2OP_WRITABLE,
#ifdef HAVE_FUSE_IOMAP
		.iomap_want = FT_DEFAULT,
		.iomap_state = IOMAP_UNKNOWN,
		.iomap_dev = FUSE_IOMAP_DEV_NULL,
#endif
		.can_hardlink = 1,
		.write_gdt_on_destroy = 1,
	};
	errcode_t err;
	FILE *orig_stderr = stderr;
	char *logfile;
	char extra_args[BUFSIZ];
	int ret;

	ret = fuse_opt_parse(&args, &fctx, fuse2fs_opts, fuse2fs_opt_proc);
	if (ret)
		exit(1);
	if (fctx.device == NULL) {
		fprintf(stderr, "Missing ext4 device/image\n");
		fprintf(stderr, "See '%s -h' for usage\n", argv[0]);
		exit(1);
	}

#ifdef HAVE_FUSE_IOMAP
	if (fctx.iomap_want == FT_DISABLE)
		fctx.iomap_state = IOMAP_DISABLED;
#endif

	/* /dev/sda -> sda for reporting */
	fctx.shortdev = strrchr(fctx.device, '/');
	if (fctx.shortdev)
		fctx.shortdev++;
	else
		fctx.shortdev = fctx.device;

	/* capture library error messages */
	err_shortdev = fctx.shortdev;
	set_com_err_hook(fuse2fs_com_err_proc);

#ifdef ENABLE_NLS
	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");
	bindtextdomain(NLS_CAT_NAME, LOCALEDIR);
	textdomain(NLS_CAT_NAME);
	set_com_err_gettext(gettext);
#endif
	add_error_table(&et_ext2_error_table);

	/* Set up error logging */
	logfile = getenv("FUSE2FS_LOGFILE");
	if (logfile) {
		FILE *fp = fopen(logfile, "a");
		if (!fp) {
			perror(logfile);
			goto out;
		}
		stderr = fp;
		stdout = fp;
	} else if (fctx.kernel) {
		/* in kernel mode, try to log errors to the kernel log */
		FILE *fp = fopen("/dev/ttyprintk", "a");
		if (fp) {
			stderr = fp;
			stdout = fp;
		}
	}

#ifdef HAVE_PR_SET_IO_FLUSHER
	/*
	 * Register as a filesystem I/O server process so that our memory
	 * allocations don't cause fs reclaim.
	 */
	ret = prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
	if (ret < 0) {
		err_printf(&fctx, "%s: %s.\n",
 _("Could not register as IO flusher thread"),
				strerror(errno));
		ret = 0;
	}
#endif

	/* Will we allow users to allocate every last block? */
	if (getenv("FUSE2FS_ALLOC_ALL_BLOCKS")) {
		log_printf(&fctx, "%s\n",
 _("Allowing users to allocate all blocks. This is dangerous!"));
		fctx.alloc_all_blocks = 1;
	}

	err = fuse2fs_open(&fctx, EXT2_FLAG_EXCLUSIVE);
	if (err) {
		ret = 32;
		goto out;
	}

	if (!fctx.cache_size)
		fctx.cache_size = default_cache_size();

	err = fuse2fs_check_support(&fctx);
	if (err) {
		ret = 32;
		goto out;
	}

	/*
	 * ext4 can't do COW of shared blocks, so if the feature is enabled,
	 * we must force ro mode.
	 */
	if (ext2fs_has_feature_shared_blocks(fctx.fs->super))
		fctx.ro = 1;

	if (fctx.norecovery) {
		ret = fuse2fs_check_norecovery(&fctx);
		if (ret)
			goto out;
	}

	err = fuse2fs_mount(&fctx);
	if (err) {
		ret = 32;
		goto out;
	}

	/* Initialize generation counter */
	get_random_bytes(&fctx.next_generation, sizeof(unsigned int));

	/* Set up default fuse parameters */
	snprintf(extra_args, BUFSIZ, "-okernel_cache,subtype=%s,"
		 "fsname=%s,attr_timeout=0" FUSE_PLATFORM_OPTS,
		 get_subtype(argv[0]),
		 fctx.device);
	if (fctx.no_default_opts == 0)
		fuse_opt_add_arg(&args, extra_args);

	if (fuse2fs_want_fuseblk(&fctx)) {
		unsigned int blksize = fctx.fs->blocksize;

		/*
		 * If this is a block device, we want to close the fs and open
		 * the fuse driver in fuseblk mode (which will reopen the block
		 * device) so that unmount will wait until op_destroy
		 * completes.  If this is not a block device, we cannot use
		 * fuseblk mode and should leave the filesystem open.
		 *
		 * However, fuse+iomap guarantees that op_destroy is called
		 * before the filesystem is unmounted, so we don't need fuseblk
		 * mode.  This save us the trouble of reopening the filesystem
		 * later, and means that fuse2fs itself owns the exclusive lock
		 * on the block device.
		 */
		fuse2fs_unmount(&fctx);

		/* "blkdev" is the magic mount option for fuseblk mode */
		snprintf(extra_args, BUFSIZ, "-oblkdev,blksize=%u", blksize);
		fuse_opt_add_arg(&args, extra_args);
		fctx.unmount_in_destroy = 1;
	}

	if (fctx.ro)
		fuse_opt_add_arg(&args, "-oro");

	if (fctx.fakeroot) {
#ifdef HAVE_MOUNT_NODEV
		fuse_opt_add_arg(&args,"-onodev");
#endif
#ifdef HAVE_MOUNT_NOSUID
		fuse_opt_add_arg(&args,"-onosuid");
#endif
	}

	if (fctx.kernel) {
		/*
		 * ACLs are always enforced when kernel mode is enabled, to
		 * match the kernel ext4 driver which always enables ACLs.
		 */
		fctx.acl = 1;
		fuse_opt_insert_arg(&args, 1,
 "-oallow_other,default_permissions,suid,dev");
	}

	if (fctx.debug) {
		int	i;

		printf("FUSE2FS (%s): fuse arguments:", fctx.shortdev);
		for (i = 0; i < args.argc; i++)
			printf(" '%s'", args.argv[i]);
		printf("\n");
		fflush(stdout);
	}

	pthread_mutex_init(&fctx.bfl, NULL);
	ret = fuse_main(args.argc, args.argv, &fs_ops, &fctx);
	pthread_mutex_destroy(&fctx.bfl);

	switch(ret) {
	case 0:
		/* success */
		ret = 0;
		break;
	case 1:
	case 2:
		/* invalid option or no mountpoint */
		ret = 1;
		break;
	case 3:
	case 4:
	case 5:
	case 6:
	case 7:
		/* setup or mounting failed */
		ret = 32;
		break;
	default:
		/* fuse started up enough to call op_init */
		ret = 0;
		break;
	}

	/* mount might have failed */
	ret |= fctx.retcode;
out:
	if (ret & 1) {
		fprintf(orig_stderr, "%s\n",
 _("Mount failed due to unrecognized options.  Check dmesg(1) for details."));
		fflush(orig_stderr);
	}
	if (ret & 32) {
		fprintf(orig_stderr, "%s\n",
 _("Mount failed while opening filesystem.  Check dmesg(1) for details."));
		fflush(orig_stderr);
	}
	fuse2fs_unmount(&fctx);
	reset_com_err_hook();
	err_shortdev = NULL;
	if (fctx.device)
		free(fctx.device);
	fuse_opt_free_args(&args);
	return ret;
}

static int __translate_error(ext2_filsys fs, ext2_ino_t ino, errcode_t err,
			     const char *func, int line)
{
	struct timespec now;
	int ret = err;
	struct fuse2fs *ff = fs->priv_data;
	int is_err = 0;

	/* Translate ext2 error to unix error code */
	switch (err) {
	case 0:
		break;
	case EXT2_ET_NO_MEMORY:
	case EXT2_ET_TDB_ERR_OOM:
		ret = -ENOMEM;
		break;
	case EXT2_ET_INVALID_ARGUMENT:
	case EXT2_ET_LLSEEK_FAILED:
		ret = -EINVAL;
		break;
	case EXT2_ET_NO_DIRECTORY:
		ret = -ENOTDIR;
		break;
	case EXT2_ET_FILE_NOT_FOUND:
		ret = -ENOENT;
		break;
	case EXT2_ET_DIR_NO_SPACE:
		is_err = 1;
		/* fallthrough */
	case EXT2_ET_TOOSMALL:
	case EXT2_ET_BLOCK_ALLOC_FAIL:
	case EXT2_ET_INODE_ALLOC_FAIL:
	case EXT2_ET_EA_NO_SPACE:
		ret = -ENOSPC;
		break;
	case EXT2_ET_SYMLINK_LOOP:
		ret = -EMLINK;
		break;
	case EXT2_ET_FILE_TOO_BIG:
		ret = -EFBIG;
		break;
	case EXT2_ET_TDB_ERR_EXISTS:
	case EXT2_ET_FILE_EXISTS:
		ret = -EEXIST;
		break;
	case EXT2_ET_MMP_FAILED:
	case EXT2_ET_MMP_FSCK_ON:
		ret = -EBUSY;
		break;
	case EXT2_ET_EA_KEY_NOT_FOUND:
		ret = -ENODATA;
		break;
	case EXT2_ET_UNIMPLEMENTED:
		ret = -EOPNOTSUPP;
		break;
	case EXT2_ET_MAGIC_EXT2_FILE:
	case EXT2_ET_MAGIC_EXT2FS_FILSYS:
	case EXT2_ET_MAGIC_BADBLOCKS_LIST:
	case EXT2_ET_MAGIC_BADBLOCKS_ITERATE:
	case EXT2_ET_MAGIC_INODE_SCAN:
	case EXT2_ET_MAGIC_IO_CHANNEL:
	case EXT2_ET_MAGIC_UNIX_IO_CHANNEL:
	case EXT2_ET_MAGIC_IO_MANAGER:
	case EXT2_ET_MAGIC_BLOCK_BITMAP:
	case EXT2_ET_MAGIC_INODE_BITMAP:
	case EXT2_ET_MAGIC_GENERIC_BITMAP:
	case EXT2_ET_MAGIC_TEST_IO_CHANNEL:
	case EXT2_ET_MAGIC_DBLIST:
	case EXT2_ET_MAGIC_ICOUNT:
	case EXT2_ET_MAGIC_PQ_IO_CHANNEL:
	case EXT2_ET_MAGIC_E2IMAGE:
	case EXT2_ET_MAGIC_INODE_IO_CHANNEL:
	case EXT2_ET_MAGIC_EXTENT_HANDLE:
	case EXT2_ET_BAD_MAGIC:
	case EXT2_ET_MAGIC_EXTENT_PATH:
	case EXT2_ET_MAGIC_GENERIC_BITMAP64:
	case EXT2_ET_MAGIC_BLOCK_BITMAP64:
	case EXT2_ET_MAGIC_INODE_BITMAP64:
	case EXT2_ET_MAGIC_RESERVED_13:
	case EXT2_ET_MAGIC_RESERVED_14:
	case EXT2_ET_MAGIC_RESERVED_15:
	case EXT2_ET_MAGIC_RESERVED_16:
	case EXT2_ET_MAGIC_RESERVED_17:
	case EXT2_ET_MAGIC_RESERVED_18:
	case EXT2_ET_MAGIC_RESERVED_19:
	case EXT2_ET_MMP_MAGIC_INVALID:
	case EXT2_ET_MAGIC_EA_HANDLE:
	case EXT2_ET_DIR_CORRUPTED:
	case EXT2_ET_CORRUPT_SUPERBLOCK:
	case EXT2_ET_RESIZE_INODE_CORRUPT:
	case EXT2_ET_TDB_ERR_CORRUPT:
	case EXT2_ET_UNDO_FILE_CORRUPT:
	case EXT2_ET_FILESYSTEM_CORRUPTED:
	case EXT2_ET_CORRUPT_JOURNAL_SB:
	case EXT2_ET_INODE_CORRUPTED:
	case EXT2_ET_EA_INODE_CORRUPTED:
		/* same errno that linux uses */
		is_err = 1;
		ret = -EUCLEAN;
		break;
	default:
		is_err = 1;
		ret = (err < 256) ? -err : -EIO;
		break;
	}

	if (!is_err)
		return ret;

	if (ino)
		err_printf(ff, "%s (inode #%d) at %s:%d.\n",
			error_message(err), ino, func, line);
	else
		err_printf(ff, "%s at %s:%d.\n",
			error_message(err), func, line);

	/* Make a note in the error log */
	fuse2fs_get_now(ff, &now);
	ext2fs_set_tstamp(fs->super, s_last_error_time, now.tv_sec);
	fs->super->s_last_error_ino = ino;
	fs->super->s_last_error_line = line;
	fs->super->s_last_error_block = err; /* Yeah... */
	strncpy((char *)fs->super->s_last_error_func, func,
		sizeof(fs->super->s_last_error_func));
	if (ext2fs_get_tstamp(fs->super, s_first_error_time) == 0) {
		ext2fs_set_tstamp(fs->super, s_first_error_time, now.tv_sec);
		fs->super->s_first_error_ino = ino;
		fs->super->s_first_error_line = line;
		fs->super->s_first_error_block = err;
		strncpy((char *)fs->super->s_first_error_func, func,
			sizeof(fs->super->s_first_error_func));
	}

	fs->super->s_error_count++;
	ext2fs_mark_super_dirty(fs);
	ext2fs_flush(fs);
	switch (ff->errors_behavior) {
	case EXT2_ERRORS_CONTINUE:
		err_printf(ff, "%s\n",
 _("Continuing after errors; is this a good idea?"));
		break;
	case EXT2_ERRORS_RO:
		err_printf(ff, "%s\n",
 _("Remounting read-only due to errors."));
		fs->flags &= ~EXT2_FLAG_RW;
		if (ff->opstate == F2OP_WRITABLE)
			ff->opstate = F2OP_READONLY;
		break;
	case EXT2_ERRORS_PANIC:
		err_printf(ff, "%s\n",
 _("Aborting filesystem mount due to errors."));
		abort();
		break;
	}

	return ret;
}
