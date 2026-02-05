// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __FUSE4FS_BPF_H__
#define __FUSE4FS_BPF_H__

struct fuse4fs_bpf_attrs {
	/* vector to the binary elf data */
	const void *elf_data;
	size_t elf_size;

	/* name associated with this bpf skeleton */
	const char *skel_name;

	/* name of the fuse_iomap_bpf_ops object in the bpf code */
	const char *ops_name;

	/* name of the iomap_begin bpf function, if provided */
	const char *begin_fn_name;

	/* name of the iomap_end bpf function, if provided */
	const char *end_fn_name;

	/* name of the iomap_ioend bpf function, if provided */
	const char *ioend_fn_name;
};

struct fuse4fs_bpf_compile {
	/* C source code */
	const char *source_code;

	/* directory containing vmlinux.h */
	const char *vmlinux_h_dir;

	/* directory containing fuse_iomap_bpf.h */
	const char *fuse_include_dir;
};

struct fuse4fs_bpf_ctl {
	struct fuse4fs_bpf *skel;
	struct bpf_link *link;
};

int fuse4fs_bpf_ctl_setup(struct fuse4fs_bpf_ctl *arg, struct fuse_session *se,
			  const struct fuse4fs_bpf_attrs *attrs);
void fuse4fs_bpf_ctl_cleanup(struct fuse4fs_bpf_ctl *arg);

int fuse4fs_bpf_compile(struct fuse4fs_bpf_attrs *attrs,
			const struct fuse4fs_bpf_compile *cc);

#endif /* __FUSE4FS_BPF_H__ */
