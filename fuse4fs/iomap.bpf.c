// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 * Copied from: Joanne Koong <joannelkoong@gmail.com>
 */
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include <fuse_iomap_bpf.h>

DECLARE_GPL2_LICENSE_FOR_FUSE_IOMAP_BPF;

/* XXX why are we redefining these? */
#define ENOSYS 38

FUSE_IOMAP_BEGIN_BPF_FUNC(fuse4fs_iomap_begin_bpf)
{
	/*
	 * Create an alternating pattern of written and unwritten mappings
	 * for FIEMAP as a demonstration of using BPF for iomapping.  Do NOT
	 * run this in production!
	 */
	if ((opflags & FUSE_IOMAP_OP_REPORT) && pos <= 65536) {
		outarg->read.offset = pos;
		outarg->read.length = 4096;
		outarg->read.type = ((pos/4096) % 2) + FUSE_IOMAP_TYPE_MAPPED;
		outarg->read.dev = 1;
		outarg->read.addr = 405504 + pos;

		fuse_iomap_begin_pure_overwrite(outarg);
		return 0;
	}

	return -ENOSYS;
}

DEFINE_FUSE_IOMAP_BPF_OPS(fuse4fs_iomap_bpf_ops, "fuse4fs_bpf",
		fuse4fs_iomap_begin_bpf, NULL, NULL);
