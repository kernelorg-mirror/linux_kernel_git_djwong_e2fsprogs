// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "config.h"
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <bpf/libbpf.h>
#include <fuse_lowlevel.h>

#include "fuse4fs_bpf.h"

#define max(a, b)	((a) > (b) ? (a) : (b))
#define BPF_SKEL_SUPPORTS_MAP_AUTO_ATTACH 1

struct fuse4fs_bpf {
	struct bpf_object_skeleton *skeleton;
	struct bpf_object *obj;
	struct {
		struct bpf_map *fuse4fs_bpf_ops;
	} maps;
	struct {
		struct fuse4fs_bpf__fuse4fs_bpf_ops__fuse_iomap_bpf_ops {
			struct bpf_program *iomap_begin;
			struct bpf_program *iomap_end;
			struct bpf_program *iomap_ioend;
			int fuse_fd;
			unsigned int zeropad;
			char __unsupported_5[16];
		} *fuse4fs_bpf_ops;
	} struct_ops;
	struct {
		struct bpf_program *crazy_begin_bpf;
		struct bpf_program *crazy_end_bpf;
		struct bpf_program *crazy_ioend_bpf;
	} progs;
	struct {
		struct bpf_link *crazy_begin_bpf;
		struct bpf_link *crazy_end_bpf;
		struct bpf_link *crazy_ioend_bpf;
		struct bpf_link *fuse4fs_bpf_ops;
	} links;
};

static void
fuse4fs_bpf__destroy(struct fuse4fs_bpf *obj)
{
	if (!obj)
		return;
	if (obj->skeleton)
		bpf_object__destroy_skeleton(obj->skeleton);
	free(obj);
}

static inline size_t
fuse4fs_bpf_nr_progs(const struct fuse4fs_bpf_attrs *attrs)
{
	size_t ret = 0;

	if (attrs->begin_fn_name)
		ret++;
	if (attrs->end_fn_name)
		ret++;
	if (attrs->ioend_fn_name)
		ret++;
	return ret;
}

static int
fuse4fs_bpf__create_skeleton(struct fuse4fs_bpf *obj,
			   const struct fuse4fs_bpf_attrs *attrs)
{
	struct bpf_object_skeleton *s;
	struct bpf_map_skeleton *map __attribute__((unused));
	int err;

	s = (struct bpf_object_skeleton *)calloc(1, sizeof(*s));
	if (!s)	{
		err = -ENOMEM;
		goto err;
	}

	s->sz = sizeof(*s);
	s->name = attrs->skel_name;
	s->obj = &obj->obj;

	/* maps */
	s->map_cnt = 1;
	s->map_skel_sz = 32;
	s->maps = (struct bpf_map_skeleton *)calloc(s->map_cnt,
			sizeof(*s->maps) > 32 ? sizeof(*s->maps) : 32);
	if (!s->maps) {
		err = -ENOMEM;
		goto err;
	}

	map = (struct bpf_map_skeleton *)((char *)s->maps + 0 * s->map_skel_sz);
	map->name = attrs->ops_name;
	map->map = &obj->maps.fuse4fs_bpf_ops;
	map->link = &obj->links.fuse4fs_bpf_ops;

	/* programs */
	s->prog_cnt = 0;
	s->prog_skel_sz = sizeof(*s->progs);
	s->progs = calloc(fuse4fs_bpf_nr_progs(attrs), s->prog_skel_sz);
	if (!s->progs) {
		err = -ENOMEM;
		goto err;
	}

	if (attrs->begin_fn_name) {
		s->progs[s->prog_cnt].name = attrs->begin_fn_name;
		s->progs[s->prog_cnt].prog = &obj->progs.crazy_begin_bpf;
		s->progs[s->prog_cnt].link = &obj->links.crazy_begin_bpf;
		s->prog_cnt++;
	}

	if (attrs->end_fn_name) {
		s->progs[s->prog_cnt].name = attrs->end_fn_name;
		s->progs[s->prog_cnt].prog = &obj->progs.crazy_end_bpf;
		s->progs[s->prog_cnt].link = &obj->links.crazy_end_bpf;
		s->prog_cnt++;
	}

	if (attrs->ioend_fn_name) {
		s->progs[s->prog_cnt].name = attrs->ioend_fn_name;
		s->progs[s->prog_cnt].prog = &obj->progs.crazy_ioend_bpf;
		s->progs[s->prog_cnt].link = &obj->links.crazy_ioend_bpf;
		s->prog_cnt++;
	}

	s->data = attrs->elf_data;
	s->data_sz = attrs->elf_size;

	obj->skeleton = s;
	return 0;
err:
	bpf_object__destroy_skeleton(s);
	return err;
}

static struct fuse4fs_bpf *
fuse4fs_bpf__open_opts(const struct bpf_object_open_opts *opts,
		     const struct fuse4fs_bpf_attrs *attrs)
{
	struct fuse4fs_bpf *obj;
	int err;

	obj = (struct fuse4fs_bpf *)calloc(1, sizeof(*obj));
	if (!obj) {
		errno = ENOMEM;
		return NULL;
	}

	err = fuse4fs_bpf__create_skeleton(obj, attrs);
	if (err)
		goto err_out;

	err = bpf_object__open_skeleton(obj->skeleton, opts);
	if (err)
		goto err_out;

	obj->struct_ops.fuse4fs_bpf_ops = (__typeof__(obj->struct_ops.fuse4fs_bpf_ops))
		bpf_map__initial_value(obj->maps.fuse4fs_bpf_ops, NULL);

	return obj;
err_out:
	fuse4fs_bpf__destroy(obj);
	errno = -err;
	return NULL;
}

