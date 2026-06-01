/*
 * iocache.c - caching IO manager
 *
 * Copyright (C) 2025-2026 Oracle.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */
#include "config.h"
#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"
#include "ext2fs/ext2fsP.h"
#include "support/iocache.h"

#define IOCACHE_IO_CHANNEL_MAGIC	0x424F5254	/* BORT */

static io_manager iocache_backing_manager;

struct iocache_private_data {
	int			magic;
	io_channel		real;
};

static struct iocache_private_data *IOCACHE(io_channel channel)
{
	return (struct iocache_private_data *)channel->private_data;
}

static errcode_t iocache_read_error(io_channel channel, unsigned long block,
				    int count, void *data, size_t size,
				    int actual_bytes_read, errcode_t error)
{
	io_channel iocache_channel = channel->app_data;

	return iocache_channel->read_error(iocache_channel, block, count, data,
					   size, actual_bytes_read, error);
}

static errcode_t iocache_write_error(io_channel channel, unsigned long block,
				     int count, const void *data, size_t size,
				     int actual_bytes_written,
				     errcode_t error)
{
	io_channel iocache_channel = channel->app_data;

	return iocache_channel->write_error(iocache_channel, block, count, data,
					    size, actual_bytes_written, error);
}

static errcode_t iocache_open(const char *name, int flags, io_channel *channel)
{
	io_channel	io = NULL;
	io_channel	real;
	struct iocache_private_data *data = NULL;
	errcode_t	retval;

	if (!name)
		return EXT2_ET_BAD_DEVICE_NAME;
	if (!iocache_backing_manager)
		return EXT2_ET_INVALID_ARGUMENT;

	retval = iocache_backing_manager->open(name, flags, &real);
	if (retval)
		return retval;

	retval = ext2fs_get_mem(sizeof(struct struct_io_channel), &io);
	if (retval)
		goto out_backing;
	memset(io, 0, sizeof(struct struct_io_channel));
	io->magic = EXT2_ET_MAGIC_IO_CHANNEL;

	retval = ext2fs_get_mem(sizeof(struct iocache_private_data), &data);
	if (retval)
		goto out_channel;
	memset(data, 0, sizeof(struct iocache_private_data));
	data->magic = IOCACHE_IO_CHANNEL_MAGIC;

	io->manager = iocache_io_manager;
	retval = ext2fs_get_mem(strlen(name) + 1, &io->name);
	if (retval)
		goto out_data;

	strcpy(io->name, name);
	io->private_data = data;
	io->block_size = real->block_size;
	io->read_error = 0;
	io->write_error = 0;
	io->refcount = 1;
	io->flags = real->flags;
	data->real = real;
	real->app_data = io;
	real->read_error = iocache_read_error;
	real->write_error = iocache_write_error;

	*channel = io;
	return 0;

out_data:
	ext2fs_free_mem(&data);
out_channel:
	ext2fs_free_mem(&io);
out_backing:
	io_channel_close(real);
	return retval;
}

static errcode_t iocache_close(io_channel channel)
{
	struct iocache_private_data *data = IOCACHE(channel);
	errcode_t	retval = 0;

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	if (--channel->refcount > 0)
		return 0;
	if (data->real)
		retval = io_channel_close(data->real);
	ext2fs_free_mem(&channel->private_data);
	if (channel->name)
		ext2fs_free_mem(&channel->name);
	ext2fs_free_mem(&channel);

	return retval;
}

static errcode_t iocache_set_blksize(io_channel channel, int blksize)
{
	struct iocache_private_data *data = IOCACHE(channel);
	errcode_t retval;

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	retval = io_channel_set_blksize(data->real, blksize);
	if (retval)
		return retval;

	channel->block_size = data->real->block_size;
	return 0;
}

static errcode_t iocache_flush(io_channel channel)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_flush(data->real);
}

static errcode_t iocache_write_byte(io_channel channel, unsigned long offset,
				    int count, const void *buf)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_write_byte(data->real, offset, count, buf);
}

static errcode_t iocache_set_option(io_channel channel, const char *option,
				    const char *arg)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return data->real->manager->set_option(data->real, option, arg);
}

static errcode_t iocache_get_stats(io_channel channel, io_stats *io_stats)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return data->real->manager->get_stats(data->real, io_stats);
}

static errcode_t iocache_read_blk64(io_channel channel,
				    unsigned long long block, int count,
				    void *buf)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_read_blk64(data->real, block, count, buf);
}

static errcode_t iocache_write_blk64(io_channel channel,
				     unsigned long long block, int count,
				     const void *buf)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_write_blk64(data->real, block, count, buf);
}

static errcode_t iocache_read_blk(io_channel channel, unsigned long block,
				  int count, void *buf)
{
	return iocache_read_blk64(channel, block, count, buf);
}

static errcode_t iocache_write_blk(io_channel channel, unsigned long block,
				   int count, const void *buf)
{
	return iocache_write_blk64(channel, block, count, buf);
}

static errcode_t iocache_discard(io_channel channel, unsigned long long block,
				 unsigned long long count)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_discard(data->real, block, count);
}

static errcode_t iocache_cache_readahead(io_channel channel,
					 unsigned long long block,
					 unsigned long long count)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_cache_readahead(data->real, block, count);
}

static errcode_t iocache_zeroout(io_channel channel, unsigned long long block,
				 unsigned long long count)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_zeroout(data->real, block, count);
}

static errcode_t iocache_get_fd(io_channel channel, int *fd)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_get_fd(data->real, fd);
}

static errcode_t iocache_flock(io_channel channel, unsigned int flock_flags)
{
	struct iocache_private_data *data = IOCACHE(channel);

	EXT2_CHECK_MAGIC(channel, EXT2_ET_MAGIC_IO_CHANNEL);
	EXT2_CHECK_MAGIC(data, IOCACHE_IO_CHANNEL_MAGIC);

	return io_channel_flock(data->real, flock_flags);
}

static struct struct_io_manager struct_iocache_manager = {
	.magic			= EXT2_ET_MAGIC_IO_MANAGER,
	.name			= "iocache I/O manager",
	.open			= iocache_open,
	.close			= iocache_close,
	.set_blksize		= iocache_set_blksize,
	.read_blk		= iocache_read_blk,
	.write_blk		= iocache_write_blk,
	.flush			= iocache_flush,
	.write_byte		= iocache_write_byte,
	.set_option		= iocache_set_option,
	.get_stats		= iocache_get_stats,
	.read_blk64		= iocache_read_blk64,
	.write_blk64		= iocache_write_blk64,
	.discard		= iocache_discard,
	.cache_readahead	= iocache_cache_readahead,
	.zeroout		= iocache_zeroout,
	.get_fd			= iocache_get_fd,
	.flock			= iocache_flock,
};

io_manager iocache_io_manager = &struct_iocache_manager;

errcode_t iocache_set_backing_manager(io_manager manager)
{
	iocache_backing_manager = manager;
	return 0;
}
