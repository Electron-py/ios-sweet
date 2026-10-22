/*
 * Just enough flattened-device-tree parsing for the shim.
 *
 * The FDT may live in Device memory (fleur LK passes it at 0x4bcb0000), so
 * every access is a volatile aligned 32-bit load or a byte copy.  No libc.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

#define FDT_MAX_TOTAL	(64U << 20)
#define FDT_MAX_DEPTH	32
#define FDT_MAX_PATH	256

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static uint64_t be64(uintptr_t a)
{
	return (uint64_t)be32(a) << 32 | be32(a + 4);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

/* Bounded string compare against a string inside the FDT strings block. */
static int fdt_streq(uintptr_t a, uintptr_t end, const char *b)
{
	while (a < end && *(volatile char *)a && *(volatile char *)a == *b)
		a++, b++;
	return a < end && *(volatile char *)a == *b;
}

static int fdt_init(uintptr_t fdt, uintptr_t *structp, uintptr_t *strp,
		    uintptr_t *endp)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uint32_t total = be32(fdt + 4);
	uint32_t off_struct = be32(fdt + 8);
	uint32_t off_strings = be32(fdt + 12);

	if (total < 40 || total > FDT_MAX_TOTAL)
		return 0;
	if (off_struct >= total || off_strings >= total)
		return 0;
	*structp = fdt + off_struct;
	*strp = fdt + off_strings;
	*endp = fdt + total;
	return 1;
}

/*
 * Find one property.  Returns 1 with the value pointer and length.  A node
 * path is matched exactly; the root is "/".  Once the target node ends, a
 * missing property is final (a same-named property in a child is ignored).
 */
static int fdt_find(uintptr_t fdt, const char *path, const char *prop,
		    uintptr_t *valp, uint32_t *lenp)
{
	uintptr_t p, strings, end;
	if (!fdt_init(fdt, &p, &strings, &end))
		return 0;

	char cur[FDT_MAX_PATH];
	int curlen = 0, depth = 0, target_depth = -1;
	int lens[FDT_MAX_DEPTH];
	cur[0] = 0;

	for (;;) {
		if (p + 4 > end)
			return 0;
		uint32_t tok = be32(p);
		p += 4;

		switch (tok) {
		case FDT_BEGIN_NODE: {
			uintptr_t name = p;
			uintptr_t q = name;
			while (q < end && *(volatile char *)q)
				q++;
			if (q >= end)
				return 0;
			uint32_t nlen = (uint32_t)(q - name);
			p = align4(q + 1);
			if (p > end)
				return 0;

			if (depth < FDT_MAX_DEPTH)
				lens[depth] = curlen;
			depth++;

			if (depth == 1) {
				curlen = 0;
				cur[curlen++] = '/';
				cur[curlen] = 0;
			} else {
				if (curlen && cur[curlen - 1] != '/' &&
				    curlen < FDT_MAX_PATH - 1)
					cur[curlen++] = '/';
				for (uint32_t i = 0; i < nlen &&
				     curlen < FDT_MAX_PATH - 1; i++)
					cur[curlen++] = *(volatile char *)(name + i);
				cur[curlen] = 0;
			}

			if (target_depth < 0 && streq(cur, path))
				target_depth = depth;
			break;
		}

		case FDT_END_NODE:
			if (target_depth == depth && streq(cur, path))
				return 0;	/* target ended without the prop */
			if (depth > 0) {
				depth--;
				if (depth < FDT_MAX_DEPTH)
					curlen = lens[depth];
				cur[curlen] = 0;
			}
			break;

		case FDT_PROP: {
			if (p + 8 > end)
				return 0;
			uint32_t len = be32(p);
			uint32_t nameoff = be32(p + 4);
			uintptr_t val = p + 8;

			if (len > end - val)
				return 0;
			p = align4(val + len);
			if (p > end)
				return 0;

			if (target_depth == depth && streq(cur, path) &&
			    fdt_streq(strings + nameoff, end, prop)) {
				*valp = val;
				*lenp = len;
				return 1;
			}
			break;
		}

		case FDT_NOP:
			break;

		case FDT_END:
		default:
			return 0;
		}
	}
}

int fdt_get(uintptr_t fdt, const char *path, const char *prop,
	    void *buf, uint32_t buflen, uint32_t *outlen)
{
	uintptr_t val;
	uint32_t len;

	if (!fdt_find(fdt, path, prop, &val, &len))
		return 0;
	if (outlen)
		*outlen = len;
	if (buf) {
		uint32_t n = len < buflen ? len : buflen;
		for (uint32_t i = 0; i < n; i++)
			((uint8_t *)buf)[i] = *(volatile uint8_t *)(val + i);
	}
	return 1;
}

int fdt_get_u64(uintptr_t fdt, const char *path, const char *prop, uint64_t *out)
{
	uintptr_t val;
	uint32_t len;

	if (!fdt_find(fdt, path, prop, &val, &len))
		return 0;
	if (len == 4)
		*out = be32(val);
	else if (len >= 8)
		*out = be64(val);
	else
		return 0;
	return 1;
}

const char *fdt_get_str(uintptr_t fdt, const char *path, const char *prop)
{
	uintptr_t val;
	uint32_t len;

	if (!fdt_find(fdt, path, prop, &val, &len) || len == 0)
		return 0;
	if (*(volatile char *)(val + len - 1) != 0)
		return 0;
	return (const char *)val;
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = depth == 2 && streq(name, "chosen");
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = (uint64_t)be32(val) << 32 | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}
