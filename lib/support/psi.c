/*
 * psi.c - Pressure stall monitor
 *
 * Copyright (C) 2025 Oracle.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */
#include "config.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <malloc.h>
#include <signal.h>
#include <limits.h>

#include "support/list.h"
#include "support/psi.h"

enum psi_state {
	/* waiting to be put in the running state */
	PSI_WAITING,
	/* running */
	PSI_RUNNING,
	/* cancelled */
	PSI_CANCELLED,
};

struct psi_handler {
	struct list_head list;
	psi_handler_fn callback;
	void *data;
};

struct psi {
	int system_fd;
	int cgroup_fd;
	unsigned int flags;
	uint64_t timeout_us;

	pthread_t thread;
	pthread_mutex_t lock;
	struct list_head handlers;

	enum psi_type type;
	enum psi_state state;
};

static const char *psi_system_path(enum psi_type type)
{
	switch (type) {
	case PSI_MEMORY:
		return "/proc/pressure/memory";
	default:
		return NULL;
	}
}

static const char *psi_cgroup_fname(enum psi_type type)
{
	switch (type) {
	case PSI_MEMORY:
		return "memory.pressure";
	default:
		return NULL;
	}
}

static const char *psi_shortname(enum psi_type type)
{
	switch (type) {
	case PSI_MEMORY:
		return "psi:memory";
	default:
		return NULL;
	}
}

static void psi_run_callbacks(struct psi *psi, unsigned int reasons)
{
	struct psi_handler *h, *i;

	pthread_mutex_lock(&psi->lock);
	list_for_each_entry_safe(h, i, &psi->handlers, list)
		h->callback(psi, reasons, h->data);
	pthread_mutex_unlock(&psi->lock);

	if (psi->flags & PSI_TRIM_HEAP)
		malloc_trim(0);
}

static inline void psi_fill_pollfd(struct pollfd *pfd, int fd)
{
	memset(pfd, 0, sizeof(*pfd));
	pfd->fd = fd;
	pfd->events = POLLPRI | POLLRDHUP | POLLERR | POLLHUP;
}

static unsigned int psi_fill_pollfds(struct psi *psi, struct pollfd *pfds)
{
	unsigned int ret = 0;

	if (psi->system_fd >= 0) {
		psi_fill_pollfd(pfds, psi->system_fd);
		pfds++;
		ret++;
	}

	if (psi->cgroup_fd >= 0) {
		psi_fill_pollfd(pfds, psi->cgroup_fd);
		pfds++;
		ret++;
	}

	return ret;
}

static void *psi_thread(void *arg)
{
	struct psi *psi = arg;
	int oldstate;

	/*
	 * Don't let pthread_cancel kill us except while we're in poll()
	 * because we don't hold any resources during that call.  Everywhere
	 * else, there could be resource cleanups that would have to be done.
	 * Hence we just turn off cancelling for simplicity's sake.
	 */
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldstate);

	pthread_mutex_lock(&psi->lock);
	psi->state = PSI_RUNNING;
	pthread_mutex_unlock(&psi->lock);

	while (1) {
		struct pollfd pfds[2];
		unsigned int nr_pfds;
		int timeout_ms;
		int n;

		pthread_mutex_lock(&psi->lock);
		if (psi_thread_cancelled(psi)) {
			pthread_mutex_unlock(&psi->lock);
			break;
		}

		nr_pfds = psi_fill_pollfds(psi, pfds);
		timeout_ms = psi->timeout_us ? psi->timeout_us / 1000 : -1;
		pthread_mutex_unlock(&psi->lock);

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
		n = poll(pfds, nr_pfds, timeout_ms);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
		if (n == 0) {
			/* run callbacks on timeout */
			psi_run_callbacks(psi, PSI_REASON_TIMEOUT);
			continue;
		}
		if (n < 0) {
			perror(psi_shortname(psi->type));
			break;
		}

		/* psi fd closed */
		if ((pfds[0].revents & POLLNVAL) ||
		    (pfds[1].revents & POLLNVAL))
			break;

		if ((pfds[0].revents & POLLERR) ||
		    (pfds[1].revents & POLLERR)) {
			fprintf(stderr, "%s: event source dead?\n",
				psi_shortname(psi->type));
			break;
		}

		/* POLLPRI on a psi fd means we hit the pressure threshold */
		if ((pfds[0].revents & POLLPRI) ||
		    (pfds[1].revents & POLLPRI)) {
			psi_run_callbacks(psi, 0);
			continue;
		}

		fprintf(stderr, "%s: unknown events 0x%x/0x%x, ignoring.\n",
			psi_shortname(psi->type), pfds[0].revents,
			pfds[1].revents);
	}

	pthread_setcancelstate(oldstate, NULL);
	return NULL;
}

