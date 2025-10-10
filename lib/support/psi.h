/*
 * psi.h - Pressure stall monitor
 *
 * Copyright (C) 2025 Oracle.
 *
 * %Begin-Header%
 * This file may be redistributed under the terms of the GNU Public
 * License.
 * %End-Header%
 */
#ifndef __PSI_H__
#define __PSI_H__

struct psi;
struct psi_handler;

enum psi_type {
	PSI_MEMORY,
};

void psi_destroy(struct psi **psip);

/* call malloc_trim after calling handlers */
#define PSI_TRIM_HEAP		(1U << 0)

#define PSI_FLAGS		(PSI_TRIM_HEAP)

int psi_create(enum psi_type type, unsigned int psi_flags,
	       uint64_t stall_us, uint64_t window_us, uint64_t timeout_us,
	       struct psi **psip);

/* psi triggered due to timeout (and not pressure) */
#define PSI_REASON_TIMEOUT	(1U << 0)

/*
 * Prototype of a function to call when a stall occurs.  Implementations must
 * not block on any resources that are held if psi_stop_thread is called.
 */
typedef void (*psi_handler_fn)(const struct psi *psi, unsigned int reasons,
			       void *data);

int psi_add_handler(struct psi *psi, psi_handler_fn callback, void *data,
		    struct psi_handler **hanp);
void psi_del_handler(struct psi *psi, struct psi_handler **hanp);

int psi_start_thread(struct psi *psi);
void psi_stop_thread(struct psi *psi);

bool psi_thread_running(const struct psi *psi);

static inline bool psi_active(struct psi *psi)
{
	return psi != NULL;
}

char *psi_system_path(enum psi_type type);
ssize_t psi_cgroup_path(enum psi_type type, char *path, size_t pathsize);

#define PSI_OPEN_FLAGS (O_RDWR | O_NONBLOCK)

int psi_create_from(enum psi_type type, unsigned int psi_flags,
		    uint64_t stall_us, uint64_t window_us, uint64_t timeout_us,
		    int *system_fd, int *cgroup_fd, struct psi **psip);

#endif /* __PSI_H__ */
