/*
 * fuse4fs.c - FUSE low-level server for e2fsprogs.
 *
 * Copyright (C) 2014-2025 Oracle.
 * Copyright (C) 2025 CTERA Networks.
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
#include <assert.h>
#ifdef HAVE_FUSE_LOOPDEV
# include <fuse_loopdev.h>
#endif
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
#include <fuse_lowlevel.h>
#ifdef __SET_FOB_FOR_FUSE
# undef _FILE_OFFSET_BITS
#endif /* __SET_FOB_FOR_FUSE */
#include <inttypes.h>
#include "ext2fs/ext2fs.h"
#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fsP.h"
#include "support/list.h"
#include "support/cache.h"

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

#define FUSE4FS_ATTR_TIMEOUT	(0.0)

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
#define min(a, b)	((a) < (b) ? (a) : (b))

#define dbg_printf(fuse4fs, format, ...) \
	while ((fuse4fs)->debug) { \
		printf("FUSE4FS (%s): tid=%d " format, (fuse4fs)->shortdev, gettid(), ##__VA_ARGS__); \
		fflush(stdout); \
		break; \
	}

#define log_printf(fuse4fs, format, ...) \
	do { \
		printf("FUSE4FS (%s): " format, (fuse4fs)->shortdev, ##__VA_ARGS__); \
		fflush(stdout); \
	} while (0)

#define err_printf(fuse4fs, format, ...) \
	do { \
		fprintf(stderr, "FUSE4FS (%s): " format, (fuse4fs)->shortdev, ##__VA_ARGS__); \
		fflush(stderr); \
	} while (0)

#define timing_printf(fuse4fs, format, ...) \
	while ((fuse4fs)->timing) { \
		printf("FUSE4FS (%s): " format, (fuse4fs)->shortdev, ##__VA_ARGS__); \
		break; \
	}

#ifdef _IOR
# ifdef _IOW
#  define SUPPORT_I_FLAGS
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

errcode_t ext2fs_check_ext3_journal(ext2_filsys fs);
errcode_t ext2fs_run_ext3_journal(ext2_filsys *fs);

const char *err_shortdev;

#ifdef CONFIG_JBD_DEBUG		/* Enabled by configure --enable-jbd-debug */
int journal_enable_debug = -1;
#endif

/*
 * ext2_file_t contains a struct inode, so we can't leave files open.
 * Use this as a proxy instead.
 */
#define FUSE4FS_FILE_MAGIC	(0xEF53DEAFUL)
struct fuse4fs_file_handle {
	unsigned long magic;
	struct fuse4fs_inode *fi;
	ext2_ino_t ino;
	int open_flags;
	int check_flags;
};

enum fuse4fs_opstate {
	F4OP_READONLY,
	F4OP_WRITABLE,
	F4OP_SHUTDOWN,
};

enum fuse4fs_feature_toggle {
	FT_DISABLE,
	FT_ENABLE,
	FT_DEFAULT,
};

#ifdef HAVE_FUSE_IOMAP
enum fuse4fs_iomap_state {
	IOMAP_DISABLED,
	IOMAP_UNKNOWN,
	IOMAP_ENABLED,
};
#endif

/* Main program context */
#define FUSE4FS_MAGIC		(0xEF53DEADUL)
struct fuse4fs {
	unsigned long magic;
	ext2_filsys fs;
	pthread_mutex_t bfl;
	char *device;
	char *shortdev;
#ifdef HAVE_FUSE_LOOPDEV
	char *loop_device;
	int loop_fd;
#endif

	/* options set by fuse_opt_parse must be of type int */
	int ro;
	int debug;
	int no_default_opts;
	int errors_behavior; /* actually an enum */
	int minixdf;
	int fakeroot;
	int alloc_all_blocks;
	int norecovery;
	int kernel;
	int directio;
	int acl;
	int dirsync;
	int translate_inums;
	int iomap_passthrough_options;

	enum fuse4fs_opstate opstate;
	int logfd;
	int blocklog;
#ifdef HAVE_FUSE_IOMAP
	enum fuse4fs_feature_toggle iomap_want;
	enum fuse4fs_iomap_state iomap_state;
	uint32_t iomap_dev;
	uint64_t iomap_cap;
	void (*old_alloc_stats)(ext2_filsys fs, blk64_t blk, int inuse);
	void (*old_alloc_stats_range)(ext2_filsys fs, blk64_t blk, blk_t num,
				      int inuse);
#ifdef STATX_WRITE_ATOMIC
	unsigned int awu_min, awu_max;
#endif
#endif
	unsigned int blockmask;
	unsigned long offset;
	unsigned int next_generation;
	unsigned long long cache_size;
	char *lockfile;
#ifdef CONFIG_MMP
	uint8_t mmp_running;
	unsigned int mmp_update_interval;
	pthread_t mmp_tid;
#endif
#ifdef HAVE_CLOCK_MONOTONIC
	struct timespec lock_start_time;
	struct timespec op_start_time;

	/* options set by fuse_opt_parse must be of type int */
	int timing;
#endif
	struct fuse_session *fuse;
	struct cache inodes;
};

#define FUSE4FS_CHECK_HANDLE(req, fh) \
	do { \
		if ((fh) == NULL || (fh)->magic != FUSE4FS_FILE_MAGIC) { \
			fprintf(stderr, \
				"FUSE4FS: Corrupt in-memory file handle at %s:%d!\n", \
				__func__, __LINE__); \
			fflush(stderr); \
			fuse_reply_err(req, EUCLEAN); \
			return; \
		} \
	} while (0)

#define __FUSE4FS_CHECK_CONTEXT(ff, retcode, shutcode) \
	do { \
		if ((ff) == NULL || (ff)->magic != FUSE4FS_MAGIC) { \
			fprintf(stderr, \
				"FUSE4FS: Corrupt in-memory data at %s:%d!\n", \
				__func__, __LINE__); \
			fflush(stderr); \
			retcode; \
		} \
		if ((ff)->opstate == F4OP_SHUTDOWN) { \
			shutcode; \
		} \
	} while (0)

#define FUSE4FS_CHECK_CONTEXT(req) \
	__FUSE4FS_CHECK_CONTEXT(fuse4fs_get(req), \
				fuse_reply_err((req), EUCLEAN); return, \
				fuse_reply_err((req), EIO); return)
#define FUSE4FS_CHECK_CONTEXT_DESTROY(req) \
	__FUSE4FS_CHECK_CONTEXT((req), return, /* do not return */)
#define FUSE4FS_CHECK_CONTEXT_INIT(req) \
	__FUSE4FS_CHECK_CONTEXT((req), abort(), abort())

static inline void fuse4fs_ino_from_fuse(const struct fuse4fs *ff,
					 ext2_ino_t *inop, fuse_ino_t fino)
{
	if (ff->translate_inums && fino == FUSE_ROOT_ID)
		*inop = EXT2_ROOT_INO;
	else
		*inop = fino;
}

static inline void fuse4fs_ino_to_fuse(const struct fuse4fs *ff,
				       fuse_ino_t *finop, ext2_ino_t ino)
{
	if (ff->translate_inums && ino == EXT2_ROOT_INO)
		*finop = FUSE_ROOT_ID;
	else
		*finop = ino;
}

#define FUSE4FS_CONVERT_FINO(req, ext2_inop, fuse_ino) \
	do { \
		if ((fuse_ino) > UINT32_MAX) { \
			fprintf(stderr, \
				"FUSE4FS: Bogus inode number 0x%llx at %s:%d!\n", \
				(unsigned long long)(fuse_ino), __func__, __LINE__); \
			fflush(stderr); \
			fuse_reply_err((req), EIO); \
			return; \
		} \
		fuse4fs_ino_from_fuse(fuse4fs_get(req), ext2_inop, fuse_ino); \
	} while (0)

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

/* inode is not on unlinked list */
#define FUSE4FS_NULL_INO	((ext2_ino_t)~0ULL)

struct fuse4fs_inode {
	struct cache_node	i_cnode;
	ext2_ino_t		i_ino;
	unsigned int		i_open_count;

	/*
	 * FUSE4FS_NULL_INO: inode is not on the orphan list
	 * 0: inode is the first on the orphan list
	 * otherwise: inode is in the middle of the list
	 */
	ext2_ino_t		i_prev_orphan;
};

struct fuse4fs_ikey {
	ext2_ino_t		i_ino;
};

#define ICKEY(key)	((struct fuse4fs_ikey *)(key))
#define ICNODE(node)	(container_of((node), struct fuse4fs_inode, i_cnode))

static unsigned int
icache_hash(cache_key_t key, unsigned int hashsize, unsigned int hashshift)
{
	uint64_t	hashval = ICKEY(key)->i_ino;
	uint64_t	tmp;

	tmp = hashval ^ (GOLDEN_RATIO_PRIME + hashval) / CACHE_LINE_SIZE;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> hashshift);
	return tmp % hashsize;
}

static int icache_compare(struct cache_node *node, cache_key_t key)
{
	struct fuse4fs_inode *fi = ICNODE(node);
	struct fuse4fs_ikey *ikey = ICKEY(key);

	if (fi->i_ino == ikey->i_ino)
		return CACHE_HIT;

	return CACHE_MISS;
}

static struct cache_node *icache_alloc(struct cache *c, cache_key_t key)
{
	struct fuse4fs_ikey *ikey = ICKEY(key);
	struct fuse4fs_inode *fi;

	fi = calloc(1, sizeof(struct fuse4fs_inode));
	if (!fi)
		return NULL;

	fi->i_ino = ikey->i_ino;
	fi->i_prev_orphan = FUSE4FS_NULL_INO;
	return &fi->i_cnode;
}

static bool icache_flush(struct cache *c, struct cache_node *node)
{
	struct fuse4fs_inode *fi = ICNODE(node);

	return fi->i_prev_orphan != FUSE4FS_NULL_INO;
}

static void icache_relse(struct cache *c, struct cache_node *node)
{
	struct fuse4fs_inode *fi = ICNODE(node);

	assert(fi->i_open_count == 0);
	free(fi);
}

static unsigned int icache_bulkrelse(struct cache *cache,
				     struct list_head *list)
{
	struct cache_node *cn, *n;
	int count = 0;

	if (list_empty(list))
		return 0;

	list_for_each_entry_safe(cn, n, list, cn_mru) {
		icache_relse(cache, cn);
		count++;
	}

	return count;
}

static const struct cache_operations icache_ops = {
	.hash		= icache_hash,
	.alloc		= icache_alloc,
	.flush		= icache_flush,
	.relse		= icache_relse,
	.compare	= icache_compare,
	.bulkrelse	= icache_bulkrelse,
	.resize		= cache_gradual_resize,
};

static errcode_t fuse4fs_iget(struct fuse4fs *ff, ext2_ino_t ino,
			      struct fuse4fs_inode **fip)
{
	struct fuse4fs_ikey ikey = {
		.i_ino = ino,
	};
	struct cache_node *node = NULL;

	cache_node_get(&ff->inodes, &ikey, 0, &node);
	if (!node)
		return ENOMEM;

	*fip = ICNODE(node);
	return 0;
}

static void fuse4fs_iput(struct fuse4fs *ff, struct fuse4fs_inode *fi)
{
	cache_node_put(&ff->inodes, &fi->i_cnode);
}

static inline blk64_t FUSE4FS_B_TO_FSBT(const struct fuse4fs *ff, off_t pos)
{
	return pos >> ff->blocklog;
}

static inline blk64_t FUSE4FS_B_TO_FSB(const struct fuse4fs *ff, off_t pos)
{
	return (pos + ff->blockmask) >> ff->blocklog;
}

static inline unsigned int FUSE4FS_OFF_IN_FSB(const struct fuse4fs *ff,
					      off_t pos)
{
	return pos & ff->blockmask;
}

static inline off_t FUSE4FS_FSB_TO_B(const struct fuse4fs *ff, blk64_t bno)
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

static inline errcode_t fuse4fs_read_inode(ext2_filsys fs, ext2_ino_t ino,
					   struct ext2_inode_large *inode)
{
	memset(inode, 0, sizeof(*inode));
	return ext2fs_read_inode_full(fs, ino, EXT2_INODE(inode),
				      sizeof(*inode));
}

static inline errcode_t fuse4fs_write_inode(ext2_filsys fs, ext2_ino_t ino,
					    struct ext2_inode_large *inode)
{
	return ext2fs_write_inode_full(fs, ino, EXT2_INODE(inode),
				       sizeof(*inode));
}

#ifdef CONFIG_MMP
static bool fuse4fs_mmp_active(const struct fuse4fs *ff)
{
	ext2_filsys fs = ff->fs;

	if (!ext2fs_has_feature_mmp(fs->super) ||
	    !(fs->flags & EXT2_FLAG_RW) || (fs->flags & EXT2_FLAG_SKIP_MMP))
		return false;
	return true;
}

static int fuse4fs_mmp_touch(struct fuse4fs *ff, bool immediate)
{
	ext2_filsys fs = ff->fs;
	struct mmp_struct *mmp = fs->mmp_buf;
	struct mmp_struct *mmp_cmp = fs->mmp_cmp;
	struct timeval tv;
	errcode_t retval = 0;

	if (!fuse4fs_mmp_active(ff))
		return 0;

	gettimeofday(&tv, 0);
	if (!immediate &&
	    tv.tv_sec - fs->mmp_last_written < ff->mmp_update_interval)
		return 0;

	retval = ext2fs_mmp_read(fs, fs->super->s_mmp_block, NULL);
	if (retval)
		return translate_error(fs, 0, retval);

	if (memcmp(mmp, mmp_cmp, sizeof(*mmp_cmp)))
		return translate_error(fs, 0, EXT2_ET_MMP_CHANGE_ABORT);

	/*
	 * Believe it or not, ext2fs_mmp_read actually overwrites fs->mmp_cmp
	 * and leaves fs->mmp_buf untouched.  Hence we copy mmp_cmp into
	 * mmp_buf, update mmp_buf, and write mmp_buf out to disk.
	 */
	memcpy(mmp, mmp_cmp, sizeof(*mmp_cmp));
	mmp->mmp_time = tv.tv_sec;
	mmp->mmp_seq = ext2fs_mmp_new_seq();

	retval = ext2fs_mmp_write(fs, fs->super->s_mmp_block, fs->mmp_buf);
	if (retval)
		return translate_error(fs, 0, retval);

	return 0;
}

static void *fuse4fs_mmp_thread(void *arg)
{
	struct fuse4fs *ff = arg;

	pthread_setname_np(pthread_self(), "fuse4fs_mmp");

	while (ff->mmp_update_interval) {
		fuse4fs_mmp_touch(ff, false);
		sleep(ff->mmp_update_interval);
	}

	return NULL;
}

static void fuse4fs_mmp_start(struct fuse4fs *ff)
{
	int error;

	if (!ff->mmp_update_interval)
		return;

	error = pthread_create(&ff->mmp_tid, NULL, fuse4fs_mmp_thread, ff);
	if (error) {
		err_printf(ff, "could not create MMP thread: %s\n",
			   strerror(error));
		return;
	}

	ff->mmp_running = 1;
}

static void fuse4fs_mmp_stop(struct fuse4fs *ff)
{
	if (ff->mmp_running) {
		ff->mmp_running = 0;
		pthread_cancel(ff->mmp_tid);
		pthread_join(ff->mmp_tid, NULL);
	}
}

static void fuse4fs_mmp_config(struct fuse4fs *ff)
{
	ext2_filsys fs = ff->fs;
	struct mmp_struct *mmp_s = fs->mmp_buf;
	unsigned int mmp_update_interval = fs->super->s_mmp_update_interval;

	if (!fuse4fs_mmp_active(ff))
		return;

	/*
	 * If update_interval in MMP block is larger, use that instead of
	 * update_interval from the superblock.
	 */
	if (mmp_s->mmp_check_interval > mmp_update_interval)
		mmp_update_interval = mmp_s->mmp_check_interval;

	/* Clamp to the relevant(?) interval values */
	if (mmp_update_interval < EXT4_MMP_MIN_CHECK_INTERVAL)
		mmp_update_interval = EXT4_MMP_MIN_CHECK_INTERVAL;
	if (mmp_update_interval > EXT4_MMP_MAX_UPDATE_INTERVAL)
		mmp_update_interval = EXT4_MMP_MAX_UPDATE_INTERVAL;

	ff->mmp_update_interval = mmp_update_interval;

	/*
	 * libext2fs writes EXT4_MMP_SEQ_FSCK after mounting, so we need to
	 * update it immediately so that it doesn't look like another node is
	 * actually running fsck.
	 */
	fuse4fs_mmp_touch(ff, true);
}
#else
# define fuse4fs_mmp_touch(...)		(0)
# define fuse4fs_mmp_start(...)		((void)0)
# define fuse4fs_mmp_stop(...)		((void)0)
# define fuse4fs_mmp_config(...)	((void)0)
#endif

static inline struct fuse4fs *fuse4fs_get(fuse_req_t req)
{
	return (struct fuse4fs *)fuse_req_userdata(req);
}

static inline struct fuse4fs_file_handle *
fuse4fs_get_handle(const struct fuse_file_info *fp)
{
	return (struct fuse4fs_file_handle *)(uintptr_t)fp->fh;
}

static inline void
fuse4fs_set_handle(struct fuse_file_info *fp, struct fuse4fs_file_handle *fh)
{
	fp->fh = (uintptr_t)fh;
	fp->keep_cache = 1;
}

#ifdef HAVE_CLOCK_MONOTONIC
static inline ext2_filsys fuse4fs_start(struct fuse4fs *ff)
{
	struct timespec lock_time;
	int ret;

	if (ff->timing)
		clock_gettime(CLOCK_MONOTONIC, &lock_time);

	pthread_mutex_lock(&ff->bfl);
	if (ff->timing) {
		ret = clock_gettime(CLOCK_MONOTONIC, &ff->op_start_time);
		if (ret)
			ff->timing = 0;
		ff->lock_start_time = lock_time;
	}
	return ff->fs;
}

static inline double ms_from_timespec(const struct timespec *ts)
{
	return ((double)ts->tv_sec * 1000) + ((double)ts->tv_nsec / 1000000);
}

static inline void fuse4fs_finish_timing(struct fuse4fs *ff, const char *func)
{
	struct timespec now;
	double lockf, startf, nowf;
	int ret;

	if (!ff->timing)
		return;

	ret = clock_gettime(CLOCK_MONOTONIC, &now);
	if (ret) {
		ff->timing = 0;
		return;
	}

	lockf = ms_from_timespec(&ff->lock_start_time);
	startf = ms_from_timespec(&ff->op_start_time);
	nowf = ms_from_timespec(&now);
	timing_printf(ff, "%s: lock=%.2fms elapsed=%.2fms\n", func,
		      startf - lockf, nowf - startf);
}
#else
static inline ext2_filsys fuse4fs_start(struct fuse4fs *ff)
{
	pthread_mutex_lock(&ff->bfl);
	return ff->fs;
}
# define fuse4fs_finish_timing(...)	((void)0)
#endif

static inline void __fuse4fs_finish(struct fuse4fs *ff, int ret,
				    const char *func)
{
	fuse4fs_finish_timing(ff, func);
	if (ret)
		dbg_printf(ff, "%s: libfuse ret=%d\n", func, ret);
	pthread_mutex_unlock(&ff->bfl);
}
#define fuse4fs_finish(ff, ret) __fuse4fs_finish((ff), (ret), __func__)

#ifdef HAVE_FUSE_IOMAP
static inline int fuse4fs_iomap_enabled(const struct fuse4fs *ff)
{
	return ff->iomap_state >= IOMAP_ENABLED;
}

static inline void fuse4fs_discover_iomap(struct fuse4fs *ff)
{
	if (ff->iomap_want == FT_DISABLE)
		return;

	ff->iomap_cap = fuse_lowlevel_discover_iomap(-1);
}

static inline bool fuse4fs_can_iomap(const struct fuse4fs *ff)
{
	return ff->iomap_cap & FUSE_IOMAP_SUPPORT_FILEIO;
}

static inline bool fuse4fs_iomap_supports_hw_atomic(const struct fuse4fs *ff)
{
	return fuse4fs_iomap_enabled(ff) &&
	       (ff->iomap_cap & FUSE_IOMAP_SUPPORT_ATOMIC) &&
#ifdef STATX_WRITE_ATOMIC
		ff->awu_min > 0 && ff->awu_min > 0;
#else
		0;
#endif
}
#else
# define fuse4fs_iomap_enabled(...)	(0)
# define fuse4fs_discover_iomap(...)	((void)0)
# define fuse4fs_can_iomap(...)		(false)
# define fuse4fs_iomap_supports_hw_atomic(...)	(0)
#endif

static inline void fuse4fs_dump_extents(struct fuse4fs *ff, ext2_ino_t ino,
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

		retval = fuse4fs_read_inode(fs, ino, inode);
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

static void fuse4fs_get_now(struct fuse4fs *ff, struct timespec *now)
{
#ifdef CLOCK_REALTIME_COARSE
	/*
	 * In iomap mode, the kernel is responsible for maintaining timestamps
	 * because file writes don't upcall to fuse4fs.  The kernel's predicate
	 * for deciding if [cm]time should be updated bases its decisions off
	 * [cm]time being an exact match for the coarse clock (instead of
	 * checking that [cm]time < coarse_clock) which means that fuse4fs
	 * setting a fine-grained timestamp that is slightly ahead of the
	 * coarse clock can result in timestamps appearing to go backwards.
	 * generic/423 doesn't like seeing btime > ctime from statx, so we'll
	 * use the coarse clock in iomap mode.
	 */
	if (fuse4fs_iomap_enabled(ff) &&
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

static void fuse4fs_init_timestamps(struct fuse4fs *ff,
				    struct ext2_inode_large *inode)
{
	struct timespec now;

	fuse4fs_get_now(ff, &now);
	EXT4_INODE_SET_XTIME(i_atime, &now, inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, inode);
	EXT4_INODE_SET_XTIME(i_mtime, &now, inode);
	EXT4_EINODE_SET_XTIME(i_crtime, &now, inode);
	increment_version(inode);
}

static int fuse4fs_update_ctime(struct fuse4fs *ff, ext2_ino_t ino,
				struct ext2_inode_large *pinode)
{
	struct timespec now;
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	errcode_t err;

	fuse4fs_get_now(ff, &now);

	/* If user already has a inode buffer, just update that */
	if (pinode) {
		increment_version(pinode);
		EXT4_INODE_SET_XTIME(i_ctime, &now, pinode);
		return 0;
	}

	/* Otherwise we have to read-modify-write the inode */
	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	increment_version(&inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, &inode);

	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse4fs_update_atime(struct fuse4fs *ff, ext2_ino_t ino)
{
	struct ext2_inode_large inode, *pinode;
	struct timespec atime, mtime, now;
	ext2_filsys fs = ff->fs;
	double datime, dmtime, dnow;
	errcode_t err;

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	pinode = &inode;
	EXT4_INODE_GET_XTIME(i_atime, &atime, pinode);
	EXT4_INODE_GET_XTIME(i_mtime, &mtime, pinode);
	fuse4fs_get_now(ff, &now);

	datime = atime.tv_sec + ((double)atime.tv_nsec / NSEC_PER_SEC);
	dmtime = mtime.tv_sec + ((double)mtime.tv_nsec / NSEC_PER_SEC);
	dnow = now.tv_sec + ((double)now.tv_nsec / NSEC_PER_SEC);

	/*
	 * If atime is newer than mtime and atime hasn't been updated in thirty
	 * seconds, skip the atime update.  Same idea as Linux "relatime".  Use
	 * doubles to account for nanosecond resolution.
	 */
	if (datime >= dmtime && datime >= dnow - 30)
		return 0;
	EXT4_INODE_SET_XTIME(i_atime, &now, &inode);

	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse4fs_update_mtime(struct fuse4fs *ff, ext2_ino_t ino,
				struct ext2_inode_large *pinode)
{
	struct ext2_inode_large inode;
	struct timespec now;
	ext2_filsys fs = ff->fs;
	errcode_t err;

	if (pinode) {
		fuse4fs_get_now(ff, &now);
		EXT4_INODE_SET_XTIME(i_mtime, &now, pinode);
		EXT4_INODE_SET_XTIME(i_ctime, &now, pinode);
		increment_version(pinode);
		return 0;
	}

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	fuse4fs_get_now(ff, &now);
	EXT4_INODE_SET_XTIME(i_mtime, &now, &inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, &inode);
	increment_version(&inode);

	err = fuse4fs_write_inode(fs, ino, &inode);
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

static int fuse4fs_can_allocate(struct fuse4fs *ff, blk64_t num)
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

static int fuse4fs_is_writeable(const struct fuse4fs *ff)
{
	return ff->opstate == F4OP_WRITABLE &&
		(ff->fs->super->s_error_count == 0);
}

static inline int fuse4fs_is_superuser(struct fuse4fs *ff,
				       const struct fuse_ctx *ctxt)
{
	if (ff->fakeroot)
		return 1;
	return ctxt->uid == 0;
}

static inline int fuse4fs_want_check_owner(struct fuse4fs *ff,
					   const struct fuse_ctx *ctxt)
{
	/*
	 * The kernel is responsible for access control, so we allow anything
	 * that the superuser can do.
	 */
	if (ff->kernel)
		return 0;
	return !fuse4fs_is_superuser(ff, ctxt);
}

/* Test for append permission */
#define A_OK	16
/* Test for linked file */
#define L_OK	32

static int fuse4fs_iflags_access(struct fuse4fs *ff, ext2_ino_t ino,
				 const struct ext2_inode *inode, int mask)
{
	EXT2FS_BUILD_BUG_ON(((A_OK | L_OK) & (R_OK | W_OK | X_OK | F_OK)) != 0);

	/* no writing or metadata changes to read-only or broken fs */
	if ((mask & (W_OK | A_OK)) && !fuse4fs_is_writeable(ff))
		return -EROFS;

	dbg_printf(ff, "access ino=%d mask=e%s%s%s%s%s iflags=0x%x\n",
		   ino,
		   (mask & R_OK ? "r" : ""),
		   (mask & W_OK ? "w" : ""),
		   (mask & X_OK ? "x" : ""),
		   (mask & A_OK ? "a" : ""),
		   (mask & L_OK ? "l" : ""),
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

static int fuse4fs_inum_access(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			       ext2_ino_t ino, int mask)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode inode;
	mode_t perms;
	errcode_t err;
	int ret;

	/* no writing to read-only or broken fs */
	if ((mask & (W_OK | A_OK)) && !fuse4fs_is_writeable(ff))
		return -EROFS;

	err = ext2fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);
	perms = inode.i_mode & 0777;

	dbg_printf(ff, "access ino=%d mask=e%s%s%s%s%s perms=0%o iflags=0x%x "
		   "fuid=%d fgid=%d uid=%d gid=%d\n", ino,
		   (mask & R_OK ? "r" : ""),
		   (mask & W_OK ? "w" : ""),
		   (mask & X_OK ? "x" : ""),
		   (mask & A_OK ? "a" : ""),
		   (mask & L_OK ? "l" : ""),
		   perms, inode.i_flags,
		   inode_uid(inode), inode_gid(inode),
		   ctxt->uid, ctxt->gid);

	if (mask & L_OK) {
		/* linked files cannot be on the unlinked list or deleted */
		if (inode.i_dtime != 0) {
			dbg_printf(ff, "%s: unlinked ino=%d dtime=0x%x\n",
				   __func__, ino, inode.i_dtime);
			return -ENOENT;
		}
	} else {
		/* unlinked files cannot be deleted */
		if (inode.i_dtime >= fs->super->s_inodes_count) {
			dbg_printf(ff, "%s: deleted ino=%d dtime=0x%x\n",
				   __func__, ino, inode.i_dtime);
			return -ENOENT;
		}
	}

	/* existence check */
	if (mask == 0)
		return 0;

	ret = fuse4fs_iflags_access(ff, ino, &inode, mask);
	if (ret)
		return ret;

	/* If kernel is responsible for mode and acl checks, we're done. */
	if (ff->kernel)
		return 0;

	/* Figure out what root's allowed to do */
	if (fuse4fs_is_superuser(ff, ctxt)) {
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

static errcode_t fuse4fs_check_support(struct fuse4fs *ff)
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

static errcode_t fuse4fs_acquire_lockfile(struct fuse4fs *ff)
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

static void fuse4fs_release_lockfile(struct fuse4fs *ff)
{
	if (unlink(ff->lockfile)) {
		errcode_t err = errno;

		err_printf(ff, "%s: %s: %s\n", ff->lockfile,
			   _("removing lockfile failed"),
			   strerror(err));
	}
	free(ff->lockfile);
}

#ifdef HAVE_FUSE_LOOPDEV
static int fuse4fs_try_losetup(struct fuse4fs *ff, int flags)
{
	bool rw = flags & EXT2_FLAG_RW;
	int dev_fd;
	int ret;

	/* Only transform a regular file into a loopdev for iomap */
	if (!fuse4fs_can_iomap(ff))
		return 0;

	/* open the actual target device, see if it's a regular file */
	dev_fd = open(ff->device, rw ? O_RDWR : O_RDONLY);
	if (dev_fd < 0) {
		err_printf(ff, "%s: %s\n", _("while opening fs"),
			   error_message(errno));
		return -1;
	}

	ret = fuse_loopdev_setup(dev_fd, rw ? O_RDWR : O_RDONLY, ff->device, 5,
			   &ff->loop_fd, &ff->loop_device);
	if (ret && errno == EBUSY) {
		/*
		 * If the setup function returned EBUSY, there is already a
		 * loop device backed by this file.  Report that the file is
		 * already in use.
		 */
		err_printf(ff, "%s: %s\n", _("while opening fs loopdev"),
				   error_message(errno));
		close(dev_fd);
		return -1;
	}

	close(dev_fd);
	return 0;
}

static void fuse4fs_detach_losetup(struct fuse4fs *ff)
{
	if (ff->loop_fd >= 0)
		close(ff->loop_fd);
	ff->loop_fd = -1;
}

static void fuse4fs_undo_losetup(struct fuse4fs *ff)
{
	fuse4fs_detach_losetup(ff);
	free(ff->loop_device);
	ff->loop_device = NULL;
}

static inline const char *fuse4fs_device(const struct fuse4fs *ff)
{
	/*
	 * If we created a loop device for the file passed in, open that.
	 * Otherwise open the path the user gave us.
	 */
	return ff->loop_device ? ff->loop_device : ff->device;
}
#else
# define fuse4fs_try_losetup(...)	(0)
# define fuse4fs_detach_losetup(...)	((void)0)
# define fuse4fs_undo_losetup(...)	((void)0)
# define fuse4fs_device(ff)		((ff)->device)
#endif

static void fuse4fs_unmount(struct fuse4fs *ff)
{
	char uuid[UUID_STR_SIZE];
	errcode_t err;

	if (ff->fs) {
		if (cache_initialized(&ff->inodes)) {
			cache_purge(&ff->inodes);
			cache_destroy(&ff->inodes);
		}

		fuse4fs_mmp_stop(ff);

		uuid_unparse(ff->fs->super->s_uuid, uuid);
		err = ext2fs_close_free(&ff->fs);
		if (err)
			err_printf(ff, "%s: %s\n", _("while closing fs"),
				   error_message(err));

		if (ff->kernel)
			log_printf(ff, "%s %s.\n", _("unmounted filesystem"),
				   uuid);
	}

	fuse4fs_undo_losetup(ff);

	if (ff->lockfile)
		fuse4fs_release_lockfile(ff);
}

static errcode_t fuse4fs_open(struct fuse4fs *ff)
{
	char options[128];
	int flags = EXT2_FLAG_64BITS | EXT2_FLAG_THREADS | EXT2_FLAG_RW |
		    EXT2_FLAG_EXCLUSIVE | EXT2_FLAG_WRITE_FULL_SUPER;
	errcode_t err;

	if (ff->lockfile) {
		err = fuse4fs_acquire_lockfile(ff);
		if (err)
			return err;
	}

	snprintf(options, sizeof(options) - 1, "offset=%lu", ff->offset);
	ff->opstate = F4OP_READONLY;

	if (ff->directio)
		flags |= EXT2_FLAG_DIRECT_IO;

	dbg_printf(ff, "opening with flags=0x%x\n", flags);

	err = fuse4fs_try_losetup(ff, flags);
	if (err)
		return err;

	err = ext2fs_open2(fuse4fs_device(ff), options, flags, 0, 0,
			   unix_io_manager, &ff->fs);
	if (err == EPERM || err == EACCES) {
		/*
		 * Source device cannot be opened for write.  Under these
		 * circumstances, mount(8) will try again with a ro mount,
		 * and the kernel will open the block device readonly.
		 */
		log_printf(ff, "%s\n",
 _("WARNING: source write-protected, mounted read-only."));
		flags &= ~EXT2_FLAG_RW;
		ff->ro = 1;

		fuse4fs_undo_losetup(ff);
		err = fuse4fs_try_losetup(ff, flags);
		if (err)
			return err;

		err = ext2fs_open2(fuse4fs_device(ff), options, flags, 0, 0,
				   unix_io_manager, &ff->fs);
	}
	if (err) {
		err_printf(ff, "%s.\n", error_message(err));
		err_printf(ff, "%s\n", _("Please run e2fsck -fy."));
		return err;
	}

	if (ff->kernel) {
		char uuid[UUID_STR_SIZE];

		uuid_unparse(ff->fs->super->s_uuid, uuid);
		log_printf(ff, "%s %s.\n", _("mounted filesystem"), uuid);
	}

	err = cache_init(CACHE_AUTO_SHRINK, 1U << 10, &icache_ops, &ff->inodes);
	if (err)
		return translate_error(ff->fs, 0, err);

	ff->fs->priv_data = ff;
	ff->blocklog = u_log2(ff->fs->blocksize);
	ff->blockmask = ff->fs->blocksize - 1;

	fuse4fs_mmp_config(ff);
	return 0;
}

static errcode_t fuse4fs_config_cache(struct fuse4fs *ff)
{
	char buf[128];
	errcode_t err;

	snprintf(buf, sizeof(buf), "cache_blocks=%llu",
		 FUSE4FS_B_TO_FSBT(ff, ff->cache_size));
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

static inline bool fuse4fs_on_bdev(const struct fuse4fs *ff)
{
	return ff->fs->io->flags & CHANNEL_FLAGS_BLOCK_DEVICE;
}

static int fuse4fs_mount(struct fuse4fs *ff)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	errcode_t err;

	if (ext2fs_has_feature_journal_needs_recovery(fs->super)) {
		if (ff->norecovery) {
			log_printf(ff, "%s\n",
 _("Mounting read-only without recovering journal."));
			ff->ro = 1;
			ff->fs->flags &= ~EXT2_FLAG_RW;
		} else if (!(fs->flags & EXT2_FLAG_RW)) {
			err_printf(ff, "%s\n",
 _("Cannot replay journal on read-only device."));
			return -1;
		} else {
			log_printf(ff, "%s\n", _("Recovering journal."));
			err = ext2fs_run_ext3_journal(&ff->fs);
			if (err) {
				err_printf(ff, "%s.\n", error_message(err));
				err_printf(ff, "%s\n",
						_("Please run e2fsck -fy."));
				return translate_error(fs, 0, err);
			}
			fs = ff->fs;

			err = fuse4fs_check_support(ff);
			if (err)
				return err;
		}
	} else if (ext2fs_has_feature_journal(fs->super)) {
		err = ext2fs_check_ext3_journal(fs);
		if (err)
			return translate_error(fs, 0, err);
	}

	/* Make sure the root directory is readable. */
	err = fuse4fs_read_inode(fs, EXT2_ROOT_INO, &inode);
	if (err)
		return translate_error(fs, EXT2_ROOT_INO, err);

	if (fs->flags & EXT2_FLAG_RW) {
		if (ext2fs_has_feature_journal(fs->super))
			log_printf(ff, "%s",
 _("Warning: fuse4fs does not support using the journal.\n"
   "There may be file system corruption or data loss if\n"
   "the file system is not gracefully unmounted.\n"));
		ff->opstate = F4OP_WRITABLE;
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

	/* Clear the valid flag so that an unclean shutdown forces a fsck */
	if (ff->opstate == F4OP_WRITABLE) {
		fs->super->s_mnt_count++;
		ext2fs_set_tstamp(fs->super, s_mtime, time(NULL));
		fs->super->s_state &= ~EXT2_VALID_FS;
		ext2fs_mark_super_dirty(fs);
		err = ext2fs_flush2(fs, 0);
		if (err)
			return translate_error(fs, 0, err);
	}

	return 0;
}

static void op_destroy(void *userdata)
{
	struct fuse4fs *ff = userdata;
	ext2_filsys fs;
	errcode_t err;

	FUSE4FS_CHECK_CONTEXT_DESTROY(ff);

	fs = fuse4fs_start(ff);

	dbg_printf(ff, "%s: dev=%s\n", __func__, fs->device_name);
	if (ff->opstate == F4OP_WRITABLE) {
		fs->super->s_state |= EXT2_VALID_FS;
		if (fs->super->s_error_count)
			fs->super->s_state |= EXT2_ERROR_FS;
		ext2fs_mark_super_dirty(fs);
		err = ext2fs_set_gdt_csum(fs);
		if (err)
			translate_error(fs, 0, err);

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

	/*
	 * If we're mounting in iomap mode, we need to unmount in op_destroy so
	 * that the block device will be released before umount(2) returns.
	 */
	if (ff->iomap_state == IOMAP_ENABLED)
		fuse4fs_unmount(ff);

	fuse4fs_finish(ff, 0);
}

/* Reopen @stream with @fileno */
static int fuse4fs_freopen_stream(const char *path, int fileno, FILE *stream)
{
	char _fdpath[256];
	const char *fdpath;
	FILE *fp;
	int ret;

	ret = snprintf(_fdpath, sizeof(_fdpath), "/dev/fd/%d", fileno);
	if (ret >= sizeof(_fdpath))
		fdpath = path;
	else
		fdpath = _fdpath;

	/*
	 * C23 defines std{out,err} as an expression of type FILE* that need
	 * not be an lvalue.  What this means is that we can't just assign to
	 * stdout: we have to use freopen, which takes a path.
	 *
	 * There's no guarantee that the OS provides a /dev/fd/X alias for open
	 * file descriptors, so if that fails, fall back to the original log
	 * file path.  We'd rather not do a path-based reopen because that
	 * exposes us to rename race attacks.
	 */
	fp = freopen(fdpath, "a", stream);
	if (!fp && errno == ENOENT && fdpath == _fdpath)
		fp = freopen(path, "a", stream);
	if (!fp) {
		perror(fdpath);
		return -1;
	}

	return 0;
}

/* Redirect stdout/stderr to a file, or return a mount-compatible error. */
static int fuse4fs_capture_output(struct fuse4fs *ff, const char *path)
{
	int ret;
	int fd;

	/*
	 * First, open the log file path with system calls so that we can
	 * redirect the stdout/stderr file numbers (typically 1 and 2) to our
	 * logfile descriptor.  We'd like to avoid allocating extra file
	 * objects in the kernel if we can because pos will be the same between
	 * stdout and stderr.
	 */
	if (ff->logfd < 0) {
		fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd < 0) {
			perror(path);
			return -1;
		}

		/*
		 * Save the newly opened fd in case we have to do this again in
		 * op_init.
		 */
		ff->logfd = fd;
	}

	ret = dup2(ff->logfd, STDOUT_FILENO);
	if (ret < 0) {
		perror(path);
		return -1;
	}

	ret = dup2(ff->logfd, STDERR_FILENO);
	if (ret < 0) {
		perror(path);
		return -1;
	}

	/*
	 * Now that we've changed STD{OUT,ERR}_FILENO to be the log file, use
	 * freopen to make sure that std{out,err} (the C library abstractions)
	 * point to the STDXXX_FILENO because any of our library dependencies
	 * might decide to printf to one of those streams and we want to
	 * capture all output in the log.
	 */
	ret = fuse4fs_freopen_stream(path, STDOUT_FILENO, stdout);
	if (ret)
		return ret;
	ret = fuse4fs_freopen_stream(path, STDERR_FILENO, stderr);
	if (ret)
		return ret;

	return 0;
}

/* Set up debug and error logging files */
static int fuse4fs_setup_logging(struct fuse4fs *ff)
{
	char *logfile = getenv("FUSE4FS_LOGFILE");
	if (logfile)
		return fuse4fs_capture_output(ff, logfile);

	/* in kernel mode, try to log errors to the kernel log */
	if (ff->kernel)
		fuse4fs_capture_output(ff, "/dev/ttyprintk");

	return 0;
}

static int fuse4fs_read_bitmaps(struct fuse4fs *ff)
{
	errcode_t err;

	err = ext2fs_read_inode_bitmap(ff->fs);
	if (err)
		return translate_error(ff->fs, 0, err);

	err = ext2fs_read_block_bitmap(ff->fs);
	if (err)
		return translate_error(ff->fs, 0, err);

	return 0;
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
static void fuse4fs_iomap_enable(struct fuse_conn_info *conn,
				 struct fuse4fs *ff)
{
	/* Don't let anyone touch iomap until the end of the patchset. */
	ff->iomap_state = IOMAP_DISABLED;
	return;

	/* iomap only works with block devices */
	if (ff->iomap_state != IOMAP_DISABLED && fuse4fs_on_bdev(ff) &&
	    fuse_set_feature_flag(conn, FUSE_CAP_IOMAP))
		ff->iomap_state = IOMAP_ENABLED;

	if (ff->iomap_state == IOMAP_UNKNOWN)
		ff->iomap_state = IOMAP_DISABLED;

	if (!fuse4fs_iomap_enabled(ff)) {
		if (ff->iomap_want == FT_ENABLE)
			err_printf(ff, "%s\n", _("Could not enable iomap."));
		if (ff->iomap_passthrough_options)
			err_printf(ff, "%s\n", _("Some mount options require iomap."));
		return;
	}
}
#else
# define fuse4fs_iomap_enable(...)	((void)0)
#endif

static void op_init(void *userdata, struct fuse_conn_info *conn)
{
	struct fuse4fs *ff = userdata;
	ext2_filsys fs;

	FUSE4FS_CHECK_CONTEXT_INIT(ff);

	fs = ff->fs;
	dbg_printf(ff, "%s: dev=%s\n", __func__, fs->device_name);
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
	fuse4fs_iomap_enable(conn, ff);
	conn->time_gran = 1;

	fuse4fs_detach_losetup(ff);

	if (ff->opstate == F4OP_WRITABLE)
		fuse4fs_read_bitmaps(ff);

	/*
	 * Background threads must be started from op_init because libfuse
	 * might daemonize us in fuse_main() by forking and execing, which
	 * kills any background threads.
	 */
	fuse4fs_mmp_start(ff);

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 17)
	/*
	 * THIS MUST GO LAST!
	 *
	 * fuse_set_feature_flag in 3.17.0 has a strange bug: it sets feature
	 * flags in conn->want_ext, but not conn->want.  Upon return to
	 * libfuse, the lower level library observes that want and want_ext
	 * have gotten out of sync, and refuses to mount.  Therefore,
	 * synchronize the two.  This bug went away in 3.17.3, but we're stuck
	 * with this forever because Debian trixie released with 3.17.2.
	 */
	conn->want = conn->want_ext & 0xFFFFFFFF;
#endif
}

struct fuse4fs_stat {
	struct fuse_entry_param	entry;
	unsigned int iflags;
};

static int fuse4fs_stat_inode(struct fuse4fs *ff, ext2_ino_t ino,
			      struct ext2_inode_large *inodep,
			      struct fuse4fs_stat *fstat)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	struct fuse_entry_param *entry = &fstat->entry;
	struct stat *statbuf = &entry->attr;
	dev_t fakedev = 0;
	errcode_t err;
	struct timespec tv;

	memset(fstat, 0, sizeof(*fstat));

	if (!inodep) {
		err = fuse4fs_read_inode(fs, ino, &inode);
		if (err)
			return translate_error(fs, ino, err);
		inodep = &inode;
	}

	memcpy(&fakedev, fs->super->s_uuid, sizeof(fakedev));
	statbuf->st_dev = fakedev;
	statbuf->st_ino = ino;
	statbuf->st_mode = inodep->i_mode;
	statbuf->st_nlink = inodep->i_links_count;
	statbuf->st_uid = inode_uid(*inodep);
	statbuf->st_gid = inode_gid(*inodep);
	statbuf->st_size = EXT2_I_SIZE(inodep);
	statbuf->st_blksize = fs->blocksize;
	statbuf->st_blocks = ext2fs_get_stat_i_blocks(fs,
						EXT2_INODE(inodep));
	EXT4_INODE_GET_XTIME(i_atime, &tv, inodep);
#if HAVE_STRUCT_STAT_ST_ATIM
	statbuf->st_atim = tv;
#else
	statbuf->st_atime = tv.tv_sec;
#endif
	EXT4_INODE_GET_XTIME(i_mtime, &tv, inodep);
#if HAVE_STRUCT_STAT_ST_ATIM
	statbuf->st_mtim = tv;
#else
	statbuf->st_mtime = tv.tv_sec;
#endif
	EXT4_INODE_GET_XTIME(i_ctime, &tv, inodep);
#if HAVE_STRUCT_STAT_ST_ATIM
	statbuf->st_ctim = tv;
#else
	statbuf->st_ctime = tv.tv_sec;
#endif
	if (LINUX_S_ISCHR(inodep->i_mode) ||
	    LINUX_S_ISBLK(inodep->i_mode)) {
		if (inodep->i_block[0])
			statbuf->st_rdev = inodep->i_block[0];
		else
			statbuf->st_rdev = inodep->i_block[1];
	}

	fuse4fs_ino_to_fuse(ff, &entry->ino, ino);
	entry->generation = inodep->i_generation;
	entry->attr_timeout = FUSE4FS_ATTR_TIMEOUT;
	entry->entry_timeout = FUSE4FS_ATTR_TIMEOUT;

	fstat->iflags = 0;
#ifdef HAVE_FUSE_IOMAP
	if (fuse4fs_iomap_enabled(ff)) {
		fstat->iflags |= FUSE_IFLAG_IOMAP;

		if (fuse4fs_iomap_supports_hw_atomic(ff))
			fstat->iflags |= FUSE_IFLAG_ATOMIC;
	}
#endif

	return 0;
}

#if FUSE_VERSION < FUSE_MAKE_VERSION(3, 99)
#define fuse_reply_entry_iflags(req, entry, iflags) \
	fuse_reply_entry((req), (entry))

#define fuse_reply_attr_iflags(req, entry, iflags, timeout) \
	fuse_reply_attr((req), (entry), (timeout))

#define fuse_add_direntry_plus_iflags(req, buf, sz, name, iflags, entry, dirpos) \
	fuse_add_direntry_plus((req), (buf), (sz), (name), (entry), (dirpos))

#define fuse_reply_create_iflags(req, entry, iflags, fp) \
	fuse_reply_create((req), (entry), (fp))
#endif

static void op_lookup(fuse_req_t req, fuse_ino_t fino, const char *name)
{
	struct fuse4fs_stat fstat;
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	dbg_printf(ff, "%s: parent=%d name='%s'\n", __func__, parent, name);
	fs = fuse4fs_start(ff);

	err = ext2fs_namei(fs, EXT2_ROOT_INO, parent, name, &child);
	if (err || child == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	ret = fuse4fs_stat_inode(ff, child, NULL, &fstat);
	if (ret)
		goto out;

out:
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_entry_iflags(req, &fstat.entry, fstat.iflags);
}

static void op_getattr(fuse_req_t req, fuse_ino_t fino,
		       struct fuse_file_info *fi EXT2FS_ATTR((unused)))
{
	struct fuse4fs_stat fstat;
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_ino_t ino;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fuse4fs_start(ff);
	ret = fuse4fs_stat_inode(ff, ino, NULL, &fstat);
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_attr_iflags(req, &fstat.entry.attr, fstat.iflags,
				       fstat.entry.attr_timeout);
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 18) && defined(STATX_BASIC_STATS)
static inline void fuse4fs_set_statx_attr(struct statx *stx,
					  uint64_t statx_flag, int set)
{
	if (set)
		stx->stx_attributes |= statx_flag;
	stx->stx_attributes_mask |= statx_flag;
}

static void fuse4fs_statx_directio(struct fuse4fs *ff, struct statx *stx)
{
	struct statx devx;
	errcode_t err;
	int fd;

	err = io_channel_get_fd(ff->fs->io, &fd);
	if (err)
		return;

	err = statx(fd, "", AT_EMPTY_PATH, STATX_DIOALIGN, &devx);
	if (err)
		return;
	if (!(devx.stx_mask & STATX_DIOALIGN))
		return;

	stx->stx_mask |= STATX_DIOALIGN;
	stx->stx_dio_mem_align = devx.stx_dio_mem_align;
	stx->stx_dio_offset_align = devx.stx_dio_offset_align;
}

static int fuse4fs_statx(struct fuse4fs *ff, ext2_ino_t ino, int statx_mask,
			 struct statx *stx)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;;
	dev_t fakedev = 0;
	errcode_t err;
	struct timespec tv;

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	memcpy(&fakedev, fs->super->s_uuid, sizeof(fakedev));
	stx->stx_mask = STATX_BASIC_STATS;
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

	if (EXT4_FITS_IN_INODE(&inode, i_crtime)) {
		stx->stx_mask |= STATX_BTIME;
		EXT4_INODE_GET_XTIME(i_crtime, &tv, &inode);
		stx->stx_btime.tv_sec = tv.tv_sec;
		stx->stx_btime.tv_nsec = tv.tv_nsec;
	}

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

	fuse4fs_set_statx_attr(stx, STATX_ATTR_COMPRESSED,
			       inode.i_flags & EXT2_COMPR_FL);
	fuse4fs_set_statx_attr(stx, STATX_ATTR_IMMUTABLE,
			       inode.i_flags & EXT2_IMMUTABLE_FL);
	fuse4fs_set_statx_attr(stx, STATX_ATTR_APPEND,
			       inode.i_flags & EXT2_APPEND_FL);
	fuse4fs_set_statx_attr(stx, STATX_ATTR_NODUMP,
			       inode.i_flags & EXT2_NODUMP_FL);

	fuse4fs_statx_directio(ff, stx);

#ifdef STATX_WRITE_ATOMIC
	if (fuse4fs_iomap_supports_hw_atomic(ff)) {
		stx->stx_mask |= STATX_WRITE_ATOMIC;
		stx->stx_atomic_write_unit_min = ff->awu_min;
		stx->stx_atomic_write_unit_max = ff->awu_max;
		stx->stx_atomic_write_segments_max = 1;
	}
#endif

	return 0;
}

static void op_statx(fuse_req_t req, fuse_ino_t fino, int flags, int mask,
		     struct fuse_file_info *fi)
{
	struct statx stx;
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_ino_t ino;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fuse4fs_start(ff);
	ret = fuse4fs_statx(ff, ino, mask, &stx);
	if (ret)
		goto out;
out:
	fuse4fs_finish(ff, ret);
	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_statx(req, 0, &stx, FUSE4FS_ATTR_TIMEOUT);
}
#else
# define op_statx		NULL
#endif

static void op_readlink(fuse_req_t req, fuse_ino_t fino)
{
	struct ext2_inode inode;
	char buf[PATH_MAX + 1];
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_file_t file;
	errcode_t err;
	ext2_ino_t ino;
	size_t len = PATH_MAX;
	unsigned int got;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	dbg_printf(ff, "%s: ino=%d\n", __func__, ino);
	fs = fuse4fs_start(ff);

	err = ext2fs_read_inode(fs, fino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	if (!LINUX_S_ISLNK(inode.i_mode)) {
		ret = -EINVAL;
		goto out;
	}

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
		if (err)
			ret = translate_error(fs, ino, err);
		else if (got != len)
			ret = translate_error(fs, ino, EXT2_ET_INODE_CORRUPTED);

		err = ext2fs_file_close(file);
		if (ret)
			goto out;
		if (err) {
			ret = translate_error(fs, ino, err);
			goto out;
		}
	}
	buf[len] = 0;

	if (fuse4fs_is_writeable(ff)) {
		ret = fuse4fs_update_atime(ff, ino);
		if (ret)
			goto out;
	}

out:
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_readlink(req, buf);
}

static int fuse4fs_getxattr(struct fuse4fs *ff, ext2_ino_t ino,
			    const char *name, void **value, size_t *value_len)
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

static int fuse4fs_setxattr(struct fuse4fs *ff, ext2_ino_t ino,
			    const char *name, void *value, size_t valuelen)
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

static int fuse4fs_propagate_default_acls(struct fuse4fs *ff, ext2_ino_t parent,
					  ext2_ino_t child, mode_t mode)
{
	void *def;
	size_t deflen;
	int ret;

	if (!ff->acl || S_ISDIR(mode) || fuse4fs_iomap_enabled(ff))
		return 0;

	ret = fuse4fs_getxattr(ff, parent, XATTR_NAME_POSIX_ACL_DEFAULT, &def,
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

	ret = fuse4fs_setxattr(ff, child, XATTR_NAME_POSIX_ACL_DEFAULT, def,
			       deflen);
	ext2fs_free_mem(&def);
	return ret;
}

static inline void fuse4fs_set_uid(struct ext2_inode_large *inode, uid_t uid)
{
	inode->i_uid = uid;
	ext2fs_set_i_uid_high(*inode, uid >> 16);
}

static inline void fuse4fs_set_gid(struct ext2_inode_large *inode, gid_t gid)
{
	inode->i_gid = gid;
	ext2fs_set_i_gid_high(*inode, gid >> 16);
}

static int fuse4fs_new_child_gid(struct fuse4fs *ff,
				 const struct fuse_ctx *ctxt,
				 ext2_ino_t parent, gid_t *gid,
				 int *parent_sgid)
{
	struct ext2_inode_large inode;
	errcode_t err;

	err = fuse4fs_read_inode(ff->fs, parent, &inode);
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

/*
 * Flush dirty data to disk if we're running in dirsync mode.  If @flushed is a
 * non-null pointer, this function sets @flushed to 1 if we decided to flush
 * data, or 0 if not.
 */
static inline int fuse4fs_dirsync_flush(struct fuse4fs *ff, ext2_ino_t ino,
					int *flushed)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	errcode_t err;

	if (ff->dirsync)
		goto flush;

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, 0, err);

	if (inode.i_flags & EXT2_DIRSYNC_FL)
		goto flush;

	if (flushed)
		*flushed = 0;
	return 0;
flush:
	err = ext2fs_flush2(fs, 0);
	if (err)
		return translate_error(fs, 0, err);

	if (flushed)
		*flushed = 1;
	return 0;
}

static void fuse4fs_set_extra_isize(struct fuse4fs *ff, ext2_ino_t ino,
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

static void fuse4fs_reply_entry(fuse_req_t req, ext2_ino_t ino,
				struct ext2_inode_large *inode, int ret)
{
	struct fuse4fs_stat fstat;
	struct fuse4fs *ff = fuse4fs_get(req);

	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	/* Get stat info for the new entry */
	ret = fuse4fs_stat_inode(ff, ino, inode, &fstat);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	fuse_reply_entry_iflags(req, &fstat.entry, fstat.iflags);
}

static void op_mknod(fuse_req_t req, fuse_ino_t fino, const char *name,
		     mode_t mode, dev_t dev)
{
	struct ext2_inode_large inode;
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	int filetype;
	gid_t gid;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	dbg_printf(ff, "%s: parent=%d name='%s' mode=0%o dev=0x%x\n",
		   __func__, parent, name, mode, (unsigned int)dev);

	fs = fuse4fs_start(ff);
	if (!fuse4fs_can_allocate(ff, 2)) {
		ret = -ENOSPC;
		goto out2;
	}

	ret = fuse4fs_inum_access(ff, ctxt, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	/* On a low level server, mknod handles all non-directory types */
	filetype = ext2_file_type(mode);

	err = fuse4fs_new_child_gid(ff, ctxt, parent, &gid, NULL);
	if (err)
		goto out2;

	err = ext2fs_new_inode(fs, parent, mode, 0, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}

	dbg_printf(ff, "%s: create ino=%d name='%s' in dir=%d\n", __func__,
		   child, name, parent);
	err = ext2fs_link(fs, parent, name, child,
			  filetype | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse4fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	memset(&inode, 0, sizeof(inode));
	inode.i_mode = mode;

	if (dev & ~0xFFFF)
		inode.i_block[1] = dev;
	else
		inode.i_block[0] = dev;
	inode.i_links_count = 1;
	fuse4fs_set_extra_isize(ff, child, &inode);
	fuse4fs_set_uid(&inode, ctxt->uid);
	fuse4fs_set_gid(&inode, gid);

	err = ext2fs_write_new_inode(fs, child, EXT2_INODE(&inode));
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	inode.i_generation = ff->next_generation++;
	fuse4fs_init_timestamps(ff, &inode);
	err = fuse4fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ext2fs_inode_alloc_stats2(fs, child, 1, 0);

	ret = fuse4fs_propagate_default_acls(ff, parent, child, inode.i_mode);
	if (ret)
		goto out2;

	ret = fuse4fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse4fs_finish(ff, ret);
	fuse4fs_reply_entry(req, child, &inode, ret);
}

static void op_mkdir(fuse_req_t req, fuse_ino_t fino, const char *name,
		     mode_t mode)
{
	struct ext2_inode_large inode;
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	char *block;
	blk64_t blk;
	int ret = 0;
	gid_t gid;
	int parent_sgid;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	dbg_printf(ff, "%s: parent=%d name='%s' mode=0%o\n",
		   __func__, parent, name, mode);

	fs = fuse4fs_start(ff);
	if (!fuse4fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out2;
	}

	ret = fuse4fs_inum_access(ff, ctxt, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse4fs_new_child_gid(ff, ctxt, parent, &gid, &parent_sgid);
	if (err)
		goto out2;

	err = ext2fs_mkdir2(fs, parent, 0, 0, EXT2FS_LINK_EXPAND, name, NULL);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse4fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	/* Still have to update the uid/gid of the dir */
	err = ext2fs_namei(fs, EXT2_ROOT_INO, parent, name, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}
	dbg_printf(ff, "%s: created ino=%d name='%s' in dir=%d\n",
		   __func__, child, name, parent);

	err = fuse4fs_read_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	fuse4fs_set_extra_isize(ff, child, &inode);
	fuse4fs_set_uid(&inode, ctxt->uid);
	fuse4fs_set_gid(&inode, gid);
	inode.i_mode = LINUX_S_IFDIR | (mode & ~S_ISUID);
	if (parent_sgid)
		inode.i_mode |= S_ISGID;
	inode.i_generation = ff->next_generation++;
	fuse4fs_init_timestamps(ff, &inode);

	err = fuse4fs_write_inode(fs, child, &inode);
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

	ret = fuse4fs_propagate_default_acls(ff, parent, child, inode.i_mode);
	if (ret)
		goto out3;

	ret = fuse4fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out3;

out3:
	ext2fs_free_mem(&block);
out2:
	fuse4fs_finish(ff, ret);
	fuse4fs_reply_entry(req, child, &inode, ret);
}

static int fuse4fs_remove_ea_inodes(struct fuse4fs *ff, ext2_ino_t ino,
				    struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	struct ext2_xattr_handle *h;
	errcode_t err;
	int ret = 0;

	/*
	 * The xattr handle maintains its own private copy of the inode, so
	 * write ours to disk so that we can read it.
	 */
	err = fuse4fs_write_inode(fs, ino, inode);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_xattrs_read(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

	err = ext2fs_xattr_remove_all(h);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_close;
	}

out_close:
	ext2fs_xattrs_close(&h);
	if (ret)
		return ret;

	/* Now read the inode back in. */
	err = fuse4fs_read_inode(fs, ino, inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse4fs_add_to_orphans(struct fuse4fs *ff, ext2_ino_t ino,
				  struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	struct fuse4fs_inode *fi;
	ext2_ino_t orphan_ino = fs->super->s_last_orphan;
	errcode_t err;

	dbg_printf(ff, "%s: orphan ino=%d dtime=%d next=%d\n",
		   __func__, ino, inode->i_dtime, fs->super->s_last_orphan);

	/* Make the first orphan on the list point back to us */
	if (orphan_ino != 0) {
		err = fuse4fs_iget(ff, orphan_ino, &fi);
		if (err)
			return translate_error(fs, orphan_ino, err);

		fi->i_prev_orphan = ino;
		fuse4fs_iput(ff, fi);
	}

	/* Add ourselves to the head of the orphan list */
	err = fuse4fs_iget(ff, ino, &fi);
	if (err)
		return translate_error(fs, ino, err);

	fi->i_prev_orphan = 0;
	fuse4fs_iput(ff, fi);

	inode->i_dtime = fs->super->s_last_orphan;
	fs->super->s_last_orphan = ino;
	ext2fs_mark_super_dirty(fs);

	return 0;
}

/*
 * Given the orphan list excerpt: prev_orphan -> ino -> next_orphan, set
 * next_orphan's backpointer to ino's backpointer (prev_orphan), having removed
 * ino from the orphan list.
 */
static int fuse4fs_update_next_orphan_backlink(struct fuse4fs *ff,
					       ext2_ino_t prev_orphan,
					       ext2_ino_t ino,
					       ext2_ino_t next_orphan)
{
	struct fuse4fs_inode *fi;
	errcode_t err;
	int ret = 0;

	err = fuse4fs_iget(ff, next_orphan, &fi);
	if (err)
		return translate_error(ff->fs, next_orphan, err);

	dbg_printf(ff, "%s: ino=%d cached next=%d nextprev=%d prev=%d\n",
		   __func__, ino, next_orphan, fi->i_prev_orphan,
		   prev_orphan);

	if (fi->i_prev_orphan != ino) {
		ret = translate_error(ff->fs, next_orphan,
				      EXT2_ET_FILESYSTEM_CORRUPTED);
		goto out_iput;
	}

	fi->i_prev_orphan = prev_orphan;
out_iput:
	fuse4fs_iput(ff, fi);
	return ret;
}

/*
 * Remove ino from the orphan list the fast way.  Returns 1 for success, 0 if
 * it didn't do anything, or a negative errno.
 */
static int fuse4fs_fast_remove_from_orphans(struct fuse4fs *ff, ext2_ino_t ino,
					    struct ext2_inode_large *inode)
{
	struct ext2_inode_large orphan;
	ext2_filsys fs = ff->fs;
	struct fuse4fs_inode *fi;
	ext2_ino_t prev_orphan;
	ext2_ino_t next_orphan = 0;
	errcode_t err;
	int ret = 0;

	err = fuse4fs_iget(ff, ino, &fi);
	if (err)
		return translate_error(fs, ino, err);

	prev_orphan = fi->i_prev_orphan;
	switch (prev_orphan) {
	case 0:
		/* First inode in the list */
		dbg_printf(ff, "%s: ino=%d cached superblock\n", __func__, ino);

		fs->super->s_last_orphan = inode->i_dtime;
		next_orphan = inode->i_dtime;
		inode->i_dtime = 0;
		ext2fs_mark_super_dirty(fs);
		fi->i_prev_orphan = FUSE4FS_NULL_INO;
		break;
	case FUSE4FS_NULL_INO:
		/* unknown */
		dbg_printf(ff, "%s: ino=%d broken list??\n", __func__, ino);
		ret = 0;
		goto out_iput;
	default:
		/* We're in the middle of the list */
		err = fuse4fs_read_inode(fs, prev_orphan, &orphan);
		if (err) {
			ret = translate_error(fs, prev_orphan, err);
			goto out_iput;
		}

		dbg_printf(ff,
 "%s: ino=%d cached prev=%d prevnext=%d next=%d\n",
			   __func__, ino, prev_orphan, orphan.i_dtime,
			   inode->i_dtime);

		if (orphan.i_dtime != ino) {
			ret = translate_error(fs, prev_orphan,
					      EXT2_ET_FILESYSTEM_CORRUPTED);
			goto out_iput;
		}

		fi->i_prev_orphan = FUSE4FS_NULL_INO;
		orphan.i_dtime = inode->i_dtime;
		next_orphan = inode->i_dtime;
		inode->i_dtime = 0;

		err = fuse4fs_write_inode(fs, prev_orphan, &orphan);
		if (err) {
			ret = translate_error(fs, prev_orphan, err);
			goto out_iput;
		}

		break;
	}

	/*
	 * Make the next orphaned inode point back to the our own previous list
	 * entry
	 */
	if (next_orphan != 0) {
		ret = fuse4fs_update_next_orphan_backlink(ff, prev_orphan, ino,
							  next_orphan);
		if (ret)
			goto out_iput;
	}
	ret = 1;

out_iput:
	fuse4fs_iput(ff, fi);
	return ret;
}

static int fuse4fs_remove_from_orphans(struct fuse4fs *ff, ext2_ino_t ino,
				       struct ext2_inode_large *inode)
{
	ext2_filsys fs = ff->fs;
	ext2_ino_t prev_orphan;
	ext2_ino_t next_orphan;
	errcode_t err;
	int ret;

	dbg_printf(ff, "%s: super=%d ino=%d next=%d\n",
		   __func__, fs->super->s_last_orphan, ino, inode->i_dtime);

	/*
	 * Fast way: use the incore list, which doesn't include any orphans
	 * that were already on the superblock when we mounted.
	 */
	ret = fuse4fs_fast_remove_from_orphans(ff, ino, inode);
	if (ret < 0)
		return ret;
	if (ret == 1)
		return 0;

	/* Slow way: If we're lucky, the ondisk superblock points to us */
	if (fs->super->s_last_orphan == ino) {
		dbg_printf(ff, "%s: superblock\n", __func__);

		next_orphan = inode->i_dtime;
		fs->super->s_last_orphan = inode->i_dtime;
		inode->i_dtime = 0;
		ext2fs_mark_super_dirty(fs);
		return fuse4fs_update_next_orphan_backlink(ff, 0, ino,
							   next_orphan);
	}

	/* Otherwise walk the ondisk orphan list. */
	prev_orphan = fs->super->s_last_orphan;
	while (prev_orphan != 0) {
		struct ext2_inode_large orphan;

		err = fuse4fs_read_inode(fs, prev_orphan, &orphan);
		if (err)
			return translate_error(fs, prev_orphan, err);

		if (orphan.i_dtime == prev_orphan)
			return translate_error(fs, prev_orphan,
					       EXT2_ET_FILESYSTEM_CORRUPTED);

		if (orphan.i_dtime == ino) {
			dbg_printf(ff, "%s: prev=%d\n",
				   __func__, prev_orphan);

			next_orphan = inode->i_dtime;
			orphan.i_dtime = inode->i_dtime;
			inode->i_dtime = 0;

			err = fuse4fs_write_inode(fs, prev_orphan, &orphan);
			if (err)
				return translate_error(fs, prev_orphan, err);

			return fuse4fs_update_next_orphan_backlink(ff,
					prev_orphan, ino, next_orphan);
		}

		dbg_printf(ff, "%s: orphan=%d next=%d\n",
			   __func__, prev_orphan, orphan.i_dtime);
		prev_orphan = orphan.i_dtime;
	}

	return translate_error(fs, ino, EXT2_ET_FILESYSTEM_CORRUPTED);
}

static int fuse4fs_remove_inode(struct fuse4fs *ff, ext2_ino_t ino)
{
	ext2_filsys fs = ff->fs;
	struct fuse4fs_inode *fi;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret = 0;

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	dbg_printf(ff, "%s: put ino=%d links=%d\n", __func__, ino,
		   inode.i_links_count);

	if (S_ISDIR(inode.i_mode)) {
		/*
		 * Caller should have checked that this is an empty directory
		 * before starting the unlink process.  nlink is usually 2, but
		 * it could be 1 if this dir ever had more than 65000 subdirs.
		 * Zero the link count.
		 */
		if (!ext2fs_dir_link_empty(EXT2_INODE(&inode)))
			return translate_error(fs, ino, EXT2_ET_INODE_CORRUPTED);
		inode.i_links_count = 0;
	} else {
		/*
		 * Any other file type can be hardlinked, so all we need to do
		 * is decrement the nlink.
		 */
		if (inode.i_links_count == 0)
			return translate_error(fs, ino, EXT2_ET_INODE_CORRUPTED);
		inode.i_links_count--;
	}

	ret = fuse4fs_update_ctime(ff, ino, &inode);
	if (ret)
		return ret;

	/* Still linked?  Leave it be. */
	if (inode.i_links_count)
		goto write_out;

	err = fuse4fs_iget(ff, ino, &fi);
	if (err)
		return translate_error(fs, ino, err);

	dbg_printf(ff, "%s: put ino=%d opencount=%d\n", __func__, ino,
		   fi->i_open_count);

	/*
	 * The file is unlinked but still open; add it to the orphan list and
	 * free it later.
	 */
	if (fi->i_open_count > 0) {
		fuse4fs_iput(ff, fi);
		ret = fuse4fs_add_to_orphans(ff, ino, &inode);
		if (ret)
			return ret;

		goto write_out;
	}
	fuse4fs_iput(ff, fi);

	if (ext2fs_has_feature_ea_inode(fs->super)) {
		ret = fuse4fs_remove_ea_inodes(ff, ino, &inode);
		if (ret)
			return ret;
	}

	/* Nobody holds this file; free its blocks! */
	err = ext2fs_free_ext_attr(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	if (ext2fs_inode_has_valid_blocks2(fs, EXT2_INODE(&inode))) {
		err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), NULL,
				   0, ~0ULL);
		if (err)
			return translate_error(fs, ino, err);
	}

	ext2fs_set_dtime(fs, EXT2_INODE(&inode));
	ext2fs_inode_alloc_stats2(fs, ino, -1,
				  LINUX_S_ISDIR(inode.i_mode));

write_out:
	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse4fs_unlink(struct fuse4fs *ff, ext2_ino_t parent,
			  const char *name, ext2_ino_t child)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	int ret = 0;

	err = ext2fs_unlink(fs, parent, name, child, 0);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out;
	}

	ret = fuse4fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out;
out:
	return ret;
}

static int fuse4fs_rmfile(struct fuse4fs *ff, ext2_ino_t parent,
			  const char *name, ext2_ino_t child)
{
	int ret;

	ret = fuse4fs_unlink(ff, parent, name, child);
	if (ret)
		return ret;

	return fuse4fs_remove_inode(ff, child);
}

static void op_unlink(fuse_req_t req, fuse_ino_t fino, const char *name)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	int ret;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	fs = fuse4fs_start(ff);

	/* Get the inode number for the file */
	err = ext2fs_namei(fs, EXT2_ROOT_INO, parent, name, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	ret = fuse4fs_inum_access(ff, ctxt, child, W_OK);
	if (ret)
		goto out;

	ret = fuse4fs_inum_access(ff, ctxt, parent, W_OK);
	if (ret)
		goto out;

	dbg_printf(ff, "%s: unlink parent=%d name='%s' child=%d\n",
		   __func__, parent, name, child);
	ret = fuse4fs_rmfile(ff, parent, name, child);
	if (ret)
		goto out;

	ret = fuse4fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out;
out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
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

static int fuse4fs_rmdir(struct fuse4fs *ff, ext2_ino_t parent,
			 const char *name, ext2_ino_t child)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	struct rd_struct rds = {
		.parent = 0,
		.empty = 1,
	};
	int ret = 0;

	err = ext2fs_dir_iterate2(fs, child, 0, 0, rmdir_proc, &rds);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out;
	}

	/* Make sure we found a dotdot entry */
	if (rds.parent == 0) {
		ret = translate_error(fs, child, EXT2_ET_FILESYSTEM_CORRUPTED);
		goto out;
	}

	if (rds.empty == 0) {
		ret = -ENOTEMPTY;
		goto out;
	}

	ret = fuse4fs_unlink(ff, parent, name, child);
	if (ret)
		goto out;
	ret = fuse4fs_remove_inode(ff, child);
	if (ret)
		goto out;

	if (rds.parent) {
		dbg_printf(ff, "%s: decr dir=%d link count\n", __func__,
			   rds.parent);
		err = fuse4fs_read_inode(fs, rds.parent, &inode);
		if (err) {
			ret = translate_error(fs, rds.parent, err);
			goto out;
		}
		ext2fs_dec_nlink(EXT2_INODE(&inode));
		ret = fuse4fs_update_mtime(ff, rds.parent, &inode);
		if (ret)
			goto out;
		err = fuse4fs_write_inode(fs, rds.parent, &inode);
		if (err) {
			ret = translate_error(fs, rds.parent, err);
			goto out;
		}
	}

out:
	return ret;
}

static void op_rmdir(fuse_req_t req, fuse_ino_t fino, const char *name)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	int ret;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	fs = fuse4fs_start(ff);

	err = ext2fs_namei(fs, EXT2_ROOT_INO, parent, name, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	ret = fuse4fs_inum_access(ff, ctxt, parent, W_OK);
	if (ret)
		goto out;

	ret = fuse4fs_inum_access(ff, ctxt, child, W_OK);
	if (ret)
		goto out;

	dbg_printf(ff, "%s: unlink parent=%d name='%s' child=%d\n",
		   __func__, parent, name, child);
	ret = fuse4fs_rmdir(ff, parent, name, child);
	if (ret)
		goto out;

	ret = fuse4fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out;

out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}

static void op_symlink(fuse_req_t req, const char *target, fuse_ino_t fino,
		       const char *name)
{
	struct ext2_inode_large inode;
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	gid_t gid;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	dbg_printf(ff, "%s: symlink dir=%d name='%s' target='%s'\n",
		   __func__, parent, name, target);

	fs = fuse4fs_start(ff);
	if (!fuse4fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out2;
	}

	ret = fuse4fs_inum_access(ff, ctxt, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse4fs_new_child_gid(ff, ctxt, parent, &gid, NULL);
	if (err)
		goto out2;

	/* Create symlink */
	err = ext2fs_symlink(fs, parent, 0, name, target);
	if (err == EXT2_ET_DIR_NO_SPACE) {
		err = ext2fs_expand_dir(fs, parent);
		if (err) {
			ret = translate_error(fs, parent, err);
			goto out2;
		}

		err = ext2fs_symlink(fs, parent, 0, name, target);
	}
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	/* Update parent dir's mtime */
	ret = fuse4fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	/* Still have to update the uid/gid of the symlink */
	err = ext2fs_namei(fs, EXT2_ROOT_INO, parent, name, &child);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out2;
	}
	dbg_printf(ff, "%s: symlinking dir=%d name='%s' child=%d\n",
		   __func__, parent, name, child);

	err = fuse4fs_read_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	fuse4fs_set_extra_isize(ff, child, &inode);
	fuse4fs_set_uid(&inode, ctxt->uid);
	fuse4fs_set_gid(&inode, gid);
	inode.i_generation = ff->next_generation++;
	fuse4fs_init_timestamps(ff, &inode);

	err = fuse4fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ret = fuse4fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse4fs_finish(ff, ret);
	fuse4fs_reply_entry(req, child, &inode, ret);
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

/*
 * If we're moving a directory, make sure that the new parent of that directory
 * can handle the nlink bump.
 */
static int fuse4fs_check_from_dir_nlink(struct fuse4fs *ff, ext2_ino_t from_ino,
					ext2_ino_t to_ino,
					ext2_ino_t from_dir_ino,
					ext2_ino_t to_dir_ino)
{
	struct ext2_inode_large inode;
	errcode_t err;

	err = fuse4fs_read_inode(ff->fs, from_ino, &inode);
	if (err)
		return translate_error(ff->fs, from_ino, err);

	if (!S_ISDIR(inode.i_mode))
		return 0;

	if (to_ino != 0)
		return 0;

	if (to_dir_ino == from_dir_ino)
		return 0;

	err = fuse4fs_read_inode(ff->fs, to_dir_ino, &inode);
	if (err)
		return translate_error(ff->fs, from_ino, err);

	if (ext2fs_dir_link_max(ff->fs, &inode))
		return -EMLINK;

	return 0;
}

static void op_rename(fuse_req_t req, fuse_ino_t from_parent, const char *from,
		      fuse_ino_t to_parent, const char *to, unsigned int flags)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t from_ino, to_ino, to_dir_ino, from_dir_ino;
	struct ext2_inode inode;
	struct update_dotdot ud;
	int flushed = 0;
	int ret = 0;

	/* renameat2 is not supported */
	if (flags) {
		fuse_reply_err(req, ENOSYS);
		return;
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &from_dir_ino, from_parent);
	FUSE4FS_CONVERT_FINO(req, &to_dir_ino, to_parent);
	dbg_printf(ff, "%s: renaming dir=%d name='%s' to dir=%d name='%s'\n",
		   __func__, from_dir_ino, from, to_dir_ino, to);
	fs = fuse4fs_start(ff);
	if (!fuse4fs_can_allocate(ff, 5)) {
		ret = -ENOSPC;
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, from_dir_ino, from, &from_ino);
	if (err || from_ino == 0) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	err = ext2fs_namei(fs, EXT2_ROOT_INO, to_dir_ino, to, &to_ino);
	if (err && err != EXT2_ET_FILE_NOT_FOUND) {
		ret = translate_error(fs, 0, err);
		goto out;
	}

	if (err == EXT2_ET_FILE_NOT_FOUND)
		to_ino = 0;

	dbg_printf(ff,
 "%s: renaming dir=%d name='%s' child=%d to dir=%d name='%s' child=%d\n",
		   __func__, from_dir_ino, from, from_ino, to_dir_ino, to,
		   to_ino);

	/* Already the same file? */
	if (to_ino != 0 && to_ino == from_ino) {
		ret = 0;
		goto out;
	}

	ret = fuse4fs_inum_access(ff, ctxt, from_ino, W_OK);
	if (ret)
		goto out;

	if (to_ino) {
		ret = fuse4fs_inum_access(ff, ctxt, to_ino, W_OK);
		if (ret)
			goto out;
	}

	ret = fuse4fs_inum_access(ff, ctxt, from_dir_ino, W_OK);
	if (ret)
		goto out;

	ret = fuse4fs_inum_access(ff, ctxt, to_dir_ino, W_OK);
	if (ret)
		goto out;

	ret = fuse4fs_check_from_dir_nlink(ff, from_ino, to_ino, from_dir_ino,
					   to_dir_ino);
	if (ret)
		goto out;

	/* If the target exists, unlink it first */
	if (to_ino != 0) {
		err = ext2fs_read_inode(fs, to_ino, &inode);
		if (err) {
			ret = translate_error(fs, to_ino, err);
			goto out;
		}

		dbg_printf(ff, "%s: unlink dir=%d name='%s' child=%d\n",
			   __func__, to_dir_ino, to, to_ino);
		if (LINUX_S_ISDIR(inode.i_mode))
			ret = fuse4fs_rmdir(ff, to_dir_ino, to, to_ino);
		else
			ret = fuse4fs_rmfile(ff, to_dir_ino, to, to_ino);
		if (ret)
			goto out;
	}

	/* Get ready to do the move */
	err = ext2fs_read_inode(fs, from_ino, &inode);
	if (err) {
		ret = translate_error(fs, from_ino, err);
		goto out;
	}

	/* Link in the new file */
	dbg_printf(ff, "%s: link dir=%d name='%s' child=%d\n",
		   __func__, to_dir_ino, to, from_ino);
	err = ext2fs_link(fs, to_dir_ino, to, from_ino,
			  ext2_file_type(inode.i_mode) | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, to_dir_ino, err);
		goto out;
	}

	/* Update '..' pointer if dir */
	if (LINUX_S_ISDIR(inode.i_mode)) {
		ud.new_dotdot = to_dir_ino;
		dbg_printf(ff, "%s: updating .. entry for child=%d parent=%d\n",
			   __func__, from_ino, to_dir_ino);
		err = ext2fs_dir_iterate2(fs, from_ino, 0, NULL,
					  update_dotdot_helper, &ud);
		if (err) {
			ret = translate_error(fs, from_ino, err);
			goto out;
		}

		/* Decrease from_dir_ino's links_count */
		dbg_printf(ff, "%s: moving linkcount from dir=%d to dir=%d\n",
			   __func__, from_dir_ino, to_dir_ino);
		err = ext2fs_read_inode(fs, from_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, from_dir_ino, err);
			goto out;
		}
		ext2fs_dec_nlink(&inode);
		err = ext2fs_write_inode(fs, from_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, from_dir_ino, err);
			goto out;
		}

		/* Increase to_dir_ino's links_count */
		err = ext2fs_read_inode(fs, to_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, to_dir_ino, err);
			goto out;
		}
		ext2fs_inc_nlink(fs, &inode);
		err = ext2fs_write_inode(fs, to_dir_ino, &inode);
		if (err) {
			ret = translate_error(fs, to_dir_ino, err);
			goto out;
		}
	}

	/* Update timestamps */
	ret = fuse4fs_update_ctime(ff, from_ino, NULL);
	if (ret)
		goto out;

	ret = fuse4fs_update_mtime(ff, to_dir_ino, NULL);
	if (ret)
		goto out;

	/* Remove the old file */
	dbg_printf(ff, "%s: unlink dir=%d name='%s' child=%d\n",
		   __func__, from_dir_ino, from, from_ino);
	ret = fuse4fs_unlink(ff, from_dir_ino, from, from_ino);
	if (ret)
		goto out;

	ret = fuse4fs_dirsync_flush(ff, from_dir_ino, &flushed);
	if (ret)
		goto out;

	if (from_dir_ino != to_dir_ino && !flushed) {
		ret = fuse4fs_dirsync_flush(ff, to_dir_ino, NULL);
		if (ret)
			goto out;
	}

out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}

static void op_link(fuse_req_t req, fuse_ino_t child_fino,
		    fuse_ino_t parent_fino, const char *name)
{
	struct ext2_inode_large inode;
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	errcode_t err;
	ext2_ino_t parent, child;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, parent_fino);
	FUSE4FS_CONVERT_FINO(req, &child, child_fino);
	dbg_printf(ff, "%s: link dir=%d name='%s' child=%d\n",
		   __func__, parent, name, child);

	fs = fuse4fs_start(ff);
	if (!fuse4fs_can_allocate(ff, 2)) {
		ret = -ENOSPC;
		goto out2;
	}

	ret = fuse4fs_inum_access(ff, ctxt, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse4fs_read_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ret = fuse4fs_iflags_access(ff, child, EXT2_INODE(&inode), W_OK);
	if (ret)
		goto out2;

	if (ext2fs_dir_link_max(ff->fs, &inode)) {
		ret = -EMLINK;
		goto out2;
	}

	/*
	 * Linking a file back into the filesystem requires removing it from
	 * the orphan list.
	 */
	if (inode.i_links_count == 0) {
		ret = fuse4fs_remove_from_orphans(ff, child, &inode);
		if (ret)
			goto out2;
	}

	ext2fs_inc_nlink(fs, EXT2_INODE(&inode));
	ret = fuse4fs_update_ctime(ff, child, &inode);
	if (ret)
		goto out2;

	err = fuse4fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	err = ext2fs_link(fs, parent, name, child,
			  ext2_file_type(inode.i_mode) | EXT2FS_LINK_EXPAND);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	ret = fuse4fs_update_mtime(ff, parent, NULL);
	if (ret)
		goto out2;

	ret = fuse4fs_dirsync_flush(ff, parent, NULL);
	if (ret)
		goto out2;

out2:
	fuse4fs_finish(ff, ret);
	fuse4fs_reply_entry(req, child, &inode, ret);
}

/* Obtain group ids of the process that sent us a command(?) */
static int fuse4fs_get_groups(struct fuse4fs *ff, fuse_req_t req, gid_t **gids,
			      size_t *nr_gids)
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

		ret = fuse_req_getgroups(req, nr, array);
		if (ret < 0) {
			/*
			 * If there's an error, we failed to find the group
			 * membership of the process that initiated the file
			 * change, either because the process went away or
			 * because there's no Linux procfs.  Regardless of the
			 * cause, we return -ENOENT.
			 */
			ext2fs_free_mem(&array);
			return -ENOENT;
		}

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
static int fuse4fs_in_file_group(struct fuse4fs *ff, fuse_req_t req,
				 const struct ext2_inode_large *inode)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	gid_t *gids = NULL;
	size_t i, nr_gids = 0;
	gid_t gid = inode_gid(*inode);
	int ret;

	/* If the inode gid matches the process' primary group, we're done. */
	if (ctxt->gid == gid)
		return 1;

	ret = fuse4fs_get_groups(ff, req, &gids, &nr_gids);
	if (ret == -ENOENT) {
		/* magic return code for "could not get caller group info" */
		return 0;
	}
	if (ret < 0)
		return ret;

	ret = 0;
	for (i = 0; i < nr_gids; i++) {
		if (gids[i] == gid) {
			ret = 1;
			break;
		}
	}

	ext2fs_free_mem(&gids);
	return ret;
}

static int fuse4fs_chmod(struct fuse4fs *ff, fuse_req_t req, ext2_ino_t ino,
			 mode_t mode, struct ext2_inode_large *inode)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	mode_t new_mode;
	int ret = 0;

	dbg_printf(ff, "%s: ino=%d mode=0%o\n", __func__, ino, mode);

	ret = fuse4fs_iflags_access(ff, ino, EXT2_INODE(inode), W_OK);
	if (ret)
		return ret;

	if (fuse4fs_want_check_owner(ff, ctxt) &&
	    ctxt->uid != inode_uid(*inode))
		return -EPERM;

	/*
	 * XXX: We should really check that the inode gid is not in /any/
	 * of the user's groups, but FUSE only tells us about the primary
	 * group.
	 */
	if (!fuse4fs_iomap_enabled(ff) && !fuse4fs_is_superuser(ff, ctxt)) {
		ret = fuse4fs_in_file_group(ff, req, inode);
		if (ret < 0)
			return ret;

		if (!ret)
			mode &= ~S_ISGID;
	}

	new_mode = (inode->i_mode & ~0xFFF) | (mode & 0xFFF);

	dbg_printf(ff, "%s: ino=%d old_mode=0%o new_mode=0%o\n",
		   __func__, ino, inode->i_mode, new_mode);

	inode->i_mode = new_mode;

	return 0;
}

static int fuse4fs_chown(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			 ext2_ino_t ino, const int to_set,
			 const struct stat *attr,
			 struct ext2_inode_large *inode)
{
	uid_t owner = (to_set & FUSE_SET_ATTR_UID) ? attr->st_uid : (uid_t)~0;
	gid_t group = (to_set & FUSE_SET_ATTR_GID) ? attr->st_gid : (gid_t)~0;
	int ret = 0;

	dbg_printf(ff, "%s: ino=%d owner=%d group=%d\n",
		   __func__, ino, owner, group);

	ret = fuse4fs_iflags_access(ff, ino, EXT2_INODE(inode), W_OK);
	if (ret)
		return ret;

	/* FUSE seems to feed us ~0 to mean "don't change" */
	if (owner != (uid_t) ~0) {
		/* Only root gets to change UID. */
		if (fuse4fs_want_check_owner(ff, ctxt) &&
		    !(inode_uid(*inode) == ctxt->uid && owner == ctxt->uid))
			return -EPERM;

		fuse4fs_set_uid(inode, owner);
	}

	if (group != (gid_t) ~0) {
		/* Only root or the owner get to change GID. */
		if (fuse4fs_want_check_owner(ff, ctxt) &&
		    inode_uid(*inode) != ctxt->uid)
			return -EPERM;

		/* XXX: We /should/ check group membership but FUSE */
		fuse4fs_set_gid(inode, group);
	}

	return 0;
}

static int fuse4fs_punch_posteof(struct fuse4fs *ff, ext2_ino_t ino,
				 off_t new_size)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	blk64_t truncate_block = FUSE4FS_B_TO_FSB(ff, new_size);
	errcode_t err;

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), 0, truncate_block,
			   ~0ULL);
	if (err)
		return translate_error(fs, ino, err);

	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static int fuse4fs_truncate(struct fuse4fs *ff, ext2_ino_t ino, off_t new_size)
{
	ext2_filsys fs = ff->fs;
	ext2_file_t file;
	__u64 old_isize;
	errcode_t err;
	int flags = EXT2_FILE_WRITE;
	int ret = 0;

	/* the kernel handles all eof zeroing for us in iomap mode */
	if (fuse4fs_iomap_enabled(ff))
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

	ret = fuse4fs_update_mtime(ff, ino, NULL);
	if (ret)
		return ret;

	/*
	 * Truncating to the current size is usually understood to mean that
	 * we should clear out post-EOF preallocations.
	 */
	if (new_size == old_isize)
		return fuse4fs_punch_posteof(ff, ino, new_size);

	return 0;
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

static int fuse4fs_open_file(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			     ext2_ino_t ino, bool linked,
			     struct fuse_file_info *fp)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct fuse4fs_file_handle *file;
	int check = 0, ret = 0;

	dbg_printf(ff, "%s: ino=%d oflags=0o%o\n", __func__, ino, fp->flags);
	err = ext2fs_get_mem(sizeof(*file), &file);
	if (err)
		return translate_error(fs, 0, err);
	file->magic = FUSE4FS_FILE_MAGIC;
	file->ino = ino;

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

	if (linked)
		check |= L_OK;

	/* the kernel handles all block IO for us in iomap mode */
	if (fuse4fs_iomap_enabled(ff))
		file->open_flags |= EXT2_FILE_NOBLOCKIO;

	/*
	 * If the caller wants to truncate the file, we need to ask for full
	 * write access even if the caller claims to be appending.
	 */
	if ((fp->flags & O_APPEND) && !(fp->flags & O_TRUNC))
		check |= A_OK;

	detect_linux_executable_open(fp->flags, &check, &file->open_flags);

	if (fp->flags & O_CREAT)
		file->open_flags |= EXT2_FILE_CREATE;

	ret = fuse4fs_inum_access(ff, ctxt, file->ino, check);
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
			ret = fuse4fs_inum_access(ff, ctxt, file->ino, X_OK);
			if (ret)
				goto out;
			check = X_OK;
		} else
			goto out;
	}

	if (fp->flags & O_TRUNC) {
		ret = fuse4fs_truncate(ff, file->ino, 0);
		if (ret)
			goto out;
	}

	err = fuse4fs_iget(ff, file->ino, &file->fi);
	if (err) {
		ret = translate_error(fs, 0, err);
		goto out;
	}
	file->fi->i_open_count++;

	file->check_flags = check;
	fuse4fs_set_handle(fp, file);
	dbg_printf(ff, "%s: ino=%d fh=%p opencount=%d\n", __func__, ino, file,
		   file->fi->i_open_count);

out:
	if (ret)
		ext2fs_free_mem(&file);
	return ret;
}

static void op_open(fuse_req_t req, fuse_ino_t fino, struct fuse_file_info *fp)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_ino_t ino;
	int ret;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	dbg_printf(ff, "%s: ino=%d\n", __func__, ino);

	fuse4fs_start(ff);
	ret = fuse4fs_open_file(ff, ctxt, ino, true, fp);
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_open(req, fp);
}

static void op_read(fuse_req_t req, fuse_ino_t fino EXT2FS_ATTR((unused)),
		    size_t len, off_t offset, struct fuse_file_info *fp)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	char *buf;
	ext2_filsys fs;
	ext2_file_t efp;
	errcode_t err;
	unsigned int got = 0;
	int ret = 0;

	buf = calloc(len, sizeof(char));
	if (!buf) {
		fuse_reply_err(req, errno);
		return;
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	dbg_printf(ff, "%s: ino=%d off=0x%llx len=0x%zx\n", __func__, fh->ino,
		   (unsigned long long)offset, len);

	fs = fuse4fs_start(ff);
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

	if (fh->check_flags != X_OK && fuse4fs_is_writeable(ff)) {
		ret = fuse4fs_update_atime(ff, fh->ino);
		if (ret)
			goto out;
	}
out:
	fuse4fs_finish(ff, ret);
	if (got)
		fuse_reply_buf(req, buf, got);
	else
		fuse_reply_err(req, -ret);
	ext2fs_free_mem(&buf);
}

static void op_write(fuse_req_t req, fuse_ino_t fino EXT2FS_ATTR((unused)),
		     const char *buf, size_t len, off_t offset,
		     struct fuse_file_info *fp)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	ext2_filsys fs;
	ext2_file_t efp;
	errcode_t err;
	unsigned int got = 0;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	dbg_printf(ff, "%s: ino=%d off=0x%llx len=0x%zx\n", __func__, fh->ino,
		   (unsigned long long) offset, len);
	fs = fuse4fs_start(ff);
	if (!fuse4fs_is_writeable(ff)) {
		ret = -EROFS;
		goto out;
	}

	if (!fuse4fs_can_allocate(ff, FUSE4FS_B_TO_FSB(ff, len))) {
		ret = -ENOSPC;
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

	ret = fuse4fs_update_mtime(ff, fh->ino, NULL);
	if (ret)
		goto out;

out:
	fuse4fs_finish(ff, ret);
	if (got)
		fuse_reply_write(req, got);
	else
		fuse_reply_err(req, -ret);
}

static int fuse4fs_free_unlinked(struct fuse4fs *ff, ext2_ino_t ino)
{
	struct ext2_inode_large inode;
	ext2_filsys fs = ff->fs;
	errcode_t err;
	int ret = 0;

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	if (inode.i_links_count > 0)
		return 0;

	dbg_printf(ff, "%s: ino=%d links=%d\n", __func__, ino,
		   inode.i_links_count);

	if (ext2fs_has_feature_ea_inode(fs->super)) {
		ret = fuse4fs_remove_ea_inodes(ff, ino, &inode);
		if (ret)
			return ret;
	}

	/* Nobody holds this file; free its blocks! */
	err = ext2fs_free_ext_attr(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	if (ext2fs_inode_has_valid_blocks2(fs, EXT2_INODE(&inode))) {
		err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), NULL,
				   0, ~0ULL);
		if (err)
			return translate_error(fs, ino, err);
	}

	ret = fuse4fs_remove_from_orphans(ff, ino, &inode);
	if (ret)
		return ret;

	ext2fs_set_dtime(fs, EXT2_INODE(&inode));
	ext2fs_inode_alloc_stats2(fs, ino, -1, LINUX_S_ISDIR(inode.i_mode));

	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static void op_release(fuse_req_t req, fuse_ino_t fino EXT2FS_ATTR((unused)),
		       struct fuse_file_info *fp)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	ext2_filsys fs;
	errcode_t err;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	dbg_printf(ff, "%s: ino=%d fh=%p opencount=%u\n",
		   __func__, fh->ino, fh, fh->fi->i_open_count);

	fs = fuse4fs_start(ff);

	/*
	 * If the file is no longer open and is unlinked, free it, which
	 * removes it from the ondisk list.
	 */
	if (--fh->fi->i_open_count == 0) {
		ret = fuse4fs_free_unlinked(ff, fh->ino);
		if (ret)
			goto out_iput;
	}

	if ((fp->flags & O_SYNC) &&
	    fuse4fs_is_writeable(ff) &&
	    (fh->open_flags & EXT2_FILE_WRITE)) {
		err = ext2fs_flush2(fs, EXT2_FLAG_FLUSH_NO_SYNC);
		if (err)
			ret = translate_error(fs, fh->ino, err);
	}

out_iput:
	fuse4fs_iput(ff, fh->fi);
	fp->fh = 0;
	fuse4fs_finish(ff, ret);

	ext2fs_free_mem(&fh);

	fuse_reply_err(req, -ret);
}

static void op_fsync(fuse_req_t req, fuse_ino_t fino EXT2FS_ATTR((unused)),
		     int datasync EXT2FS_ATTR((unused)),
		     struct fuse_file_info *fp)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	ext2_filsys fs;
	errcode_t err;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	fs = fuse4fs_start(ff);
	/* For now, flush everything, even if it's slow */
	if (fuse4fs_is_writeable(ff) && fh->open_flags & EXT2_FILE_WRITE) {
		err = ext2fs_flush2(fs, 0);
		if (err)
			ret = translate_error(fs, fh->ino, err);
	}
	fuse4fs_finish(ff, ret);

	fuse_reply_err(req, -ret);
}

static void op_statfs(fuse_req_t req, fuse_ino_t fino)
{
	struct statvfs buf;
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	uint64_t fsid, *f;
	ext2_ino_t ino;
	blk64_t overhead, reserved, free;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	dbg_printf(ff, "%s: ino=%d\n", __func__, ino);
	fs = fuse4fs_start(ff);
	buf.f_bsize = fs->blocksize;
	buf.f_frsize = 0;

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

	buf.f_blocks = ext2fs_blocks_count(fs->super) - overhead;
	buf.f_bfree = free;
	if (free < reserved)
		buf.f_bavail = 0;
	else
		buf.f_bavail = free - reserved;
	buf.f_files = fs->super->s_inodes_count;
	buf.f_ffree = fs->super->s_free_inodes_count;
	buf.f_favail = fs->super->s_free_inodes_count;
	f = (uint64_t *)fs->super->s_uuid;
	fsid = *f;
	f++;
	fsid ^= *f;
	buf.f_fsid = fsid;
	buf.f_flag = 0;
	if (ff->opstate != F4OP_WRITABLE)
		buf.f_flag |= ST_RDONLY;
	buf.f_namemax = EXT2_NAME_LEN;
	fuse4fs_finish(ff, 0);

	fuse_reply_statfs(req, &buf);
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

static void op_getxattr(fuse_req_t req, fuse_ino_t fino, const char *key,
			size_t len)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	void *ptr = NULL;
	size_t plen;
	ext2_ino_t ino;
	int ret = 0;

	if (!validate_xattr_name(key)) {
		fuse_reply_err(req, ENODATA);
		return;
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fs = fuse4fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	dbg_printf(ff, "%s: ino=%d name='%s'\n", __func__, ino, key);

	ret = fuse4fs_inum_access(ff, ctxt, ino, R_OK);
	if (ret)
		goto out;

	ret = fuse4fs_getxattr(ff, ino, key, &ptr, &plen);
	if (ret)
		goto out;

	if (!len) {
		/* Just tell us the length */
		ret = plen;
	} else if (len < plen) {
		/* Caller's buffer wasn't big enough */
		ret = -ERANGE;
	} else {
		/* We have data */
		ret = plen;
	}

out:
	fuse4fs_finish(ff, ret);

	if (ret < 0)
		fuse_reply_err(req, -ret);
	else if (!len)
		fuse_reply_xattr(req, ret);
	else
		fuse_reply_buf(req, ptr, ret);
	ext2fs_free_mem(&ptr);
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

static void op_listxattr(fuse_req_t req, fuse_ino_t fino, size_t len)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	struct ext2_xattr_handle *h;
	char *names = NULL;
	char *next_name;
	unsigned int bufsz;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fs = fuse4fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	dbg_printf(ff, "%s: ino=%d\n", __func__, ino);

	ret = fuse4fs_inum_access(ff, ctxt, ino, R_OK);
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
		/* Just tell us the length */
		goto out2;
	} else if (len < bufsz) {
		/* Caller's buffer wasn't big enough */
		ret = -ERANGE;
		goto out2;
	}

	/* Copy names out */
	names = calloc(len, sizeof(char));
	if (!names) {
		ret = translate_error(fs, ino, errno);
		goto out2;
	}
	next_name = names;

	err = ext2fs_xattrs_iterate(h, copy_names, &next_name);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out2;
	}

out2:
	err = ext2fs_xattrs_close(&h);
	if (err && !ret)
		ret = translate_error(fs, ino, err);
out:
	fuse4fs_finish(ff, ret);

	if (ret < 0)
		fuse_reply_err(req, -ret);
	else if (names)
		fuse_reply_buf(req, names, bufsz);
	else
		fuse_reply_xattr(req, bufsz);
	free(names);
}