/* Call a function whenever there is resource pressure */
int psi_add_handler(struct psi *psi, psi_handler_fn callback, void *data,
		    struct psi_handler **hanp)
{
	struct psi_handler *handler;

	handler = malloc(sizeof(*handler));
	if (!handler)
		return -1;

	INIT_LIST_HEAD(&handler->list);
	handler->callback = callback;
	handler->data = data;

	pthread_mutex_lock(&psi->lock);
	list_add_tail(&handler->list, &psi->handlers);
	pthread_mutex_unlock(&psi->lock);

	*hanp = handler;
	return 0;
}

/* Stop calling this handler when there is resource pressure */
void psi_del_handler(struct psi *psi, struct psi_handler **hanp)
{
	struct psi_handler *handler = *hanp;

	if (handler) {
		pthread_mutex_lock(&psi->lock);
		list_del_init(&handler->list);
		pthread_mutex_unlock(&psi->lock);
		free(handler);
	}

	*hanp = NULL;
}

/* Cancel a running handler. */
void psi_cancel_handler(struct psi *psi, struct psi_handler **hanp)
{
	struct psi_handler *handler = *hanp;

	if (handler) {
		list_del_init(&handler->list);
		free(handler);
	}

	*hanp = NULL;
}

/*
 * Stop monitoring for resource pressure stalls.  The monitor cannot be
 * restarted after this call completes.
 */
void psi_stop_thread(struct psi *psi)
{
	int system_fd;
	int cgroup_fd;
	enum psi_state old_state;

	pthread_mutex_lock(&psi->lock);
	system_fd = psi->system_fd;
	cgroup_fd = psi->cgroup_fd;
	old_state = psi->state;
	psi->system_fd = -1;
	psi->cgroup_fd = -1;
	psi->state = PSI_CANCELLED;
	pthread_mutex_unlock(&psi->lock);

	if (system_fd >= 0)
		close(system_fd);
	if (cgroup_fd >= 0)
		close(cgroup_fd);

	if (old_state == PSI_RUNNING) {
		/* Cancelling the thread interrupts the poll() call */
		pthread_cancel(psi->thread);
		pthread_join(psi->thread, NULL);
	}
}

/* Is this stall monitor active and its thread running? */
bool psi_thread_cancelled(const struct psi *psi)
{
	return !psi || psi->state == PSI_CANCELLED;
}

/* Destroy this resource pressure stall monitor having stopped the thread */
void psi_destroy(struct psi **psip)
{
	struct psi *psi = *psip;

	if (psi) {
		psi_stop_thread(psi);
		pthread_mutex_destroy(&psi->lock);
		free(psi);
	}

	*psip = NULL;
}

static int psi_open_control(const char *path)
{
	return open(path, O_RDWR | O_NONBLOCK);
}

static void psi_open_system_control(struct psi *psi)
{
	/* PSI may not exist, so we don't error if it's not there */
	psi->system_fd = psi_open_control(psi_system_path(psi->type));
}

static ssize_t psi_cgroup_path(enum psi_type type, char *path, size_t pathsize)
{
	char cgpath[PATH_MAX];
	char *p = cgpath;
	ssize_t bytes;
	int pscgroupfd = open("/proc/self/cgroup", O_RDONLY);
	int nr_colons = 0;

	/*
	 * Read the contents of /proc/self/cgroup, which should have the
	 * format:
	 *
	 * <id>:<stuff>:<absolute path under cgroupfs>\n
	 *
	 * We care about the cgroupfs path (column 3) and not the newline.
	 */
	if (pscgroupfd < 0)
		return 0;

	bytes = read(pscgroupfd, cgpath, sizeof(cgpath) - 1);
	close(pscgroupfd);
	if (bytes < 0)
		return 0;
	cgpath[bytes] = 0;

	/*
	 * Find the second colon, turn it into a dot so that we have a relative
	 * path.  sysfs paths can contain colons, so this will always be the
	 * last column... right?
	 */
	while (*p != 0) {
		if (*p == ':')
			nr_colons++;
		if (nr_colons == 2) {
			*p = '.';
			break;
		}

		p++;
	}

	if (nr_colons != 2)
		return 0;

	/* Trim trailing newline, p points to column 3 */
	bytes = strlen(p);
	if (p[bytes - 1] == '\n')
		p[bytes - 1] = 0;

	/* /sys/fs/cgroup/$col3/$psi_cgroup_fname */
	return snprintf(path, pathsize, "/sys/fs/cgroup/%s/%s", p,
			psi_cgroup_fname(type));
}

