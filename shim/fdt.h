#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/*
 * Look up a property by absolute node path ("/" for the root, "/memory",
 * "/ufshci@11270000", ...).  `buf` may be NULL when only the length is wanted.
 * Returns 1 and sets *outlen when found.
 */
int fdt_get(uintptr_t fdt, const char *path, const char *prop,
	    void *buf, uint32_t buflen, uint32_t *outlen);

/* Convenience wrappers for 32/64-bit integer and string properties. */
int fdt_get_u64(uintptr_t fdt, const char *path, const char *prop, uint64_t *out);
const char *fdt_get_str(uintptr_t fdt, const char *path, const char *prop);