static void op_setxattr(fuse_req_t req, fuse_ino_t fino, const char *key,
			const char *value, size_t len, int flags)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	struct ext2_xattr_handle *h;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) {
		fuse_reply_err(req, EOPNOTSUPP);
		return;
	}

	if (!validate_xattr_name(key)) {
		fuse_reply_err(req, EINVAL);
		return;
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fs = fuse4fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	dbg_printf(ff, "%s: ino=%d name='%s'\n", __func__, ino, key);

	ret = fuse4fs_inum_access(ff, ctxt, ino, W_OK);
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

	ret = fuse4fs_update_ctime(ff, ino, NULL);
out2:
	err = ext2fs_xattrs_close(&h);
	if (!ret && err)
		ret = translate_error(fs, ino, err);
out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}

static void op_removexattr(fuse_req_t req, fuse_ino_t fino, const char *key)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
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
	if (key[0] == 0) {
		fuse_reply_err(req, 0);
		return;
	}

	if (!validate_xattr_name(key)) {
		fuse_reply_err(req, ENODATA);
		return;
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fs = fuse4fs_start(ff);
	if (!ext2fs_has_feature_xattr(fs->super)) {
		ret = -ENOTSUP;
		goto out;
	}

	if (!fuse4fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out;
	}

	dbg_printf(ff, "%s: ino=%d name=%s\n", __func__, ino, key);

	ret = fuse4fs_inum_access(ff, ctxt, ino, W_OK);
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

	ret = fuse4fs_update_ctime(ff, ino, NULL);
out2:
	err = ext2fs_xattrs_close(&h);
	if (err && !ret)
		ret = translate_error(fs, ino, err);
out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}

