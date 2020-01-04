#include <debug.h>
#include <lib/iter.h>
#include <memory.h>
#include <thread.h>
static DECLARE_LIST(physical_regions);
static DECLARE_LIST(physical_regions_alloc);

static const char *memory_type_strings[] = {
	[MEMORY_AVAILABLE] = "System RAM",
	[MEMORY_RESERVED] = "Reserved",
	[MEMORY_CODE] = "Firmware Code",
	[MEMORY_BAD] = "Bad Memory",
	[MEMORY_RECLAIMABLE] = "Reclaimable System Memory",
	[MEMORY_UNKNOWN] = "Unknown Memory",
};

static const char *memory_subtype_strings[] = {
	[MEMORY_SUBTYPE_NONE] = "",
	[MEMORY_AVAILABLE_VOLATILE] = "(volatile)",
	[MEMORY_AVAILABLE_PERSISTENT] = "(persistent)",
};

void mm_register_region(struct memregion *reg, struct mem_allocator *alloc)
{
	if(reg->length < mm_page_size(1))
		return;
	reg->start += mm_page_size(1) - (reg->start % (mm_page_size(1)));
	reg->length -= mm_page_size(1) - (reg->start % (mm_page_size(1)));
	if(reg->length < mm_page_size(1))
		return;
	reg->alloc = alloc;
	printk("[mm] registering memory region %lx -> %lx %s %s\n",
	  reg->start,
	  reg->start + reg->length - 1,
	  memory_type_strings[reg->type],
	  memory_subtype_strings[reg->subtype]);

	if(alloc && reg->start < 0x100000000ull) {
		pmm_buddy_init(reg);
		list_insert(&physical_regions_alloc, &reg->alloc_entry);
	}
	list_insert(&physical_regions, &reg->entry);
}

void mm_init_region(struct memregion *reg,
  uintptr_t start,
  size_t len,
  enum memory_type type,
  enum memory_subtype st)
{
	reg->start = start;
	reg->length = len;
	reg->flags = 0;
	reg->alloc = NULL;
	reg->type = type;
	reg->subtype = st;
	reg->off = 0;
}

void mm_init(void)
{
	arch_mm_init();
#if 0
	arch_mm_get_regions(&physical_regions);
	foreach(e, list, &physical_regions) {
		struct memregion *reg = list_entry(e, struct memregion, entry);
		pmm_buddy_init(reg);

		printk("[mm]: memory region %lx -> %lx (%ld MB), %x\n",
		  reg->start,
		  reg->start + reg->length,
		  reg->length / (1024 * 1024),
		  reg->flags);

		for(uintptr_t addr = reg->start; addr < reg->start + reg->length;
		    addr += MM_BUDDY_MIN_SIZE) {
			pmm_buddy_deallocate(reg, addr);
		}
		reg->ready = true;
	}
#endif
}

struct memregion *mm_physical_find_region(uintptr_t addr)
{
	foreach(e, list, &physical_regions) {
		struct memregion *reg = list_entry(e, struct memregion, entry);
		if(addr >= reg->start && addr < reg->start + reg->length)
			return reg;
	}
	return NULL;
}

uintptr_t mm_physical_alloc(size_t length, int type, bool clear)
{
	static struct spinlock nvs = SPINLOCK_INIT;
	if(type == PM_TYPE_NV) {
		/* TODO: clean this up */
		// printk("ALloc NVM\n");
		foreach(e, list, &physical_regions) {
			struct memregion *reg = list_entry(e, struct memregion, entry);

			//	printk(":: %d %d\n", reg->type, reg->subtype);
			if(reg->type == MEMORY_AVAILABLE && reg->subtype == MEMORY_AVAILABLE_PERSISTENT) {
				spinlock_acquire_save(&nvs);
				size_t a = length - ((reg->start + reg->off) % (length - 1));
				reg->off += a;
				size_t x = reg->off;
				reg->off += length;
				spinlock_release_restore(&nvs);
				return x + reg->start;
			}
		}
		/* TODO: */
		// printk("warning - allocating volatile RAM when NVM was requested\n");
	}
	foreach(e, list, &physical_regions_alloc) {
		struct memregion *reg = list_entry(e, struct memregion, alloc_entry);

		if((reg->flags & type) == reg->flags && reg->alloc && reg->alloc->ready
		   && reg->alloc->free_memory > length) {
			uintptr_t alloc = mm_physical_region_alloc(reg, length, clear);
			printk("alloc: %lx\n", alloc);
			if(alloc)
				return alloc;
		}
	}
	return 0;
}

void mm_physical_dealloc(uintptr_t addr)
{
	struct memregion *reg = mm_physical_find_region(addr);
	assert(reg != NULL);

	mm_physical_region_dealloc(reg, addr);
}

void kernel_fault_entry(uintptr_t ip, uintptr_t addr, int flags)
{
	if(addr < KERNEL_VIRTUAL_BASE) {
		vm_context_fault(ip, addr, flags);
	} else {
		panic("kernel page fault: %lx, %x at ip=%lx", addr, flags, ip);
	}
}
