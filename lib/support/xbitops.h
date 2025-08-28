// SPDX-License-Identifier: GPL-2.0
#ifndef __BITOPS_H__
#define __BITOPS_H__

/*
 * fls: find last bit set.
 */

static inline int fls(int x)
{
	int r = 32;

	if (!x)
		return 0;
	if (!(x & 0xffff0000u)) {
		x = (x & 0xffffu) << 16;
		r -= 16;
	}
	if (!(x & 0xff000000u)) {
		x = (x & 0xffffffu) << 8;
		r -= 8;
	}
	if (!(x & 0xf0000000u)) {
		x = (x & 0xfffffffu) << 4;
		r -= 4;
	}
	if (!(x & 0xc0000000u)) {
		x = (x & 0x3fffffffu) << 2;
		r -= 2;
	}
	if (!(x & 0x80000000u)) {
		r -= 1;
	}
	return r;
}

static inline int fls64(uint64_t x)
{
	uint32_t h = x >> 32;
	if (h)
		return fls(h) + 32;
	return fls(x);
}

static inline unsigned fls_long(unsigned long l)
{
        if (sizeof(l) == 4)
                return fls(l);
        return fls64(l);
}

/*
 * ffz: find first zero bit.
 * Result is undefined if no zero bit exists.
 */
#define ffz(x)	ffs(~(x))

/*
 * XFS bit manipulation routines.  Repeated here so that some programs
 * don't have to link in all of libxfs just to have bit manipulation.
 */

/*
 * masks with n high/low bits set, 64-bit values
 */
static inline uint64_t mask64hi(int n)
{
	return (uint64_t)-1 << (64 - (n));
}
static inline uint32_t mask32lo(int n)
{
	return ((uint32_t)1 << (n)) - 1;
}
static inline uint64_t mask64lo(int n)
{
	return ((uint64_t)1 << (n)) - 1;
}

/* Get high bit set out of 32-bit argument, -1 if none set */
static inline int highbit32(uint32_t v)
{
	return fls(v) - 1;
}

/* Get high bit set out of 64-bit argument, -1 if none set */
static inline int highbit64(uint64_t v)
{
	return fls64(v) - 1;
}

/* Get low bit set out of 32-bit argument, -1 if none set */
static inline int lowbit32(uint32_t v)
{
	return ffs(v) - 1;
}

/* Get low bit set out of 64-bit argument, -1 if none set */
static inline int lowbit64(uint64_t v)
{
	uint32_t	w = (uint32_t)v;
	int		n = 0;

	if (w) {	/* lower bits */
		n = ffs(w);
	} else {	/* upper bits */
		w = (uint32_t)(v >> 32);
		if (w) {
			n = ffs(w);
			if (n)
				n += 32;
		}
	}
	return n - 1;
}

/**
 * __rounddown_pow_of_two() - round down to nearest power of two
 * @n: value to round down
 */
static inline __attribute__((const))
unsigned long __rounddown_pow_of_two(unsigned long n)
{
	return 1UL << (fls_long(n) - 1);
}

#define rounddown_pow_of_two(n) __rounddown_pow_of_two(n)

#endif