struct readdir_iter {
	void *buf;
	size_t bufsz;
	size_t bufused;

	ext2_filsys fs;
	struct fuse4fs *ff;
	fuse_req_t req;

	bool readdirplus;
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
	struct fuse4fs_stat fstat = {
		.entry = {
			.attr = {
				.st_ino = dirent->inode,
				.st_mode = dirent_fmode(i->fs, dirent),
			},
		},
	};
	size_t entrysize;
	int ret;

	i->dirpos++;
	if (i->startpos >= i->dirpos)
		return 0;

	dbg_printf(i->ff, "READDIR%s ino=%d %u offset=0x%llx\n",
			i->readdirplus ? "PLUS" : "",
			dir,
			i->nr++,
			(unsigned long long)i->dirpos);

	if (i->readdirplus) {
		ret = fuse4fs_stat_inode(i->ff, dirent->inode, NULL, &fstat);
		if (ret)
			return DIRENT_ABORT;
	}

	memcpy(namebuf, dirent->name, dirent->name_len & 0xFF);
	namebuf[dirent->name_len & 0xFF] = 0;

	if (i->readdirplus) {
		entrysize = fuse_add_direntry_plus_iflags(i->req,
							  i->buf + i->bufused,
							  i->bufsz - i->bufused,
							  namebuf,
							  fstat.iflags,
							  &fstat.entry,
							  i->dirpos);
	} else {
		entrysize = fuse_add_direntry(i->req, i->buf + i->bufused,
					      i->bufsz - i->bufused, namebuf,
					      &fstat.entry.attr, i->dirpos);
	}
	if (entrysize > i->bufsz - i->bufused) {
		/* Buffer is full */
		return DIRENT_ABORT;
	}

