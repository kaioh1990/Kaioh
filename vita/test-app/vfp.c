#include <stdint.h>
#include <psp2/kernel/sysmem.h>

unsigned vfp_get_fpscr(void) {
	unsigned v;
	__asm__ volatile("vmrs %0, fpscr" : "=r"(v));
	return v;
}
// IEEE mode: flush-to-zero (bit 24) and default-NaN (bit 25) off,
// round-to-nearest (bits 22-23 = 0).
void vfp_set_ieee(void) {
	unsigned v = vfp_get_fpscr();
	v &= ~((1u << 24) | (1u << 25) | (3u << 22));
	__asm__ volatile("vmsr fpscr, %0" : : "r"(v));
}
// Largest USER_RW memory block we can get right now, in MB (freed again).
int mem_probe_mb(void) {
	for (int mb = 512; mb >= 8; mb -= 8) {
		SceUID b = sceKernelAllocMemBlock("probe", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, (SceSize)mb << 20, NULL);
		if (b >= 0) { sceKernelFreeMemBlock(b); return mb; }
	}
	return 0;
}