static inline int
fuse4fs_bpf__load(struct fuse4fs_bpf *obj)
{
	return bpf_object__load_skeleton(obj->skeleton);
}

static inline int
fuse4fs_bpf__attach(struct fuse4fs_bpf *obj)
{
	return bpf_object__attach_skeleton(obj->skeleton);
}

static inline void
fuse4fs_bpf__detach(struct fuse4fs_bpf *obj)
{
	bpf_object__detach_skeleton(obj->skeleton);
}

int
fuse4fs_bpf_ctl_setup(struct fuse4fs_bpf_ctl *arg, struct fuse_session *se,
		      const struct fuse4fs_bpf_attrs *attrs)
{
	int err;

	arg->skel = fuse4fs_bpf__open_opts(NULL, attrs);
	if (!arg->skel)
		return -ENOENT;

	arg->skel->struct_ops.fuse4fs_bpf_ops->fuse_fd = fuse_session_fd(se);

	err = fuse4fs_bpf__load(arg->skel);
	if (err) {
		err = -EINVAL;
		goto cleanup;
	}

	arg->link = bpf_map__attach_struct_ops(arg->skel->maps.fuse4fs_bpf_ops);
	if (!arg->link) {
		err = -errno;
		goto cleanup;
	}

	return 0;

cleanup:
	fuse4fs_bpf__destroy(arg->skel);
	arg->skel = NULL;
	return err;
}

void
fuse4fs_bpf_ctl_cleanup(struct fuse4fs_bpf_ctl *arg)
{
	if (arg->link) {
		bpf_link__destroy(arg->link);
		arg->link = NULL;
	}

	if (arg->skel) {
		fuse4fs_bpf__destroy(arg->skel);
		arg->skel = NULL;
	}
}

int
fuse4fs_bpf_compile(struct fuse4fs_bpf_attrs *attrs,
		    const struct fuse4fs_bpf_compile *cc)
{
	char infile[64];
	char outfile[64];
	// clang --target=bpf -Wall -O2 -g -x c -c /dev/fd/37 -o /dev/fd/38
	// -I /usr/include/x86_64-linux-gnu/linux/bpf/ -I ../../include/
	char *args[] = {
		"clang", "--target=bpf", "-Wall", "-O2", "-g", "-x", "c",
		"-c", infile, "-o", outfile,
		"-I", (char *)cc->vmlinux_h_dir,
		"-I", (char *)cc->fuse_include_dir,
		NULL
	};
	struct stat statbuf;
	void *buf, *read_ptr;
	const void *write_ptr;
	size_t to_read, to_write;
	pid_t child;
	int child_status;
	int infd;
	int outfd;
	int ret = 0;

	infd = memfd_create("infile", 0);
	if (infd < 0)
		return -1;

	outfd = memfd_create("outfile", 0);
	if (outfd < 0) {
		ret = -1;
		goto out_infd;
	}

	snprintf(infile, sizeof(infile), "/dev/fd/%d", infd);
	snprintf(outfile, sizeof(outfile), "/dev/fd/%d", outfd);

	write_ptr = cc->source_code;
	to_write = strlen(cc->source_code);
	while (to_write > 0) {
		ssize_t bytes_written = write(infd, write_ptr, to_write);
		if (bytes_written < 0) {
			ret = -1;
			goto out_outfd;
		}
		if (bytes_written == 0) {
			errno = -EIO;
			ret = -1;
			goto out_outfd;
		}

		write_ptr += bytes_written;
		to_write -= bytes_written;
	}

	child = fork();
	switch (child) {
	case -1:
		ret = -1;
		goto out_outfd;
	case 0:
		close_range(max(infd, outfd) + 1, ~0U, 0);
		execvp(args[0], args);
		perror(args[0]);
		exit(EXIT_FAILURE);
		break;
	default:
		waitpid(child, &child_status, 0);
		break;
	}

	if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
		goto out_outfd;

	ret = fstat(outfd, &statbuf);
	if (ret)
		goto out_outfd;

	if (statbuf.st_size >= SIZE_MAX) {
		errno = -EFBIG;
		ret = -1;
		goto out_outfd;
	}
	to_read = statbuf.st_size;

	buf = malloc(to_read);
	if (!buf)
		goto out_outfd;
	read_ptr = buf;

	while (to_read > 0) {
		ssize_t bytes_read = read(outfd, read_ptr, to_read);
		if (bytes_read < 0) {
			ret = -1;
			goto out_buf;
		}
		if (bytes_read == 0) {
			errno = -EIO;
			ret = -1;
			goto out_buf;
		}

		read_ptr += bytes_read;
		to_read -= bytes_read;
	}

	attrs->elf_data = buf;
	attrs->elf_size = statbuf.st_size;
	buf = NULL;
	ret = 0;

out_buf:
	free(buf);
out_outfd:
	close(outfd);
out_infd:
	close(infd);
	return ret;
}