	i->bufused += entrysize;
	return 0;
}

static void __op_readdir(fuse_req_t req, fuse_ino_t fino, size_t size,
			 off_t offset, bool plus, struct fuse_file_info *fp)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	errcode_t err;
	struct readdir_iter i = {
		.ff = ff,
		.req = req,
		.dirpos = 0,
		.startpos = offset,
		.readdirplus = plus,
		.bufsz = size,
	};
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	dbg_printf(ff, "%s: ino=%d offset=0x%llx\n", __func__, fh->ino,
			(unsigned long long)offset);

	err = ext2fs_get_mem(size, &i.buf);
	if (err) {
		ret = translate_error(i.fs, fh->ino, err);
		goto out;
	}

	i.fs = fuse4fs_start(ff);
	err = ext2fs_dir_iterate2(i.fs, fh->ino, 0, NULL, op_readdir_iter, &i);
	if (err) {
		ret = translate_error(i.fs, fh->ino, err);
		goto out;
	}

	if (fuse4fs_is_writeable(ff)) {
		ret = fuse4fs_update_atime(i.ff, fh->ino);
		if (ret)
			goto out;
	}
out:
	fuse4fs_finish(ff, ret);
	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_buf(req, i.buf, i.bufused);

	ext2fs_free_mem(&i.buf);
}