static void psi_open_cgroup_control(struct psi *psi)
{
	char path[PATH_MAX];
	ssize_t pathlen;

	pathlen = psi_cgroup_path(psi->type, path, sizeof(path));
	if (!pathlen || pathlen >= sizeof(path)) {
		psi->cgroup_fd = -1;
		return;
	}

	/* PSI may not exist, so we don't error if it's not there */
	psi->cgroup_fd = psi_open_control(path);
}

static int psi_config_fd(struct psi *psi, int fd, uint64_t stall_us,
			 uint64_t window_us)
{
	char buf[256];
	size_t bytes;
	ssize_t written;

	if (fd < 0)
		return 0;

	/*
	 * The kernel blindly nulls out the last byte we write into the psi
	 * file, so put a newline at the end because I bet they're only testing
	 * this with bash scripts.
	 */
	bytes = snprintf(buf, sizeof(buf), "some %llu %llu\n",
			 (unsigned long long)stall_us,
			 (unsigned long long)window_us);
	if (bytes > sizeof(buf))
		return -1;

	written = write(fd, buf, bytes);
	if (written >= 0 && written != bytes) {
		written = -1;
		errno = EMSGSIZE;
	}
	if (written < 0) {
		perror(psi_shortname(psi->type));
		return -1;
	}

	return 0;
}

static inline struct psi *psi_alloc(enum psi_type type, unsigned int psi_flags,
				    uint64_t timeout_us)
{
	struct psi *psi = calloc(1, sizeof(*psi));
	if (!psi)
		return NULL;

	psi->type = type;
	psi->flags = psi_flags;
	psi->timeout_us = timeout_us;
	psi->state = PSI_WAITING;
	INIT_LIST_HEAD(&psi->handlers);
	pthread_mutex_init(&psi->lock, NULL);

	return psi;
}

/*
 * Create a pressure stall indicator monitor thread that monitors for
 * resource availability stalls exceeding @stall_us in any @window_us time
 * period and calls any attached handlers.  If @timeout_us is nonzero, the
 * handlers will be called at that interval with PSI_REASON_TIMEOUT.
 *
 * Unprivileged processes are not allowed to set a @window_us that is not a
 * multiple of 2 seconds(!)
 *
 * Returns 0 on success.  On error, returns -1 and sets errno.  errno values
 * are as follows:
 *
 *    ENOENT means the monitor file cannot be found
 *    EACCESS or EPERM mean that monitoring is not available
 *    EINVAL means the window values are not valid
 *    Any other errno is a sign of deeper problems
 */
int psi_create(enum psi_type type, unsigned int psi_flags, uint64_t stall_us,
	       uint64_t window_us, uint64_t timeout_us, struct psi **psip)
{
	struct psi *psi;
	int ret;

	if (psi_flags & ~PSI_FLAGS) {
		errno = EINVAL;
		return -1;
	}

	psi = psi_alloc(type, psi_flags, timeout_us);
	if (!psi)
		return -1;

	psi_open_system_control(psi);
	psi_open_cgroup_control(psi);

	if (psi->system_fd < 0 && psi->cgroup_fd < 0 && !psi->timeout_us) {
		errno = ENOENT;
		goto out_fds;
	}

	ret = psi_config_fd(psi, psi->system_fd, stall_us, window_us);
	if (ret)
		goto out_fds;

	ret = psi_config_fd(psi, psi->cgroup_fd, stall_us, window_us);
	if (ret)
		goto out_fds;

	*psip = psi;
	return 0;

out_fds:
	psi_destroy(&psi);
	return -1;
}

/* Start monitoring for resource pressure stalls */
int psi_start_thread(struct psi *psi)
{
	int error;

	if (psi->state != PSI_WAITING) {
		fprintf(stderr, "%s: psi already torn down\n",
			psi_shortname(psi->type));
		errno = EINVAL;
		return -1;
	}

	error = pthread_create(&psi->thread, NULL, psi_thread, psi);
	if (error) {
		fprintf(stderr, "%s: could not create thread: %s\n",
			psi_shortname(psi->type), strerror(error));
		errno = error;
		return -1;
	}

	pthread_setname_np(psi->thread, psi_shortname(psi->type));
	return 0;
}
