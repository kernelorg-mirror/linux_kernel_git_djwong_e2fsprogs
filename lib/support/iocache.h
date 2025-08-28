/*
 * iocache.h - IO cache
 *
 * Copyright (C) 2025 Oracle.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */
#ifndef __IOCACHE_H__
#define __IOCACHE_H__

errcode_t iocache_set_backing_manager(io_manager manager);
extern io_manager iocache_io_manager;

#endif /* __IOCACHE_H__ */