static void op_readdir(fuse_req_t req, fuse_ino_t fino, size_t size,
		       off_t offset, struct fuse_file_info *fp)
{
	__op_readdir(req, fino, size, offset, false, fp);
}

static void op_readdirplus(fuse_req_t req, fuse_ino_t fino, size_t size,
			   off_t offset, struct fuse_file_info *fp)
{
	__op_readdir(req, fino, size, offset, true, fp);
}

static void op_access(fuse_req_t req, fuse_ino_t fino, int mask)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_ino_t ino;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	dbg_printf(ff, "%s: ino=%d mask=0x%x\n",
		   __func__, ino, mask);
	fuse4fs_start(ff);

	ret = fuse4fs_inum_access(ff, ctxt, ino, mask);
	if (ret)
		goto out;

out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}

static void op_create(fuse_req_t req, fuse_ino_t fino, const char *name,
		      mode_t mode, struct fuse_file_info *fp)
{
	struct ext2_inode_large inode;
	struct fuse4fs_stat fstat;
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t parent, child;
	errcode_t err;
	int filetype;
	gid_t gid;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &parent, fino);
	dbg_printf(ff, "%s: parent=%d name='%s' mode=0%o\n",
		   __func__, parent, name, mode);

	fs = fuse4fs_start(ff);
	if (!fuse4fs_can_allocate(ff, 1)) {
		ret = -ENOSPC;
		goto out2;
	}

	ret = fuse4fs_inum_access(ff, ctxt, parent, A_OK | W_OK);
	if (ret)
		goto out2;

	err = fuse4fs_new_child_gid(ff, ctxt, parent, &gid, NULL);
	if (err)
		goto out2;

	filetype = ext2_file_type(mode);

	err = ext2fs_new_inode(fs, parent, mode, 0, &child);
	if (err) {
		ret = translate_error(fs, parent, err);
		goto out2;
	}

	if (name) {
		dbg_printf(ff, "%s: creating dir=%d name='%s' child=%d\n",
			   __func__, parent, name, child);

		err = ext2fs_link(fs, parent, name, child,
				  filetype | EXT2FS_LINK_EXPAND);
		if (err) {
			ret = translate_error(fs, parent, err);
			goto out2;
		}

		ret = fuse4fs_update_mtime(ff, parent, NULL);
		if (ret)
			goto out2;
	} else {
		dbg_printf(ff, "%s: creating dir=%d tempfile=%d\n",
			   __func__, parent, child);
	}

	memset(&inode, 0, sizeof(inode));
	inode.i_mode = mode;
	inode.i_links_count = name ? 1 : 0;
	fuse4fs_set_extra_isize(ff, child, &inode);
	fuse4fs_set_uid(&inode, ctxt->uid);
	fuse4fs_set_gid(&inode, gid);
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

	if (!name) {
		ret = fuse4fs_add_to_orphans(ff, child, &inode);
		if (ret)
			goto out2;
	}

	err = ext2fs_write_new_inode(fs, child, EXT2_INODE(&inode));
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	inode.i_generation = ff->next_generation++;
	fuse4fs_init_timestamps(ff, &inode);
	err = fuse4fs_write_inode(fs, child, &inode);
	if (err) {
		ret = translate_error(fs, child, err);
		goto out2;
	}

	ext2fs_inode_alloc_stats2(fs, child, 1, 0);

	ret = fuse4fs_propagate_default_acls(ff, parent, child, inode.i_mode);
	if (ret)
		goto out2;

	fp->flags &= ~O_TRUNC;
	ret = fuse4fs_open_file(ff, ctxt, child, name != NULL, fp);
	if (ret)
		goto out2;

	if (name) {
		ret = fuse4fs_dirsync_flush(ff, parent, NULL);
		if (ret)
			goto out2;
	}

	ret = fuse4fs_stat_inode(ff, child, NULL, &fstat);
	if (ret)
		goto out2;

out2:
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_create_iflags(req, &fstat.entry, fstat.iflags, fp);
}

#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 17)
static void op_tmpfile(fuse_req_t req, fuse_ino_t fino, mode_t mode,
		       struct fuse_file_info *fp)
{
	op_create(req, fino, NULL, mode, fp);
}
#endif

enum fuse4fs_time_action {
	TA_NOW,		/* set to current time */
	TA_OMIT,	/* do not set timestamp */
	TA_THIS,	/* set to specific timestamp */
};

static inline const char *
fuse4fs_time_action_string(enum fuse4fs_time_action act)
{
	switch (act) {
	case TA_NOW:
		return "now";
	case TA_OMIT:
		return "omit";
	case TA_THIS:
		return "specific";
	}
	return NULL; /* shut up gcc */
}

static int fuse4fs_utimens(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			   ext2_ino_t ino, const int to_set,
			   const struct stat *attr,
			   struct ext2_inode_large *inode)
{
	enum fuse4fs_time_action aact = TA_OMIT;
	enum fuse4fs_time_action mact = TA_OMIT;
	struct timespec atime = { };
	struct timespec mtime = { };
	struct timespec now = { };
	int access = W_OK;
	int ret = 0;

	if (to_set & (FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW))
		fuse4fs_get_now(ff, &now);

	if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
		atime = now;
		aact = TA_NOW;
	} else if (to_set & FUSE_SET_ATTR_ATIME) {
#if HAVE_STRUCT_STAT_ST_ATIM
		atime = attr->st_atim;
#else
		atime.tv_sec = attr->st_atime;
#endif
		aact = TA_THIS;
	}

	if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
		mtime = now;
		mact = TA_NOW;
	} else if (to_set & FUSE_SET_ATTR_MTIME) {
#if HAVE_STRUCT_STAT_ST_ATIM
		mtime = attr->st_mtim;
#else
		mtime.tv_sec = attr->st_mtime;
#endif
		mact = TA_THIS;
	}

	dbg_printf(ff, "%s: ino=%d atime=%s:%lld.%ld mtime=%s:%lld.%ld\n",
		   __func__, ino, fuse4fs_time_action_string(aact),
		   (long long int)atime.tv_sec, atime.tv_nsec,
		   fuse4fs_time_action_string(mact),
		   (long long int)mtime.tv_sec, mtime.tv_nsec);

	/*
	 * ext4 allows timestamp updates of append-only files but only if we're
	 * setting to current time.  If iomap is enabled, the kernel does the
	 * permission checking for timestamp updates; skip the access check.
	 */
	if (aact == TA_NOW && mact == TA_NOW)
		access |= A_OK;
	if (!fuse4fs_iomap_enabled(ff)) {
		ret = fuse4fs_inum_access(ff, ctxt, ino, access);
		if (ret)
			return ret;
	}

	if (aact != TA_OMIT)
		EXT4_INODE_SET_XTIME(i_atime, &atime, inode);
	if (mact != TA_OMIT)
		EXT4_INODE_SET_XTIME(i_mtime, &mtime, inode);

	return 0;
}

