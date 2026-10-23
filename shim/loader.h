#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* True when /chosen points at a SWEETXNU payload (used to pick the boot path). */
int loader_payload_present(uintptr_t dtb);