static int fuse4fs_setsize(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			   ext2_ino_t ino, off_t new_size,
			   struct ext2_inode_large *inode)
{
	errcode_t err;
	int ret;

	/* Write inode because truncate makes its own copy */
	err = fuse4fs_write_inode(ff->fs, ino, inode);
	if (err)
		return translate_error(ff->fs, ino, err);

	ret = fuse4fs_inum_access(ff, ctxt, ino, W_OK);
	if (ret)
		return ret;

	ret = fuse4fs_truncate(ff, ino, new_size);
	if (ret)
		return ret;

	/* Re-read inode after truncate */
	err = fuse4fs_read_inode(ff->fs, ino, inode);
	if (err)
		return translate_error(ff->fs, ino, err);

	return 0;
}

static void op_setattr(fuse_req_t req, fuse_ino_t fino, struct stat *attr,
		       int to_set, struct fuse_file_info *fi EXT2FS_ATTR((unused)))
{
	struct ext2_inode_large inode;
	struct fuse4fs_stat fstat;
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t ino;
	errcode_t err;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	dbg_printf(ff, "%s: ino=%d to_set=0x%x\n", __func__, ino, to_set);
	fs = fuse4fs_start(ff);

	if (!fuse4fs_is_writeable(ff)) {
		ret = -EROFS;
		goto out;
	}

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	/* Handle mode change using helper */
	if (to_set & FUSE_SET_ATTR_MODE) {
		ret = fuse4fs_chmod(ff, req, ino, attr->st_mode, &inode);
		if (ret)
			goto out;
	}

	/* Handle owner/group change using helper */
	if (to_set & (FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID)) {
		ret = fuse4fs_chown(ff, ctxt, ino, to_set, attr, &inode);
		if (ret)
			goto out;
	}

	/* Handle size change using helper */
	if (to_set & FUSE_SET_ATTR_SIZE) {
		ret = fuse4fs_setsize(ff, ctxt, ino, attr->st_size, &inode);
		if (ret)
			goto out;
	}

	/* Handle time changes using helper */
	if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
		ret = fuse4fs_utimens(ff, ctxt, ino, to_set, attr, &inode);
		if (ret)
			goto out;
	}

	/* Update ctime for any attribute change */
	ret = fuse4fs_update_ctime(ff, ino, &inode);
	if (ret)
		goto out;

	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

	/* Get updated stat info to return */
	ret = fuse4fs_stat_inode(ff, ino, &inode, &fstat);

out:
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_attr_iflags(req, &fstat.entry.attr, fstat.iflags,
				       fstat.entry.attr_timeout);
}

#define FUSE4FS_MODIFIABLE_IFLAGS \
	(EXT2_FL_USER_MODIFIABLE & ~(EXT4_EXTENTS_FL | EXT4_CASEFOLD_FL | \
				     EXT3_JOURNAL_DATA_FL))

static inline int set_iflags(struct ext2_inode_large *inode, __u32 iflags)
{
	if ((inode->i_flags ^ iflags) & ~FUSE4FS_MODIFIABLE_IFLAGS)
		return -EINVAL;

	inode->i_flags = (inode->i_flags & ~FUSE4FS_MODIFIABLE_IFLAGS) |
			 (iflags & FUSE4FS_MODIFIABLE_IFLAGS);
	return 0;
}

#ifdef SUPPORT_I_FLAGS
static int ioctl_getflags(struct fuse4fs *ff, struct fuse4fs_file_handle *fh,
			  __u32 *outdata, size_t *outsize)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;

	if (*outsize < sizeof(__u32))
		return -EFAULT;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	*outdata = inode.i_flags & EXT2_FL_USER_VISIBLE;
	*outsize = sizeof(__u32);
	return 0;
}

static int ioctl_setflags(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			  struct fuse4fs_file_handle *fh, const __u32 *indata,
			  size_t insize)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret;

	if (insize < sizeof(__u32))
		return -EFAULT;

	dbg_printf(ff, "%s: ino=%d iflags=0x%x\n", __func__, fh->ino, *indata);
	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	if (fuse4fs_want_check_owner(ff, ctxt) && inode_uid(inode) != ctxt->uid)
		return -EPERM;

	ret = set_iflags(&inode, *indata);
	if (ret)
		return ret;

	ret = fuse4fs_update_ctime(ff, fh->ino, &inode);
	if (ret)
		return ret;

	err = fuse4fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}

static int ioctl_getversion(struct fuse4fs *ff, struct fuse4fs_file_handle *fh,
			    __u32 *outdata, size_t *outsize)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;

	if (*outsize < sizeof(__u32))
		return -EFAULT;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	*outdata = inode.i_generation;
	*outsize = sizeof(__u32);
	return 0;
}

static int ioctl_setversion(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			    struct fuse4fs_file_handle *fh, const __u32 *indata,
			    size_t insize)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret;

	if (insize < sizeof(__u32))
		return -EFAULT;

	dbg_printf(ff, "%s: ino=%d generation=%d\n", __func__, fh->ino, *indata);
	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	if (fuse4fs_want_check_owner(ff, ctxt) && inode_uid(inode) != ctxt->uid)
		return -EPERM;

	inode.i_generation = *indata;

	ret = fuse4fs_update_ctime(ff, fh->ino, &inode);
	if (ret)
		return ret;

	err = fuse4fs_write_inode(fs, fh->ino, &inode);
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

static int ioctl_fsgetxattr(struct fuse4fs *ff, struct fuse4fs_file_handle *fh,
			    struct fsxattr *fsx, size_t *outsize)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	unsigned int inode_size;

	if (*outsize < sizeof(struct fsxattr))
		return -EFAULT;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	memset(fsx, 0, sizeof(*fsx));
	inode_size = EXT2_GOOD_OLD_INODE_SIZE + inode.i_extra_isize;
	if (ext2fs_inode_includes(inode_size, i_projid))
		fsx->fsx_projid = inode_projid(inode);
	fsx->fsx_xflags = iflags_to_fsxflags(inode.i_flags);
	*outsize = sizeof(struct fsxattr);
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

#define FUSE4FS_MODIFIABLE_XFLAGS (FS_XFLAG_IMMUTABLE | \
				   FS_XFLAG_APPEND | \
				   FS_XFLAG_SYNC | \
				   FS_XFLAG_NOATIME | \
				   FS_XFLAG_NODUMP | \
				   FS_XFLAG_PROJINHERIT)

#define FUSE4FS_MODIFIABLE_IXFLAGS (FS_IMMUTABLE_FL | \
				    FS_APPEND_FL | \
				    FS_SYNC_FL | \
				    FS_NOATIME_FL | \
				    FS_NODUMP_FL | \
				    FS_PROJINHERIT_FL)

static inline int set_xflags(struct ext2_inode_large *inode, __u32 xflags)
{
	__u32 iflags;

	if (xflags & ~FUSE4FS_MODIFIABLE_XFLAGS)
		return -EINVAL;

	iflags = fsxflags_to_iflags(xflags);
	inode->i_flags = (inode->i_flags & ~FUSE4FS_MODIFIABLE_IXFLAGS) |
			 (iflags & FUSE4FS_MODIFIABLE_IXFLAGS);
	return 0;
}

static int ioctl_fssetxattr(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			    struct fuse4fs_file_handle *fh,
			    const struct fsxattr *fsx, size_t insize)
{
	ext2_filsys fs = ff->fs;
	errcode_t err;
	struct ext2_inode_large inode;
	int ret;
	unsigned int inode_size;

	if (insize < sizeof(struct fsxattr))
		return -EFAULT;

	dbg_printf(ff, "%s: ino=%d\n", __func__, fh->ino);
	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	if (fuse4fs_want_check_owner(ff, ctxt) && inode_uid(inode) != ctxt->uid)
		return -EPERM;

	ret = set_xflags(&inode, fsx->fsx_xflags);
	if (ret)
		return ret;

	inode_size = EXT2_GOOD_OLD_INODE_SIZE + inode.i_extra_isize;
	if (ext2fs_inode_includes(inode_size, i_projid))
		inode.i_projid = fsx->fsx_projid;

	ret = fuse4fs_update_ctime(ff, fh->ino, &inode);
	if (ret)
		return ret;

	err = fuse4fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}
#endif /* FS_IOC_FSGETXATTR */

#ifdef FITRIM
static int ioctl_fitrim(struct fuse4fs *ff, struct fuse4fs_file_handle *fh,
			const struct fstrim_range *fr_in, size_t insize,
			struct fstrim_range *fr, size_t *outsize)
{
	ext2_filsys fs = ff->fs;
	blk64_t start, end, max_blocks, b, cleared, minlen;
	blk64_t max_blks = ext2fs_blocks_count(fs->super);
	errcode_t err = 0;

	if (insize < sizeof(struct fstrim_range))
		return -EFAULT;

	if (*outsize < sizeof(struct fstrim_range))
		return -EFAULT;

	if (!fuse4fs_is_writeable(ff))
		return -EROFS;

	memcpy(fr, fr_in, sizeof(*fr));
	start = FUSE4FS_B_TO_FSBT(ff, fr->start);
	if (fr->len == -1ULL)
		end = -1ULL;
	else
		end = FUSE4FS_B_TO_FSBT(ff, fr->start + fr->len - 1);
	minlen = FUSE4FS_B_TO_FSBT(ff, fr->minlen);

	if (EXT2FS_NUM_B2C(fs, minlen) > EXT2_CLUSTERS_PER_GROUP(fs->super) ||
	    start >= max_blks ||
	    fr->len < fs->blocksize)
		return -EINVAL;

	dbg_printf(ff, "%s: start=0x%llx end=0x%llx minlen=0x%llx\n", __func__,
		   start, end, minlen);

	if (start < fs->super->s_first_data_block)
		start = fs->super->s_first_data_block;

	if (end < fs->super->s_first_data_block)
		end = fs->super->s_first_data_block;
	if (end >= ext2fs_blocks_count(fs->super))
		end = ext2fs_blocks_count(fs->super) - 1;

	cleared = 0;
	max_blocks = FUSE4FS_B_TO_FSBT(ff, 2048ULL * 1024 * 1024);

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
		switch (err) {
		case 0:
			break;
		case ENOENT:
			/*
			 * No free blocks found between start and b; discard
			 * the entire range.
			 */
			err = 0;
			break;
		default:
			return translate_error(fs, fh->ino, err);
		}

		if (b - start >= minlen) {
			err = io_channel_discard(fs->io, start, b - start);
			if (err == EBUSY) {
				/*
				 * Apparently dm-thinp can return EBUSY when
				 * it's too busy deallocating thinp units to
				 * deallocate more.  Swallow these errors.
				 */
				err = 0;
			}
			if (err)
				return translate_error(fs, fh->ino, err);
			cleared += b - start;
			fr->len = FUSE4FS_FSB_TO_B(ff, cleared);
		}
		start = b + 1;
	}

out:
	fr->len = FUSE4FS_FSB_TO_B(ff, cleared);
	*outsize = sizeof(struct fstrim_range);
	dbg_printf(ff, "%s: len=%llu err=%ld\n", __func__, fr->len, err);
	return err;
}
#endif /* FITRIM */

#ifndef EXT4_IOC_SHUTDOWN
# define EXT4_IOC_SHUTDOWN	_IOR('X', 125, __u32)
#endif

static int ioctl_shutdown(struct fuse4fs *ff, const struct fuse_ctx *ctxt,
			  struct fuse4fs_file_handle *fh, const void *indata,
			  size_t insize)
{
	ext2_filsys fs = ff->fs;

	if (!fuse4fs_is_superuser(ff, ctxt))
		return -EPERM;

	err_printf(ff, "%s.\n", _("shut down requested"));

	fuse4fs_mmp_stop(ff);

	/*
	 * EXT4_IOC_SHUTDOWN inherited the inverted polarity on the ioctl
	 * direction from XFS.  Unfortunately, that means we can't implement
	 * any of the flags.  Flush whatever is dirty and shut down.
	 */
	if (ff->opstate == F4OP_WRITABLE)
		ext2fs_flush2(fs, 0);
	ff->opstate = F4OP_SHUTDOWN;
	fs->flags &= ~EXT2_FLAG_RW;

	return 0;
}

static void op_ioctl(fuse_req_t req, fuse_ino_t fino EXT2FS_ATTR((unused)),
		     unsigned int cmd,
		     void *arg EXT2FS_ATTR((unused)),
		     struct fuse_file_info *fp,
		     unsigned int flags EXT2FS_ATTR((unused)),
		     const void *indata, size_t insize,
		     size_t outsize)
{
	const struct fuse_ctx *ctxt = fuse_req_ctx(req);
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	void *outdata = NULL;
	int ret = 0;

	if (outsize > 0) {
		outdata = calloc(outsize, sizeof(char));
		if (!outdata) {
			fuse_reply_err(req, errno);
			return;
		}
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	fuse4fs_start(ff);
	switch ((unsigned long) cmd) {
#ifdef SUPPORT_I_FLAGS
	case EXT2_IOC_GETFLAGS:
		ret = ioctl_getflags(ff, fh, outdata, &outsize);
		break;
	case EXT2_IOC_SETFLAGS:
		ret = ioctl_setflags(ff, ctxt, fh, indata, insize);
		break;
	case EXT2_IOC_GETVERSION:
		ret = ioctl_getversion(ff, fh, outdata, &outsize);
		break;
	case EXT2_IOC_SETVERSION:
		ret = ioctl_setversion(ff, ctxt, fh, indata, insize);
		break;
#endif
#ifdef FS_IOC_FSGETXATTR
	case FS_IOC_FSGETXATTR:
		ret = ioctl_fsgetxattr(ff, fh, outdata, &outsize);
		break;
	case FS_IOC_FSSETXATTR:
		ret = ioctl_fssetxattr(ff, ctxt, fh, indata, insize);
		break;
#endif
#ifdef FITRIM
	case FITRIM:
		ret = ioctl_fitrim(ff, fh, indata, insize, outdata, &outsize);
		break;
#endif
	case EXT4_IOC_SHUTDOWN:
		ret = ioctl_shutdown(ff, ctxt, fh, indata, insize);
		break;
	default:
		dbg_printf(ff, "%s: Unknown ioctl %d\n", __func__, cmd);
		ret = -ENOTTY;
	}
	fuse4fs_finish(ff, ret);

	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_ioctl(req, 0, outdata, outsize);
	free(outdata);
}

static void op_bmap(fuse_req_t req, fuse_ino_t fino,
		    size_t blocksize EXT2FS_ATTR((unused)), uint64_t idx)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	ext2_ino_t ino;
	blk64_t blkno;
	errcode_t err;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);
	fs = fuse4fs_start(ff);
	dbg_printf(ff, "%s: ino=%d blk=%"PRIu64"\n", __func__, ino, idx);

	err = ext2fs_bmap2(fs, ino, NULL, NULL, 0, idx, 0, &blkno);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out;
	}

out:
	fuse4fs_finish(ff, ret);
	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_bmap(req, blkno);
}

#ifdef SUPPORT_FALLOCATE
static int fuse4fs_allocate_range(struct fuse4fs *ff,
				  struct fuse4fs_file_handle *fh, int mode,
				  off_t offset, off_t len)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	blk64_t start, end;
	__u64 fsize;
	errcode_t err;
	int flags;

	start = FUSE4FS_B_TO_FSBT(ff, offset);
	end = FUSE4FS_B_TO_FSBT(ff, offset + len - 1);
	dbg_printf(ff, "%s: ino=%d mode=0x%x offset=0x%llx len=0x%llx start=0x%llx end=0x%llx\n",
		   __func__, fh->ino, mode,
		   (unsigned long long)offset,
		   (unsigned long long)len,
		   (unsigned long long)start,
		   (unsigned long long)end);
	if (!fuse4fs_can_allocate(ff, FUSE4FS_B_TO_FSB(ff, len)))
		return -ENOSPC;

	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return err;
	fsize = EXT2_I_SIZE(&inode);

	/* Indirect files do not support unwritten extents */
	if (!(inode.i_flags & EXT4_EXTENTS_FL))
		return -EOPNOTSUPP;

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

	err = fuse4fs_update_mtime(ff, fh->ino, &inode);
	if (err)
		return err;

	err = fuse4fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return err;
}

static errcode_t fuse4fs_zero_middle(struct fuse4fs *ff, ext2_ino_t ino,
				     struct ext2_inode_large *inode,
				     off_t offset, off_t len, char **buf)
{
	ext2_filsys fs = ff->fs;
	blk64_t blk;
	off_t residue = FUSE4FS_OFF_IN_FSB(ff, offset);
	int retflags;
	errcode_t err;

	/* the kernel does this for us in iomap mode */
	if (fuse4fs_iomap_enabled(ff))
		return 0;

	if (!*buf) {
		err = ext2fs_get_mem(fs->blocksize, buf);
		if (err)
			return err;
	}

	err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), *buf, 0,
			   FUSE4FS_B_TO_FSBT(ff, offset), &retflags, &blk);
	if (err)
		return err;
	if (!blk || (retflags & BMAP_RET_UNINIT))
		return 0;

	err = io_channel_read_blk64(fs->io, blk, 1, *buf);
	if (err)
		return err;

	dbg_printf(ff, "%s: ino=%d offset=0x%llx len=0x%llx\n",
		   __func__, ino,
		   (unsigned long long)offset + residue,
		   (unsigned long long)len);
	memset(*buf + residue, 0, len);

	return io_channel_write_blk64(fs->io, blk, 1, *buf);
}

static errcode_t fuse4fs_zero_edge(struct fuse4fs *ff, ext2_ino_t ino,
				   struct ext2_inode_large *inode, off_t offset,
				   int clean_before, char **buf)
{
	ext2_filsys fs = ff->fs;
	blk64_t blk;
	int retflags;
	off_t residue;
	errcode_t err;

	/* the kernel does this for us in iomap mode */
	if (fuse4fs_iomap_enabled(ff))
		return 0;

	residue = FUSE4FS_OFF_IN_FSB(ff, offset);
	if (residue == 0)
		return 0;

	if (!*buf) {
		err = ext2fs_get_mem(fs->blocksize, buf);
		if (err)
			return err;
	}

	err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), *buf, 0,
			   FUSE4FS_B_TO_FSBT(ff, offset), &retflags, &blk);
	if (err)
		return err;

	err = io_channel_read_blk64(fs->io, blk, 1, *buf);
	if (err)
		return err;
	if (!blk || (retflags & BMAP_RET_UNINIT))
		return 0;

	if (clean_before) {
		dbg_printf(ff, "%s: ino=%d before offset=0x%llx len=0x%llx\n",
			   __func__, ino,
			   (unsigned long long)offset,
			   (unsigned long long)residue);
		memset(*buf, 0, residue);
	} else {
		dbg_printf(ff, "%s: ino=%d after offset=0x%llx len=0x%llx\n",
			   __func__, ino,
			   (unsigned long long)offset,
			   (unsigned long long)fs->blocksize - residue);
		memset(*buf + residue, 0, fs->blocksize - residue);
	}

	return io_channel_write_blk64(fs->io, blk, 1, *buf);
}

static int fuse4fs_punch_range(struct fuse4fs *ff,
			       struct fuse4fs_file_handle *fh, int mode,
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
	start = FUSE4FS_B_TO_FSBT(ff, round_up(offset, fs->blocksize));
	end = FUSE4FS_B_TO_FSBT(ff, round_down(offset + len, fs->blocksize));

	dbg_printf(ff,
 "%s: ino=%d mode=0x%x offset=0x%llx len=0x%llx start=0x%llx end=0x%llx\n",
		   __func__, fh->ino, mode,
		   (unsigned long long)offset,
		   (unsigned long long)len,
		   (unsigned long long)start,
		   (unsigned long long)end);

	err = fuse4fs_read_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	/*
	 * Indirect files do not support unwritten extents, which means we
	 * can't support zero range.  Punch goes first in zero-range, which
	 * is why the check is here.
	 */
	if ((mode & FL_ZERO_RANGE_FLAG) && !(inode.i_flags & EXT4_EXTENTS_FL))
		return -EOPNOTSUPP;

	/* Zero everything before the first block and after the last block */
	if (FUSE4FS_B_TO_FSBT(ff, offset) == FUSE4FS_B_TO_FSBT(ff, offset + len))
		err = fuse4fs_zero_middle(ff, fh->ino, &inode, offset,
					 len, &buf);
	else {
		err = fuse4fs_zero_edge(ff, fh->ino, &inode, offset, 0, &buf);
		if (!err)
			err = fuse4fs_zero_edge(ff, fh->ino, &inode,
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

	err = fuse4fs_update_mtime(ff, fh->ino, &inode);
	if (err)
		return err;

	err = fuse4fs_write_inode(fs, fh->ino, &inode);
	if (err)
		return translate_error(fs, fh->ino, err);

	return 0;
}

static int fuse4fs_zero_range(struct fuse4fs *ff,
			      struct fuse4fs_file_handle *fh, int mode,
			      off_t offset, off_t len)
{
	int ret = fuse4fs_punch_range(ff, fh, mode | FL_KEEP_SIZE_FLAG, offset,
				      len);

	if (!ret)
		ret = fuse4fs_allocate_range(ff, fh, mode, offset, len);
	return ret;
}

static void op_fallocate(fuse_req_t req, fuse_ino_t fino EXT2FS_ATTR((unused)),
			 int mode, off_t offset, off_t len,
			 struct fuse_file_info *fp)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct fuse4fs_file_handle *fh = fuse4fs_get_handle(fp);
	int ret;

	/* Catch unknown flags */
	if (mode & ~(FL_ZERO_RANGE_FLAG | FL_PUNCH_HOLE_FLAG | FL_KEEP_SIZE_FLAG)) {
		fuse_reply_err(req, EOPNOTSUPP);
		return;
	}

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CHECK_HANDLE(req, fh);
	fuse4fs_start(ff);
	if (!fuse4fs_is_writeable(ff)) {
		ret = -EROFS;
		goto out;
	}

	dbg_printf(ff, "%s: ino=%d mode=0x%x start=0x%llx end=0x%llx\n", __func__,
		   fh->ino, mode,
		   (unsigned long long)offset,
		   (unsigned long long)offset + len);

	if (mode & FL_ZERO_RANGE_FLAG)
		ret = fuse4fs_zero_range(ff, fh, mode, offset, len);
	else if (mode & FL_PUNCH_HOLE_FLAG)
		ret = fuse4fs_punch_range(ff, fh, mode, offset, len);
	else
		ret = fuse4fs_allocate_range(ff, fh, mode, offset, len);
out:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}
#endif /* SUPPORT_FALLOCATE */

#ifdef HAVE_FUSE_IOMAP
static void fuse4fs_iomap_hole(struct fuse4fs *ff, struct fuse_file_iomap *iomap,
			       off_t pos, uint64_t count)
{
	iomap->dev = FUSE_IOMAP_DEV_NULL;
	iomap->addr = FUSE_IOMAP_NULL_ADDR;
	iomap->offset = pos;
	iomap->length = count;
	iomap->type = FUSE_IOMAP_TYPE_HOLE;
}

static void fuse4fs_iomap_hole_to_eof(struct fuse4fs *ff,
				      struct fuse_file_iomap *iomap, off_t pos,
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

	fuse4fs_iomap_hole(ff, iomap, startoff, eofoff - startoff);
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

# define __DUMP_INFO(ff, func, tag, startoff, err, info) \
	do { \
		dbg_printf((ff), \
 "%s: %s startoff 0x%llx err %ld entry %d/%d/%d level  %d/%d\n", \
			   (func), (tag), (startoff), (err), \
			   (info)->curr_entry, (info)->num_entries, \
			   (info)->max_entries, (info)->curr_level, \
			   (info)->max_depth); \
	} while(0)
# define DUMP_INFO(ff, tag, startoff, err, info) \
	__DUMP_INFO((ff), __func__, (tag), (startoff), (err), (info))
#else
# define __DUMP_EXTENT(...)	((void)0)
# define DUMP_EXTENT(...)	((void)0)
# define DUMP_INFO(...)		((void)0)
#endif

static inline errcode_t __fuse4fs_get_mapping_at(struct fuse4fs *ff,
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

static inline errcode_t __fuse4fs_get_next_mapping(struct fuse4fs *ff,
						   ext2_extent_handle_t handle,
						   blk64_t startoff,
						   struct ext2fs_extent *bmap,
						   const char *func)
{
	struct ext2fs_extent newex;
	struct ext2_extent_info info;
	errcode_t err;

	/*
	 * The extent tree code has this (probably broken) behavior that if
	 * more than two of the highest levels of the cursor point at the
	 * rightmost edge of an extent tree block, a _NEXT_LEAF movement fails
	 * to move the cursor position of any of the lower levels.  IOWs, if
	 * leaf level N is at the right edge, it will only advance level N-1
	 * to the right.  If N-1 was at the right edge, the cursor resets to
	 * record 0 of that level and goes down to the wrong leaf.
	 *
	 * Work around this by walking up (towards root level 0) the extent
	 * tree until we find a level where we're not already at the rightmost
	 * edge.  The _NEXT_LEAF movement will walk down the tree to find the
	 * leaves.
	 */
	err = ext2fs_extent_get_info(handle, &info);
	DUMP_INFO(ff, "UP?", startoff, err, &info);
	if (err)
		return err;

	while (info.curr_entry == info.num_entries && info.curr_level > 0) {
		err = ext2fs_extent_get(handle, EXT2_EXTENT_UP, &newex);
		DUMP_EXTENT(ff, "UP", startoff, err, &newex);
		if (err)
			return err;
		err = ext2fs_extent_get_info(handle, &info);
		DUMP_INFO(ff, "UP", startoff, err, &info);
		if (err)
			return err;
	}

	/*
	 * If we're at the root and there are no more entries, there's nothing
	 * else to be found.
	 */
	if (info.curr_level == 0 && info.curr_entry == info.num_entries)
		return EXT2_ET_EXTENT_NOT_FOUND;

	/* Otherwise grab this next leaf and return it. */
	err = ext2fs_extent_get(handle, EXT2_EXTENT_NEXT_LEAF, &newex);
	DUMP_EXTENT(ff, "NEXT", startoff, err, &newex);
	if (err)
		return err;

	*bmap = newex;
	return 0;
}

#define fuse4fs_get_mapping_at(ff, handle, startoff, bmap) \
	__fuse4fs_get_mapping_at((ff), (handle), (startoff), (bmap), __func__)
#define fuse4fs_get_next_mapping(ff, handle, startoff, bmap) \
	__fuse4fs_get_next_mapping((ff), (handle), (startoff), (bmap), __func__)

static errcode_t fuse4fs_iomap_begin_extent(struct fuse4fs *ff, uint64_t ino,
					    struct ext2_inode_large *inode,
					    off_t pos, uint64_t count,
					    uint32_t opflags,
					    struct fuse_file_iomap *iomap)
{
	ext2_extent_handle_t handle;
	struct ext2fs_extent extent = { };
	ext2_filsys fs = ff->fs;
	const blk64_t startoff = FUSE4FS_B_TO_FSBT(ff, pos);
	errcode_t err;
	int ret = 0;

	err = ext2fs_extent_open2(fs, ino, EXT2_INODE(inode), &handle);
	if (err)
		return translate_error(fs, ino, err);

	err = fuse4fs_get_mapping_at(ff, handle, startoff, &extent);
	if (err == EXT2_ET_EXTENT_NOT_FOUND) {
		/* No mappings at all; the whole range is a hole. */
		fuse4fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);
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
		fuse4fs_iomap_hole(ff, iomap, FUSE4FS_FSB_TO_B(ff, startoff),
				FUSE4FS_FSB_TO_B(ff, extent.e_lblk - startoff));
		goto out_handle;
	}

	if (startoff >= extent.e_lblk + extent.e_len) {
		/*
		 * Mapping ends to the left of the current position.  Try to
		 * find the next mapping.  If there is no next mapping, the
		 * whole range is in a hole.
		 */
		err = fuse4fs_get_next_mapping(ff, handle, startoff, &extent);
		if (err == EXT2_ET_EXTENT_NOT_FOUND) {
			fuse4fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);
			goto out_handle;
		}

		/*
		 * If the new mapping starts to the right of startoff, there's
		 * a hole from startoff to the start of the new mapping.
		 */
		if (startoff < extent.e_lblk) {
			fuse4fs_iomap_hole(ff, iomap,
				FUSE4FS_FSB_TO_B(ff, startoff),
				FUSE4FS_FSB_TO_B(ff, extent.e_lblk - startoff));
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
	iomap->addr = FUSE4FS_FSB_TO_B(ff, extent.e_pblk);
	iomap->offset = FUSE4FS_FSB_TO_B(ff, extent.e_lblk);
	iomap->length = FUSE4FS_FSB_TO_B(ff, extent.e_len);
	if (extent.e_flags & EXT2_EXTENT_FLAGS_UNINIT)
		iomap->type = FUSE_IOMAP_TYPE_UNWRITTEN;
	else
		iomap->type = FUSE_IOMAP_TYPE_MAPPED;

out_handle:
	ext2fs_extent_free(handle);
	return ret;
}

static int fuse4fs_iomap_begin_indirect(struct fuse4fs *ff, uint64_t ino,
					struct ext2_inode_large *inode,
					off_t pos, uint64_t count,
					uint32_t opflags,
					struct fuse_file_iomap *iomap)
{
	ext2_filsys fs = ff->fs;
	blk64_t startoff = FUSE4FS_B_TO_FSBT(ff, pos);
	uint64_t isize = EXT2_I_SIZE(inode);
	uint64_t real_count = min(count, 131072);
	const blk64_t endoff = FUSE4FS_B_TO_FSB(ff, pos + real_count);
	blk64_t startblock;
	errcode_t err;

	err = ext2fs_bmap2(fs, ino, EXT2_INODE(inode), NULL, 0, startoff, NULL,
			   &startblock);
	if (err)
		return translate_error(fs, ino, err);

	iomap->offset = FUSE4FS_FSB_TO_B(ff, startoff);
	iomap->flags |= FUSE_IOMAP_F_MERGED;
	if (startblock) {
		iomap->dev = ff->iomap_dev;
		iomap->addr = FUSE4FS_FSB_TO_B(ff, startblock);
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
			if (startblock == 0)
				iomap->length += fs->blocksize;
			else
				break;
		}
	}

	/*
	 * If this is a hole that goes beyond EOF, report this as a hole to the
	 * end of the range queried so that FIEMAP doesn't go mad.
	 */
	if (iomap->type == FUSE_IOMAP_TYPE_HOLE &&
	    iomap->offset + iomap->length >= isize)
		fuse4fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);

	return 0;
}

static int fuse4fs_iomap_begin_inline(struct fuse4fs *ff, ext2_ino_t ino,
				      struct ext2_inode_large *inode, off_t pos,
				      uint64_t count, struct fuse_file_iomap *iomap)
{
	uint64_t one_fsb = FUSE4FS_FSB_TO_B(ff, 1);

	if (pos >= one_fsb) {
		fuse4fs_iomap_hole_to_eof(ff, iomap, pos, count, inode);
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

static int fuse4fs_iomap_begin_report(struct fuse4fs *ff, ext2_ino_t ino,
				      struct ext2_inode_large *inode,
				      off_t pos, uint64_t count,
				      uint32_t opflags,
				      struct fuse_file_iomap *read)
{
	if (inode->i_flags & EXT4_INLINE_DATA_FL)
		return fuse4fs_iomap_begin_inline(ff, ino, inode, pos, count,
						  read);

	if (inode->i_flags & EXT4_EXTENTS_FL)
		return fuse4fs_iomap_begin_extent(ff, ino, inode, pos, count,
						  opflags, read);

	return fuse4fs_iomap_begin_indirect(ff, ino, inode, pos, count,
					    opflags, read);
}

static int fuse4fs_iomap_begin_read(struct fuse4fs *ff, ext2_ino_t ino,
				    struct ext2_inode_large *inode, off_t pos,
				    uint64_t count, uint32_t opflags,
				    struct fuse_file_iomap *read)
{
	/* fall back to slow path for inline data reads */
	if (inode->i_flags & EXT4_INLINE_DATA_FL)
		return fuse4fs_iomap_begin_inline(ff, ino, inode, pos, count,
						  read);

	if (inode->i_flags & EXT4_EXTENTS_FL)
		return fuse4fs_iomap_begin_extent(ff, ino, inode, pos, count,
						  opflags, read);

	return fuse4fs_iomap_begin_indirect(ff, ino, inode, pos, count,
					    opflags, read);
}

static int fuse4fs_iomap_write_allocate(struct fuse4fs *ff, ext2_ino_t ino,
					struct ext2_inode_large *inode,
					off_t pos, uint64_t count,
					uint32_t opflags,
					struct fuse_file_iomap *read,
					bool *dirty)
{
	ext2_filsys fs = ff->fs;
	blk64_t startoff = FUSE4FS_B_TO_FSBT(ff, pos);
	blk64_t stopoff = FUSE4FS_B_TO_FSB(ff, pos + count);
	blk64_t old_iblocks;
	errcode_t err;
	int ret;

	dbg_printf(ff,
 "%s: ino=%d startoff 0x%llx blockcount 0x%llx\n",
		   __func__, ino, startoff, stopoff - startoff);

	if (!fuse4fs_can_allocate(ff, stopoff - startoff))
		return -ENOSPC;

	old_iblocks = ext2fs_get_stat_i_blocks(fs, EXT2_INODE(inode));
	err = ext2fs_fallocate(fs, EXT2_FALLOCATE_FORCE_UNINIT, ino,
			       EXT2_INODE(inode), ~0ULL, startoff,
			       stopoff - startoff);
	if (err)
		return translate_error(fs, ino, err);

	/*
	 * New allocations for file data blocks on indirect mapped files are
	 * zeroed through the IO manager so we have to flush it to disk.
	 */
	if (!(inode->i_flags & EXT4_EXTENTS_FL) &&
	    old_iblocks != ext2fs_get_stat_i_blocks(fs, EXT2_INODE(inode))) {
		err = io_channel_flush(fs->io);
		if (err)
			return translate_error(fs, ino, err);
	}

	/* pick up the newly allocated mapping */
	ret = fuse4fs_iomap_begin_read(ff, ino, inode, pos, count, opflags,
				       read);
	if (ret)
		return ret;

	read->flags |= FUSE_IOMAP_F_DIRTY;
	*dirty = true;
	return 0;
}

static off_t fuse4fs_max_file_size(const struct fuse4fs *ff,
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

	return FUSE4FS_FSB_TO_B(ff, max_map_block) + (fs->blocksize - 1);
}

static int fuse4fs_iomap_begin_write(struct fuse4fs *ff, ext2_ino_t ino,
				     struct ext2_inode_large *inode, off_t pos,
				     uint64_t count, uint32_t opflags,
				     struct fuse_file_iomap *read,
				     bool *dirty)
{
	off_t max_size = fuse4fs_max_file_size(ff, inode);
	int ret;

	if (pos >= max_size)
		return -EFBIG;

	if (pos >= max_size - count)
		count = max_size - pos;

	ret = fuse4fs_iomap_begin_read(ff, ino, inode, pos, count, opflags,
				       read);
	if (ret)
		return ret;

	if (fuse_iomap_need_write_allocate(opflags, read)) {
		ret = fuse4fs_iomap_write_allocate(ff, ino, inode, pos, count,
						   opflags, read, dirty);
		if (ret)
			return ret;
	}

	return 0;
}

static void op_iomap_begin(fuse_req_t req, fuse_ino_t fino, uint64_t dontcare,
			   off_t pos, uint64_t count, uint32_t opflags)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct ext2_inode_large inode;
	struct fuse_file_iomap read = { };
	ext2_filsys fs;
	ext2_ino_t ino;
	errcode_t err;
	bool dirty = false;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);

	dbg_printf(ff, "%s: ino=%d pos=0x%llx count=0x%llx opflags=0x%x\n",
		   __func__, ino,
		   (unsigned long long)pos,
		   (unsigned long long)count,
		   opflags);

	fs = fuse4fs_start(ff);
	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_unlock;
	}

	if (opflags & FUSE_IOMAP_OP_REPORT)
		ret = fuse4fs_iomap_begin_report(ff, ino, &inode, pos, count,
						 opflags, &read);
	else if (fuse_iomap_is_write(opflags))
		ret = fuse4fs_iomap_begin_write(ff, ino, &inode, pos, count,
						opflags, &read, &dirty);
	else
		ret = fuse4fs_iomap_begin_read(ff, ino, &inode, pos, count,
					       opflags, &read);
	if (ret)
		goto out_unlock;

	dbg_printf(ff,
 "%s: ino=%d pos=0x%llx -> addr=0x%llx offset=0x%llx length=0x%llx type=%u flags=0x%x\n",
		   __func__, ino,
		   (unsigned long long)pos,
		   (unsigned long long)read.addr,
		   (unsigned long long)read.offset,
		   (unsigned long long)read.length,
		   read.type,
		   read.flags);

	/* Not filling even the first byte will make the kernel unhappy. */
	if (ff->debug && (read.offset > pos ||
			  read.offset + read.length <= pos))
		fuse4fs_dump_extents(ff, ino, &inode, "BAD DATA");

	if (dirty) {
		err = fuse4fs_write_inode(fs, ino, &inode);
		if (err) {
			ret = translate_error(fs, ino, err);
			goto out_unlock;
		}
	}

	if (opflags & FUSE_IOMAP_OP_ATOMIC)
		read.flags |= FUSE_IOMAP_F_ATOMIC_BIO;

out_unlock:
	fuse4fs_finish(ff, ret);
	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_iomap_begin(req, &read, NULL);
}

static int fuse4fs_iomap_append_setsize(struct fuse4fs *ff, ext2_ino_t ino,
					loff_t newsize)
{
	ext2_filsys fs = ff->fs;
	struct ext2_inode_large inode;
	ext2_off64_t isize;
	errcode_t err;

	dbg_printf(ff, "%s: ino=%u newsize=%llu\n", __func__, ino,
		   (unsigned long long)newsize);

	err = fuse4fs_read_inode(fs, ino, &inode);
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

	err = fuse4fs_write_inode(fs, ino, &inode);
	if (err)
		return translate_error(fs, ino, err);

	return 0;
}

static void op_iomap_end(fuse_req_t req, fuse_ino_t fino, uint64_t dontcare,
			 off_t pos, uint64_t count, uint32_t opflags,
			 ssize_t written, const struct fuse_file_iomap *iomap)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_ino_t ino;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);

	dbg_printf(ff,
 "%s: ino=%d pos=0x%llx count=0x%llx opflags=0x%x written=0x%zx mapflags=0x%x\n",
		   __func__, ino,
		   (unsigned long long)pos,
		   (unsigned long long)count,
		   opflags,
		   written,
		   iomap->flags);

	fuse4fs_start(ff);

	/* XXX is this really necessary? */
	if ((opflags & FUSE_IOMAP_OP_WRITE) &&
	    !(opflags & FUSE_IOMAP_OP_DIRECT) &&
	    (iomap->flags & FUSE_IOMAP_F_SIZE_CHANGED) &&
	    written > 0) {
		ret = fuse4fs_iomap_append_setsize(ff, ino, pos + written);
		if (ret)
			goto out_unlock;
	}

out_unlock:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
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
static off_t fuse4fs_max_size(struct fuse4fs *ff, off_t upper_limit)
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
static int fuse4fs_set_bdev_blocksize(struct fuse4fs *ff, int fd)
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

	/* Pretend that BLKBSZSET rejected our proposed block size */
	if (blocksize > ff->fs->blocksize) {
		set_error = EINVAL;
		goto out_bad;
	}

	return 0;
out_bad:
	err_printf(ff, "%s: cannot set blocksize %u: %s\n", __func__,
		   blocksize, strerror(set_error));
	return -EIO;
}

#ifdef STATX_WRITE_ATOMIC
static void fuse4fs_configure_atomic_write(struct fuse4fs *ff, int bdev_fd)
{
	struct statx devx;
	unsigned int awu_min, awu_max;
	int ret;

	if (!ext2fs_has_feature_extents(ff->fs->super))
		return;

	ret = statx(bdev_fd, "", AT_EMPTY_PATH, STATX_WRITE_ATOMIC, &devx);
	if (ret)
		return;
	if (!(devx.stx_mask & STATX_WRITE_ATOMIC))
		return;

	awu_min = max(ff->fs->blocksize, devx.stx_atomic_write_unit_min);
	awu_max = min(ff->fs->blocksize, devx.stx_atomic_write_unit_max);
	if (awu_min > awu_max)
		return;

	log_printf(ff, "%s awu_min: %u, awu_max: %u\n",
		   _("Supports (experimental) DIO atomic writes"),
		   awu_min, awu_max);

	ff->awu_min = awu_min;
	ff->awu_max = awu_max;
}
#else
# define fuse4fs_configure_atomic_write(...)	((void)0)
#endif

static int fuse4fs_iomap_config_devices(struct fuse4fs *ff)
{
	errcode_t err;
	int fd;
	int ret;

	err = io_channel_get_fd(ff->fs->io, &fd);
	if (err)
		return translate_error(ff->fs, 0, err);

	ret = fuse4fs_set_bdev_blocksize(ff, fd);
	if (ret)
		return ret;

	ret = fuse_lowlevel_iomap_device_add(ff->fuse, fd, 0);
	if (ret < 0) {
		dbg_printf(ff, "%s: cannot register iomap dev fd=%d, err=%d\n",
			   __func__, fd, -ret);
		return translate_error(ff->fs, 0, -ret);
	}

	dbg_printf(ff, "%s: registered iomap dev fd=%d iomap_dev=%u\n",
		   __func__, fd, ff->iomap_dev);

	fuse4fs_configure_atomic_write(ff, fd);

	ff->iomap_dev = ret;
	return 0;
}

static void fuse4fs_invalidate_bdev(struct fuse4fs *ff, blk64_t blk, blk_t num)
{
	off_t offset = FUSE4FS_FSB_TO_B(ff, blk);
	off_t length = FUSE4FS_FSB_TO_B(ff, num);
	int ret;

	ret = fuse_lowlevel_iomap_device_invalidate(ff->fuse, ff->iomap_dev,
						    offset, length);
	if (!ret)
		return;

	if (num == 1)
		err_printf(ff, "%s %llu: %s\n",
			   _("error invalidating block"),
			   (unsigned long long)blk,
			   strerror(ret));
	else
		err_printf(ff, "%s %llu-%llu: %s\n",
			   _("error invalidating blocks"),
			   (unsigned long long)blk,
			   (unsigned long long)blk + num - 1,
			   strerror(ret));
}

static void fuse4fs_alloc_stats(ext2_filsys fs, blk64_t blk, int inuse)
{
	struct fuse4fs *ff = fs->priv_data;

	if (inuse < 0)
		fuse4fs_invalidate_bdev(ff, blk, 1);
	if (ff->old_alloc_stats)
		ff->old_alloc_stats(fs, blk, inuse);
}

static void fuse4fs_alloc_stats_range(ext2_filsys fs, blk64_t blk, blk_t num,
				      int inuse)
{
	struct fuse4fs *ff = fs->priv_data;

	if (inuse < 0)
		fuse4fs_invalidate_bdev(ff, blk, num);
	if (ff->old_alloc_stats_range)
		ff->old_alloc_stats_range(fs, blk, num, inuse);
}

static void op_iomap_config(fuse_req_t req, uint64_t flags, uint64_t maxbytes)
{
	struct fuse_iomap_config cfg = { };
	struct fuse4fs *ff = fuse4fs_get(req);
	ext2_filsys fs;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);

	dbg_printf(ff, "%s: flags=0x%llx maxbytes=0x%llx\n", __func__,
		   (unsigned long long)flags,
		   (unsigned long long)maxbytes);
	fs = fuse4fs_start(ff);

	cfg.flags |= FUSE_IOMAP_CONFIG_UUID;
	memcpy(cfg.s_uuid, fs->super->s_uuid, sizeof(cfg.s_uuid));
	cfg.s_uuid_len = sizeof(fs->super->s_uuid);

	cfg.flags |= FUSE_IOMAP_CONFIG_BLOCKSIZE;
	cfg.s_blocksize = FUSE4FS_FSB_TO_B(ff, 1);

	/*
	 * If there inode is large enough to house i_[acm]time_extra then we
	 * can turn on nanosecond timestamps; i_crtime was the next field added
	 * after i_atime_extra.
	 */
	cfg.flags |= FUSE_IOMAP_CONFIG_TIME;
	if (fs->super->s_inode_size >=
	    offsetof(struct ext2_inode_large, i_crtime)) {
		cfg.s_time_gran = 1;
		cfg.s_time_max = EXT4_EXTRA_TIMESTAMP_MAX;
	} else {
		cfg.s_time_gran = NSEC_PER_SEC;
		cfg.s_time_max = EXT4_NON_EXTRA_TIMESTAMP_MAX;
	}
	cfg.s_time_min = EXT4_TIMESTAMP_MIN;

	cfg.flags |= FUSE_IOMAP_CONFIG_MAXBYTES;
	cfg.s_maxbytes = fuse4fs_max_size(ff, maxbytes);

	ret = fuse4fs_iomap_config_devices(ff);
	if (ret)
		goto out_unlock;

	/*
	 * If we let iomap do all file block IO, then we need to watch for
	 * freed blocks so that we can invalidate any page cache that might
	 * get written to the block deivce.
	 */
	if (fuse4fs_iomap_enabled(ff)) {
		ext2fs_set_block_alloc_stats_callback(ff->fs,
				fuse4fs_alloc_stats, &ff->old_alloc_stats);
		ext2fs_set_block_alloc_stats_range_callback(ff->fs,
				fuse4fs_alloc_stats_range,
				&ff->old_alloc_stats_range);
	}

out_unlock:
	fuse4fs_finish(ff, ret);
	if (ret)
		fuse_reply_err(req, -ret);
	else
		fuse_reply_iomap_config(req, &cfg);
}

static inline bool fuse4fs_can_merge_mappings(const struct ext2fs_extent *left,
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

static int fuse4fs_try_merge_mappings(struct fuse4fs *ff, ext2_ino_t ino,
				      ext2_extent_handle_t handle,
				      blk64_t startoff)
{
	ext2_filsys fs = ff->fs;
	struct ext2fs_extent left, right;
	errcode_t err;

	/* Look up the mappings before startoff */
	err = fuse4fs_get_mapping_at(ff, handle, startoff - 1, &left);
	if (err == EXT2_ET_EXTENT_NOT_FOUND)
		return 0;
	if (err)
		return translate_error(fs, ino, err);

	/* Look up the mapping at startoff */
	err = fuse4fs_get_mapping_at(ff, handle, startoff, &right);
	if (err == EXT2_ET_EXTENT_NOT_FOUND)
		return 0;
	if (err)
		return translate_error(fs, ino, err);

	/* Can we combine them? */
	if (!fuse4fs_can_merge_mappings(&left, &right))
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

static int fuse4fs_convert_unwritten_mapping(struct fuse4fs *ff,
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
	err = fuse4fs_get_mapping_at(ff, handle, startoff, &extent);
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
		err = fuse4fs_get_next_mapping(ff, handle, startoff, &extent);
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
		err = fuse4fs_try_merge_mappings(ff, ino, handle, startoff);
		if (err)
			return translate_error(fs, ino, err);
	}

	*cursor = extent.e_lblk + extent.e_len;
	return 0;
}

static int fuse4fs_convert_unwritten_mappings(struct fuse4fs *ff,
					      ext2_ino_t ino,
					      struct ext2_inode_large *inode,
					      off_t pos, size_t written)
{
	ext2_extent_handle_t handle;
	ext2_filsys fs = ff->fs;
	blk64_t startoff = FUSE4FS_B_TO_FSBT(ff, pos);
	const blk64_t stopoff = FUSE4FS_B_TO_FSB(ff, pos + written);
	errcode_t err;
	int ret;

	err = ext2fs_extent_open2(fs, ino, EXT2_INODE(inode), &handle);
	if (err)
		return translate_error(fs, ino, err);

	/* Walk every mapping in the range, converting them. */
	while (startoff < stopoff) {
		blk64_t old_startoff = startoff;

		ret = fuse4fs_convert_unwritten_mapping(ff, ino, inode, handle,
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
	ret = fuse4fs_try_merge_mappings(ff, ino, handle, stopoff);
out_handle:
	ext2fs_extent_free(handle);
	return ret;
}

static void op_iomap_ioend(fuse_req_t req, fuse_ino_t fino, uint64_t dontcare,
			   off_t pos, size_t written, uint32_t ioendflags,
			   int error, uint64_t new_addr)
{
	struct fuse4fs *ff = fuse4fs_get(req);
	struct ext2_inode_large inode;
	ext2_filsys fs;
	ext2_ino_t ino;
	errcode_t err;
	bool dirty = false;
	int ret = 0;

	FUSE4FS_CHECK_CONTEXT(req);
	FUSE4FS_CONVERT_FINO(req, &ino, fino);

	dbg_printf(ff,
 "%s: ino=%d pos=0x%llx written=0x%zx ioendflags=0x%x error=%d new_addr=0x%llx\n",
		   __func__, ino,
		   (unsigned long long)pos,
		   written,
		   ioendflags,
		   error,
		   (unsigned long long)new_addr);

	if (error) {
		fuse_reply_err(req, -error);
		return;
	}

	fs = fuse4fs_start(ff);

	/* should never see these ioend types */
	if (ioendflags & FUSE_IOMAP_IOEND_SHARED) {
		ret = translate_error(fs, ino, EXT2_ET_FILESYSTEM_CORRUPTED);
		goto out_unlock;
	}

	err = fuse4fs_read_inode(fs, ino, &inode);
	if (err) {
		ret = translate_error(fs, ino, err);
		goto out_unlock;
	}

	if (ioendflags & FUSE_IOMAP_IOEND_UNWRITTEN) {
		/* unwritten extents are only supported on extents files */
		if (!(inode.i_flags & EXT4_EXTENTS_FL)) {
			ret = translate_error(fs, ino,
					      EXT2_ET_FILESYSTEM_CORRUPTED);
			goto out_unlock;
		}

		ret = fuse4fs_convert_unwritten_mappings(ff, ino, &inode,
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
				ret = translate_error(fs, ino, err);
				goto out_unlock;
			}

			dirty = true;
		}
	}

	if (dirty) {
		err = fuse4fs_write_inode(fs, ino, &inode);
		if (err) {
			ret = translate_error(fs, ino, err);
			goto out_unlock;
		}
	}

out_unlock:
	fuse4fs_finish(ff, ret);
	fuse_reply_err(req, -ret);
}
#endif /* HAVE_FUSE_IOMAP */

static struct fuse_lowlevel_ops fs_ops = {
	.lookup = op_lookup,
	.setattr = op_setattr,
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
	.readdirplus = op_readdirplus,
	.releasedir = op_release,
	.fsyncdir = op_fsync,
	.access = op_access,
	.create = op_create,
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 17)
	.tmpfile = op_tmpfile,
#endif
	.bmap = op_bmap,
#ifdef SUPERFLUOUS
	.lock = op_lock,
	.poll = op_poll,
#endif
	.ioctl = op_ioctl,
#ifdef SUPPORT_FALLOCATE
	.fallocate = op_fallocate,
#endif
#if FUSE_VERSION >= FUSE_MAKE_VERSION(3, 18)
	.statx = op_statx,
#endif
#ifdef HAVE_FUSE_IOMAP
	.iomap_begin = op_iomap_begin,
	.iomap_end = op_iomap_end,
	.iomap_config = op_iomap_config,
	.iomap_ioend = op_iomap_ioend,
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
	FUSE4FS_IGNORED,
	FUSE4FS_VERSION,
	FUSE4FS_HELP,
	FUSE4FS_HELPFULL,
	FUSE4FS_CACHE_SIZE,
	FUSE4FS_DIRSYNC,
	FUSE4FS_ERRORS_BEHAVIOR,
#ifdef HAVE_FUSE_IOMAP
	FUSE4FS_IOMAP,
	FUSE4FS_IOMAP_PASSTHROUGH,
#endif
};

#define FUSE4FS_OPT(t, p, v) { t, offsetof(struct fuse4fs, p), v }

static struct fuse_opt fuse4fs_opts[] = {
	FUSE4FS_OPT("ro",		ro,			1),
	FUSE4FS_OPT("rw",		ro,			0),
	FUSE4FS_OPT("minixdf",		minixdf,		1),
	FUSE4FS_OPT("bsddf",		minixdf,		0),
	FUSE4FS_OPT("fakeroot",		fakeroot,		1),
	FUSE4FS_OPT("fuse4fs_debug",	debug,			1),
	FUSE4FS_OPT("no_default_opts",	no_default_opts,	1),
	FUSE4FS_OPT("norecovery",	norecovery,		1),
	FUSE4FS_OPT("noload",		norecovery,		1),
	FUSE4FS_OPT("offset=%lu",	offset,			0),
	FUSE4FS_OPT("kernel",		kernel,			1),
	FUSE4FS_OPT("directio",		directio,		1),
	FUSE4FS_OPT("acl",		acl,			1),
	FUSE4FS_OPT("noacl",		acl,			0),
	FUSE4FS_OPT("lockfile=%s",	lockfile,		0),
#ifdef HAVE_CLOCK_MONOTONIC
	FUSE4FS_OPT("timing",		timing,			1),
#endif

#ifdef HAVE_FUSE_IOMAP
#ifdef MS_LAZYTIME
	FUSE_OPT_KEY("lazytime",	FUSE4FS_IOMAP_PASSTHROUGH),
	FUSE_OPT_KEY("nolazytime",	FUSE4FS_IOMAP_PASSTHROUGH),
#endif
#ifdef MS_STRICTATIME
	FUSE_OPT_KEY("strictatime",	FUSE4FS_IOMAP_PASSTHROUGH),
	FUSE_OPT_KEY("nostrictatime",	FUSE4FS_IOMAP_PASSTHROUGH),
#endif
#endif

	FUSE_OPT_KEY("user_xattr",	FUSE4FS_IGNORED),
	FUSE_OPT_KEY("noblock_validity", FUSE4FS_IGNORED),
	FUSE_OPT_KEY("nodelalloc",	FUSE4FS_IGNORED),
	FUSE_OPT_KEY("cache_size=%s",	FUSE4FS_CACHE_SIZE),
	FUSE_OPT_KEY("dirsync",		FUSE4FS_DIRSYNC),
	FUSE_OPT_KEY("errors=%s",	FUSE4FS_ERRORS_BEHAVIOR),
#ifdef HAVE_FUSE_IOMAP
	FUSE_OPT_KEY("iomap=%s",	FUSE4FS_IOMAP),
	FUSE_OPT_KEY("iomap",		FUSE4FS_IOMAP),
#endif

	FUSE_OPT_KEY("-V",             FUSE4FS_VERSION),
	FUSE_OPT_KEY("--version",      FUSE4FS_VERSION),
	FUSE_OPT_KEY("-h",             FUSE4FS_HELP),
	FUSE_OPT_KEY("--help",         FUSE4FS_HELP),
	FUSE_OPT_KEY("--helpfull",     FUSE4FS_HELPFULL),
	FUSE_OPT_END
};


static int fuse4fs_opt_proc(void *data, const char *arg,
			    int key, struct fuse_args *outargs)
{
	struct fuse4fs *ff = data;

	switch (key) {
#ifdef HAVE_FUSE_IOMAP
	case FUSE4FS_IOMAP_PASSTHROUGH:
		ff->iomap_passthrough_options = 1;
		/* pass through to libfuse */
		return 1;
#endif
	case FUSE4FS_DIRSYNC:
		ff->dirsync = 1;
		/* pass through to libfuse */
		return 1;
	case FUSE_OPT_KEY_NONOPT:
		if (!ff->device) {
			ff->device = strdup(arg);
			return 0;
		}
		return 1;
	case FUSE4FS_CACHE_SIZE:
		ff->cache_size = parse_num_blocks2(arg + 11, -1);
		if (ff->cache_size < 1 || ff->cache_size > INT32_MAX) {
			fprintf(stderr, "%s: %s\n", arg,
 _("cache size must be between 1 block and 2GB."));
			return -1;
		}

		/* do not pass through to libfuse */
		return 0;
	case FUSE4FS_ERRORS_BEHAVIOR:
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
	case FUSE4FS_IOMAP:
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
	case FUSE4FS_IGNORED:
		return 0;
	case FUSE4FS_HELP:
	case FUSE4FS_HELPFULL:
		fprintf(stderr,
	"usage: %s device/image mountpoint [options]\n"
	"\n"
	"general options:\n"
	"    -o opt,[opt...]  mount options\n"
	"    -h   --help      print help\n"
	"    -V   --version   print version\n"
	"\n"
	"fuse4fs options:\n"
	"    -o errors=panic        dump core on error\n"
	"    -o minixdf             minix-style df\n"
	"    -o fakeroot            pretend to be root for permission checks\n"
	"    -o no_default_opts     do not include default fuse options\n"
	"    -o offset=<bytes>      similar to mount -o offset=<bytes>, mount the partition starting at <bytes>\n"
	"    -o norecovery          don't replay the journal\n"
	"    -o fuse4fs_debug       enable fuse4fs debugging\n"
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
		if (key == FUSE4FS_HELPFULL) {
			printf("FUSE options:\n");
			fuse_cmdline_help();
		} else {
			fprintf(stderr, "Try --helpfull to get a list of "
				"all flags, including the FUSE options.\n");
		}
		exit(1);

	case FUSE4FS_VERSION:
		fprintf(stderr, "fuse4fs %s (%s)\n", E2FSPROGS_VERSION,
			E2FSPROGS_DATE);
		fprintf(stderr, "FUSE library version %s\n", fuse_pkgversion());
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

static void fuse4fs_com_err_proc(const char *whoami, errcode_t code,
				 const char *fmt, va_list args)
{
	fprintf(stderr, "FUSE4FS (%s): ", err_shortdev ? err_shortdev : "?");
	if (whoami)
		fprintf(stderr, "%s: ", whoami);
	fprintf(stderr, "%s ", error_message(code));
        vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	fflush(stderr);
}

static int fuse4fs_main(struct fuse_args *args, struct fuse4fs *ff)
{
	struct fuse_cmdline_opts opts;
	struct fuse_session *se;
	struct fuse_loop_config *loop_config = NULL;
	int ret;

	if (fuse_parse_cmdline(args, &opts) != 0) {
		ret = 1;
		goto out;
	}

	if (ff->debug)
		opts.debug = true;

	if (opts.show_help) {
		fuse_cmdline_help();
		ret = 0;
		goto out_free_opts;
	}

	if (opts.show_version) {
		printf("FUSE library version %s\n", fuse_pkgversion());
		ret = 0;
		goto out_free_opts;
	}

	if (!opts.mountpoint) {
		fprintf(stderr, "error: no mountpoint specified\n");
		ret = 2;
		goto out_free_opts;
	}

	se = fuse_session_new(args, &fs_ops, sizeof(fs_ops), ff);
	if (se == NULL) {
		ret = 3;
		goto out_free_opts;
	}
	ff->fuse = se;

	if (fuse_session_mount(se, opts.mountpoint) != 0) {
		ret = 4;
		goto out_destroy_session;
	}

	if (fuse_daemonize(opts.foreground) != 0) {
		ret = 5;
		goto out_unmount;
	}

	/*
	 * Configure logging a second time, because libfuse might have
	 * redirected std{out,err} as part of daemonization.  If this fails,
	 * give up and move on.
	 */
	fuse4fs_setup_logging(ff);
	if (ff->logfd >= 0)
		close(ff->logfd);
	ff->logfd = -1;

	if (fuse_set_signal_handlers(se) != 0) {
		ret = 6;
		goto out_unmount;
	}

	loop_config = fuse_loop_cfg_create();
	if (loop_config == NULL) {
		ret = 7;
		goto out_remove_signal_handlers;
	}

	/*
	 * Since there's a Big Kernel Lock around all the libext2fs code, we
	 * only need to start four threads -- one to decode a request, another
	 * to do the filesystem work, a third to transmit the reply, and a
	 * fourth to handle fuse notifications.
	 */
	fuse_loop_cfg_set_clone_fd(loop_config, opts.clone_fd);
	fuse_loop_cfg_set_idle_threads(loop_config, opts.max_idle_threads);
	fuse_loop_cfg_set_max_threads(loop_config, 4);

	if (fuse_session_loop_mt(se, loop_config) != 0) {
		ret = 8;
		goto out_loopcfg;
	}

out_loopcfg:
	fuse_loop_cfg_destroy(loop_config);
out_remove_signal_handlers:
	fuse_remove_signal_handlers(se);
out_unmount:
	fuse_session_unmount(se);
out_destroy_session:
	ff->fuse = NULL;
	fuse_session_destroy(se);
out_free_opts:
	free(opts.mountpoint);
out:
	return ret;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse4fs fctx = {
		.magic = FUSE4FS_MAGIC,
		.opstate = F4OP_WRITABLE,
		.logfd = -1,
#ifdef HAVE_FUSE_IOMAP
		.iomap_want = FT_DEFAULT,
		.iomap_state = IOMAP_UNKNOWN,
		.iomap_dev = FUSE_IOMAP_DEV_NULL,
#endif
#ifdef HAVE_FUSE_LOOPDEV
		.loop_fd = -1,
#endif
		.translate_inums = 1,
	};
	errcode_t err;
	FILE *orig_stderr = stderr;
	char extra_args[BUFSIZ];
	int ret;

	ret = fuse_opt_parse(&args, &fctx, fuse4fs_opts, fuse4fs_opt_proc);
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
	set_com_err_hook(fuse4fs_com_err_proc);

#ifdef ENABLE_NLS
	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");
	bindtextdomain(NLS_CAT_NAME, LOCALEDIR);
	textdomain(NLS_CAT_NAME);
	set_com_err_gettext(gettext);
#endif
	add_error_table(&et_ext2_error_table);

	ret = fuse4fs_setup_logging(&fctx);
	if (ret) {
		/* operational error */
		ret = 2;
		goto out;
	}

#ifdef HAVE_PR_SET_IO_FLUSHER
	/*
	 * Register as a filesystem I/O server process so that our memory
	 * allocations don't cause fs reclaim.
	 */
	ret = prctl(PR_GET_IO_FLUSHER, 0, 0, 0, 0);
	if (ret == 0) {
		ret = prctl(PR_SET_IO_FLUSHER, 1, 0, 0, 0);
		if (ret < 0) {
			err_printf(&fctx, "%s: %s.\n",
 _("Could not register as IO flusher thread"),
					strerror(errno));
			ret = 0;
		}
	}
#endif

	/* Will we allow users to allocate every last block? */
	if (getenv("FUSE4FS_ALLOC_ALL_BLOCKS")) {
		log_printf(&fctx, "%s\n",
 _("Allowing users to allocate all blocks. This is dangerous!"));
		fctx.alloc_all_blocks = 1;
	}

	fuse4fs_discover_iomap(&fctx);
	err = fuse4fs_open(&fctx);
	if (err) {
		ret = 32;
		goto out;
	}

	if (fuse4fs_can_iomap(&fctx)) {
		/*
		 * The root_nodeid mount option was added when iomap support
		 * was added to fuse.  This enables us to control the root
		 * nodeid in the kernel, which enables a 1:1 translation of
		 * ext2 to kernel inumbers.
		 */
		snprintf(extra_args, BUFSIZ, "-oroot_nodeid=%d",
			 EXT2_ROOT_INO);
		fuse_opt_add_arg(&args, extra_args);
		fctx.translate_inums = 0;
	} else if (fctx.iomap_passthrough_options) {
		err_printf(&fctx, "%s\n",
			   _("Some mount options require iomap."));
		ret |= 1;
		goto out;
	}

	if (!fctx.cache_size)
		fctx.cache_size = default_cache_size();
	if (fctx.cache_size) {
		err = fuse4fs_config_cache(&fctx);
		if (err) {
			ret = 32;
			goto out;
		}
	}

	err = fuse4fs_check_support(&fctx);
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

	err = fuse4fs_mount(&fctx);
	if (err) {
		ret = 32;
		goto out;
	}

	/* Initialize generation counter */
	get_random_bytes(&fctx.next_generation, sizeof(unsigned int));

	/* Set up default fuse parameters */
	snprintf(extra_args, BUFSIZ, "-osubtype=%s,fsname=%s",
		 get_subtype(argv[0]),
		 fctx.device);
	if (fctx.no_default_opts == 0)
		fuse_opt_add_arg(&args, extra_args);

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

		printf("FUSE4FS (%s): fuse arguments:", fctx.shortdev);
		for (i = 0; i < args.argc; i++)
			printf(" '%s'", args.argv[i]);
		printf("\n");
		fflush(stdout);
	}

	pthread_mutex_init(&fctx.bfl, NULL);
	ret = fuse4fs_main(&args, &fctx);
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
	fuse4fs_unmount(&fctx);
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
	struct fuse4fs *ff = fs->priv_data;
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
	case EXT2_ET_RO_FILSYS:
		ret = -EROFS;
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
	case EIO:
#ifdef EILSEQ
	case EILSEQ:
#endif
	case EUCLEAN:
		/* these errnos usually denote corruption or persistence fail */
		is_err = 1;
		ret = -err;
		break;
	default:
		if (err < 256) {
			/* other errno are usually operational errors */
			ret = -err;
		} else {
			is_err = 1;
			ret = -EIO;
		}
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
	fuse4fs_get_now(ff, &now);
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

	fs->super->s_state |= EXT2_ERROR_FS;
	fs->super->s_error_count++;
	ext2fs_mark_super_dirty(fs);
	ext2fs_flush(fs);
	switch (ff->errors_behavior) {
	case EXT2_ERRORS_CONTINUE:
		err_printf(ff, "%s\n",
 _("Continuing after errors; is this a good idea?"));
		break;
	case EXT2_ERRORS_RO:
		if (ff->opstate == F4OP_WRITABLE) {
			err_printf(ff, "%s\n",
 _("Remounting read-only due to errors."));
			ff->opstate = F4OP_READONLY;
		}
		fuse4fs_mmp_stop(ff);
		fs->flags &= ~EXT2_FLAG_RW;
		break;
	case EXT2_ERRORS_PANIC:
		err_printf(ff, "%s\n",
 _("Aborting filesystem mount due to errors."));
		abort();
		break;
	}

	return ret;
}
