/*
 * Based on arch/arm/mm/mmu.c
 *
 * Copyright (C) 1995-2005 Russell King
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (c) 2017, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/cache.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/memremap.h>
#include <linux/mman.h>
#include <linux/nodemask.h>
#include <linux/memblock.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>
#include <linux/cma.h>
#include <linux/mm.h>

#include <asm/barrier.h>
#include <asm/cputype.h>
#include <asm/fixmap.h>
#include <asm/kasan.h>
#include <asm/kernel-pgtable.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/sizes.h>
#include <asm/tlb.h>
#include <asm/memblock.h>
#include <asm/mmu_context.h>

u64 idmap_t0sz = TCR_T0SZ(VA_BITS);

u64 kimage_voffset __ro_after_init;
EXPORT_SYMBOL(kimage_voffset);

/*
 * Empty_zero_page is a special page that is used for zero-initialized data
 * and COW.
 */
unsigned long empty_zero_page[PAGE_SIZE / sizeof(unsigned long)] __page_aligned_bss;
EXPORT_SYMBOL(empty_zero_page);

static pte_t bm_pte[PTRS_PER_PTE] __page_aligned_bss;
static pmd_t bm_pmd[PTRS_PER_PMD] __page_aligned_bss __maybe_unused;
static pud_t bm_pud[PTRS_PER_PUD] __page_aligned_bss __maybe_unused;

pgprot_t phys_mem_access_prot(struct file *file, unsigned long pfn,
			      unsigned long size, pgprot_t vma_prot)
{
	if (!pfn_valid(pfn))
		return pgprot_noncached(vma_prot);
	else if (file->f_flags & O_SYNC)
		return pgprot_writecombine(vma_prot);
	return vma_prot;
}
EXPORT_SYMBOL(phys_mem_access_prot);

static phys_addr_t __init early_pgtable_alloc(void)
{
	phys_addr_t phys;
	void *ptr;

	phys = memblock_alloc(PAGE_SIZE, PAGE_SIZE);

	/*
	 * The FIX_{PGD,PUD,PMD} slots may be in active use, but the FIX_PTE
	 * slot will be free, so we can (ab)use the FIX_PTE slot to initialise
	 * any level of table.
	 */
	ptr = pte_set_fixmap(phys);

	memset(ptr, 0, PAGE_SIZE);

	/*
	 * Implicit barriers also ensure the zeroed page is visible to the page
	 * table walker
	 */
	pte_clear_fixmap();

	return phys;
}

static void split_pmd(pmd_t *pmd, pte_t *pte)
{
	unsigned long pfn = pmd_pfn(*pmd);
	int i = 0;

	do {
		/*
		 * Need to have the least restrictive permissions available
		 * permissions will be fixed up later
		 */
		set_pte(pte, pfn_pte(pfn, PAGE_KERNEL_EXEC));
		pfn++;
	} while (pte++, i++, i < PTRS_PER_PTE);
}

static void alloc_init_pte(pmd_t *pmd, unsigned long addr,
				  unsigned long end, unsigned long pfn,
				  pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void))
{
	pte_t *pte;

	if (pmd_none(*pmd) || pmd_sect(*pmd)) {
		phys_addr_t pte_phys;
		BUG_ON(!pgtable_alloc);
		pte_phys = pgtable_alloc();
		pte = pte_set_fixmap(pte_phys);
		if (pmd_sect(*pmd)) {
			split_pmd(pmd, pte);
		}
		__pmd_populate(pmd, pte_phys, PMD_TYPE_TABLE);
		flush_tlb_all();
		pte_clear_fixmap();
	}
	BUG_ON(pmd_bad(*pmd));

	pte = pte_set_fixmap_offset(pmd, addr);
	do {
		set_pte(pte, pfn_pte(pfn, prot));
		pfn++;
	} while (pte++, addr += PAGE_SIZE, addr != end);

	pte_clear_fixmap();
}

static void split_pud(pud_t *old_pud, pmd_t *pmd)
{
	unsigned long addr = pud_pfn(*old_pud) << PAGE_SHIFT;
	pgprot_t prot = __pgprot(pud_val(*old_pud) ^ addr);
	int i = 0;

	do {
		set_pmd(pmd, __pmd(addr | pgprot_val(prot)));
		addr += PMD_SIZE;
	} while (pmd++, i++, i < PTRS_PER_PMD);
}

static void alloc_init_pmd(pud_t *pud, unsigned long addr, unsigned long end,
				  phys_addr_t phys, pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void),
				  bool allow_block_mappings)
{
	pmd_t *pmd;
	unsigned long next;

	/*
	 * Check for initial section mappings in the pgd/pud and remove them.
	 */
	if (pud_none(*pud) || pud_sect(*pud)) {
		phys_addr_t pmd_phys;
		BUG_ON(!pgtable_alloc);
		pmd_phys = pgtable_alloc();
		pmd = pmd_set_fixmap(pmd_phys);
		if (pud_sect(*pud)) {
			/*
			 * need to have the 1G of mappings continue to be
			 * present
			 */
			split_pud(pud, pmd);
		}
		__pud_populate(pud, pmd_phys, PUD_TYPE_TABLE);
		flush_tlb_all();
		pmd_clear_fixmap();
	}
	BUG_ON(pud_bad(*pud));

	pmd = pmd_set_fixmap_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		/* try section mapping first */
		if (((addr | next | phys) & ~SECTION_MASK) == 0 &&
		      allow_block_mappings) {
			pmd_t old_pmd =*pmd;
			pmd_set_huge(pmd, phys, prot);
			/*
			 * Check for previous table entries created during
			 * boot (__create_page_tables) and flush them.
			 */
			if (!pmd_none(old_pmd)) {
				flush_tlb_all();
				if (pmd_table(old_pmd)) {
					phys_addr_t table = pmd_page_paddr(old_pmd);
					if (!WARN_ON_ONCE(slab_is_available()))
						memblock_free(table, PAGE_SIZE);
				}
			}
		} else {
			alloc_init_pte(pmd, addr, next, __phys_to_pfn(phys),
				       prot, pgtable_alloc);
		}
		phys += next - addr;
	} while (pmd++, addr = next, addr != end);

	pmd_clear_fixmap();
}

static inline bool use_1G_block(unsigned long addr, unsigned long next,
			unsigned long phys)
{
	if (PAGE_SHIFT != 12)
		return false;

	if (((addr | next | phys) & ~PUD_MASK) != 0)
		return false;

	return true;
}

static void alloc_init_pud(pgd_t *pgd, unsigned long addr, unsigned long end,
				  phys_addr_t phys, pgprot_t prot,
				  phys_addr_t (*pgtable_alloc)(void),
				  bool allow_block_mappings)
{
	pud_t *pud;
	unsigned long next;

	if (pgd_none(*pgd)) {
		phys_addr_t pud_phys;
		BUG_ON(!pgtable_alloc);
		pud_phys = pgtable_alloc();
		__pgd_populate(pgd, pud_phys, PUD_TYPE_TABLE);
	}
	BUG_ON(pgd_bad(*pgd));

	pud = pud_set_fixmap_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);

		/*
		 * For 4K granule only, attempt to put down a 1GB block
		 */
		if (use_1G_block(addr, next, phys) && allow_block_mappings) {
			pud_t old_pud = *pud;
			pud_set_huge(pud, phys, prot);

			/*
			 * If we have an old value for a pud, it will
			 * be pointing to a pmd table that we no longer
			 * need (from swapper_pg_dir).
			 *
			 * Look up the old pmd table and free it.
			 */
			if (!pud_none(old_pud)) {
				flush_tlb_all();
				if (pud_table(old_pud)) {
					phys_addr_t table = pud_page_paddr(old_pud);
					if (!WARN_ON_ONCE(slab_is_available()))
						memblock_free(table, PAGE_SIZE);
				}
			}
		} else {
			alloc_init_pmd(pud, addr, next, phys, prot,
				       pgtable_alloc, allow_block_mappings);
		}
		phys += next - addr;
	} while (pud++, addr = next, addr != end);

	pud_clear_fixmap();
}

static void __create_pgd_mapping(pgd_t *pgdir, phys_addr_t phys,
				 unsigned long virt, phys_addr_t size,
				 pgprot_t prot,
				 phys_addr_t (*pgtable_alloc)(void),
				 bool allow_block_mappings)
{
	unsigned long addr, length, end, next;
	pgd_t *pgd = pgd_offset_raw(pgdir, virt);

	/*
	 * If the virtual and physical address don't have the same offset
	 * within a page, we cannot map the region as the caller expects.
	 */
	if (WARN_ON((phys ^ virt) & ~PAGE_MASK))
		return;

	phys &= PAGE_MASK;
	addr = virt & PAGE_MASK;
	length = PAGE_ALIGN(size + (virt & ~PAGE_MASK));

	end = addr + length;
	do {
		next = pgd_addr_end(addr, end);
		alloc_init_pud(pgd, addr, next, phys, prot, pgtable_alloc,
			       allow_block_mappings);
		phys += next - addr;
	} while (pgd++, addr = next, addr != end);
}

static phys_addr_t pgd_pgtable_alloc(void)
{
	void *ptr = (void *)__get_free_page(PGALLOC_GFP);
	if (!ptr || !pgtable_page_ctor(virt_to_page(ptr)))
		BUG();

	/* Ensure the zeroed page is visible to the page table walker */
	dsb(ishst);
	return __pa(ptr);
}

static phys_addr_t __init early_pgd_pgtable_alloc(void)
{
	phys_addr_t phys;
	void *ptr;

	phys = memblock_alloc(PAGE_SIZE, PAGE_SIZE);
	BUG_ON(!phys);
	ptr = __va(phys);
	memset(ptr, 0, PAGE_SIZE);
	/* Ensure the zeroed page is visible to the page table walker */
	dsb(ishst);
	return phys;
}

/*
 * This function can only be used to modify existing table entries,
 * without allocating new levels of table. Note that this permits the
 * creation of new section or page entries.
 */
void __init create_mapping_noalloc(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}
	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot, early_pgd_pgtable_alloc, true);
}

void __init create_pgd_mapping(struct mm_struct *mm, phys_addr_t phys,
			       unsigned long virt, phys_addr_t size,
			       pgprot_t prot, bool allow_block_mappings)
{
	BUG_ON(mm == &init_mm);

	__create_pgd_mapping(mm->pgd, phys, virt, size, prot,
			     pgd_pgtable_alloc, allow_block_mappings);
}

static void create_mapping_late(phys_addr_t phys, unsigned long virt,
				  phys_addr_t size, pgprot_t prot)
{
	if (virt < VMALLOC_START) {
		pr_warn("BUG: not creating mapping for %pa at 0x%016lx - outside kernel range\n",
			&phys, virt);
		return;
	}

	__create_pgd_mapping(init_mm.pgd, phys, virt, size, prot,
			     NULL, !debug_pagealloc_enabled());
}

static void __init __map_memblock(pgd_t *pgd, phys_addr_t start, phys_addr_t end)
{
	unsigned long kernel_start = __pa_symbol(_text);
	unsigned long kernel_end = __pa_symbol(__init_begin);

	/*
	 * Take care not to create a writable alias for the
	 * read-only text and rodata sections of the kernel image.
	 */

	/* No overlap with the kernel text/rodata */
	if (end < kernel_start || start >= kernel_end) {
		__create_pgd_mapping(pgd, start, __phys_to_virt(start),
				     end - start, PAGE_KERNEL,
				     early_pgtable_alloc,
				     !debug_pagealloc_enabled());
		return;
	}

	/*
	 * This block overlaps the kernel text/rodata mappings.
	 * Map the portion(s) which don't overlap.
	 */
	if (start < kernel_start)
		__create_pgd_mapping(pgd, start,
				     __phys_to_virt(start),
				     kernel_start - start, PAGE_KERNEL,
				     early_pgtable_alloc,
				     !debug_pagealloc_enabled());
	if (kernel_end < end)
		__create_pgd_mapping(pgd, kernel_end,
				     __phys_to_virt(kernel_end),
				     end - kernel_end, PAGE_KERNEL,
				     early_pgtable_alloc,
				     !debug_pagealloc_enabled());

	/*
	 * Map the linear alias of the [_text, __init_begin) interval as
	 * read-only/non-executable. This makes the contents of the
	 * region accessible to subsystems such as hibernate, but
	 * protects it from inadvertent modification or execution.
	 */
	__create_pgd_mapping(pgd, kernel_start, __phys_to_virt(kernel_start),
			     kernel_end - kernel_start, PAGE_KERNEL_RO,
			     early_pgtable_alloc, !debug_pagealloc_enabled());
}

static void __init map_mem(pgd_t *pgd)
{
	struct memblock_region *reg;

	/* map all the memory banks */
	for_each_memblock(memory, reg) {
		phys_addr_t start = reg->base;
		phys_addr_t end = start + reg->size;

		if (start >= end)
			break;
		if (memblock_is_nomap(reg))
			continue;

		__map_memblock(pgd, start, end);
	}
}

void mark_rodata_ro(void)
{
	unsigned long section_size;

	section_size = (unsigned long)_etext - (unsigned long)_text;
	create_mapping_late(__pa_symbol(_text), (unsigned long)_text,
			    section_size, PAGE_KERNEL_ROX);
	/*
	 * mark .rodata as read only. Use __init_begin rather than __end_rodata
	 * to cover NOTES and EXCEPTION_TABLE.
	 */
	section_size = (unsigned long)__init_begin - (unsigned long)__start_rodata;
	create_mapping_late(__pa_symbol(__start_rodata), (unsigned long)__start_rodata,
			    section_size, PAGE_KERNEL_RO);
}

static void __init map_kernel_segment(pgd_t *pgd, void *va_start, void *va_end,
				      pgprot_t prot, struct vm_struct *vma)
{
	phys_addr_t pa_start = __pa_symbol(va_start);
	unsigned long size = va_end - va_start;

	BUG_ON(!PAGE_ALIGNED(pa_start));
	BUG_ON(!PAGE_ALIGNED(size));

	__create_pgd_mapping(pgd, pa_start, (unsigned long)va_start, size, prot,
			     early_pgtable_alloc, !debug_pagealloc_enabled());

	vma->addr	= va_start;
	vma->phys_addr	= pa_start;
	vma->size	= size;
	vma->flags	= VM_MAP;
	vma->caller	= __builtin_return_address(0);

	vm_area_add_early(vma);
}

#ifdef CONFIG_UNMAP_KERNEL_AT_EL0
static int __init map_entry_trampoline(void)
{
	int i;
	extern char __entry_tramp_text_start[];

	pgprot_t prot = rodata_enabled ? PAGE_KERNEL_ROX : PAGE_KERNEL_EXEC;
	phys_addr_t pa_start = __pa_symbol(__entry_tramp_text_start);

	/* The trampoline is always mapped and can therefore be global */
	pgprot_val(prot) &= ~PTE_NG;

	/* Map only the text into the trampoline page table */
	memset(tramp_pg_dir, 0, PGD_SIZE);
	__create_pgd_mapping(tramp_pg_dir, pa_start, TRAMP_VALIAS,
			     entry_tramp_text_size(), prot, pgd_pgtable_alloc,
			     0);

	/* Map both the text and data into the kernel page table */
	for (i = 0; i < DIV_ROUND_UP(entry_tramp_text_size(), PAGE_SIZE); i++)
		__set_fixmap(FIX_ENTRY_TRAMP_TEXT1 - i,
			     pa_start + i * PAGE_SIZE, prot);

	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE)) {
		extern char __entry_tramp_data_start[];

		__set_fixmap(FIX_ENTRY_TRAMP_DATA,
			     __pa_symbol(__entry_tramp_data_start),
			     PAGE_KERNEL_RO);
	}

	return 0;
}
core_initcall(map_entry_trampoline);
#endif

/*
 * Create fine-grained mappings for the kernel.
 */
static void __init map_kernel(pgd_t *pgd)
{
	static struct vm_struct vmlinux_text, vmlinux_rodata, vmlinux_init, vmlinux_data;

	map_kernel_segment(pgd, _text, _etext, PAGE_KERNEL_EXEC, &vmlinux_text);
	map_kernel_segment(pgd, __start_rodata, __init_begin, PAGE_KERNEL, &vmlinux_rodata);
	map_kernel_segment(pgd, __init_begin, __init_end, PAGE_KERNEL_EXEC,
			   &vmlinux_init);
	map_kernel_segment(pgd, _data, _end, PAGE_KERNEL, &vmlinux_data);

	if (!pgd_val(*pgd_offset_raw(pgd, FIXADDR_START))) {
		/*
		 * The fixmap falls in a separate pgd to the kernel, and doesn't
		 * live in the carveout for the swapper_pg_dir. We can simply
		 * re-use the existing dir for the fixmap.
		 */
		set_pgd(pgd_offset_raw(pgd, FIXADDR_START),
			*pgd_offset_k(FIXADDR_START));
	} else if (CONFIG_PGTABLE_LEVELS > 3) {
		/*
		 * The fixmap shares its top level pgd entry with the kernel
		 * mapping. This can really only occur when we are running
		 * with 16k/4 levels, so we can simply reuse the pud level
		 * entry instead.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		set_pud(pud_set_fixmap_offset(pgd, FIXADDR_START),
			__pud(__pa_symbol(bm_pmd) | PUD_TYPE_TABLE));
		pud_clear_fixmap();
	} else {
		BUG();
	}

	kasan_copy_shadow(pgd);
}

/*
 * paging_init() sets up the page tables, initialises the zone memory
 * maps and sets up the zero page.
 */
void __init dma_contiguous_remap(void);

void __init paging_init(void)
{
	phys_addr_t pgd_phys = early_pgtable_alloc();
	pgd_t *pgd = pgd_set_fixmap(pgd_phys);

	map_kernel(pgd);
	map_mem(pgd);

	/*
	 * We want to reuse the original swapper_pg_dir so we don't have to
	 * communicate the new address to non-coherent secondaries in
	 * secondary_entry, and so cpu_switch_mm can generate the address with
	 * adrp+add rather than a load from some global variable.
	 *
	 * To do this we need to go via a temporary pgd.
	 */
	cpu_replace_ttbr1(__va(pgd_phys));
	memcpy(swapper_pg_dir, pgd, PGD_SIZE);
	cpu_replace_ttbr1(lm_alias(swapper_pg_dir));

	pgd_clear_fixmap();
	memblock_free(pgd_phys, PAGE_SIZE);

	/*
	 * We only reuse the PGD from the swapper_pg_dir, not the pud + pmd
	 * allocated with it.
	 */
	memblock_free(__pa_symbol(swapper_pg_dir) + PAGE_SIZE,
		      SWAPPER_DIR_SIZE - PAGE_SIZE);
	dma_contiguous_remap();
	local_flush_tlb_all();
}

#ifdef CONFIG_MEMORY_HOTPLUG

/*
 * hotplug_paging() is used by memory hotplug to build new page tables
 * for hot added memory.
 */
void hotplug_paging(phys_addr_t start, phys_addr_t size)
{

       struct page *pg;
       phys_addr_t pgd_phys = pgd_pgtable_alloc();
       pgd_t *pgd = pgd_set_fixmap(pgd_phys);

       memcpy(pgd, swapper_pg_dir, PAGE_SIZE);

       __create_pgd_mapping(pgd, start, __phys_to_virt(start), size,
               PAGE_KERNEL, pgd_pgtable_alloc, false);

       cpu_replace_ttbr1(__va(pgd_phys));
       memcpy(swapper_pg_dir, pgd, PAGE_SIZE);
       cpu_replace_ttbr1(swapper_pg_dir);

       pgd_clear_fixmap();

       pg = phys_to_page(pgd_phys);
       pgtable_page_dtor(pg);
       __free_pages(pg, 0);
}

#ifdef CONFIG_MEMORY_HOTREMOVE
#define PAGE_INUSE 0xFD

static void  free_pagetable(struct page *page, int order, bool direct)
{
	unsigned long magic;
	unsigned int nr_pages = 1 << order;
	struct vmem_altmap *altmap = to_vmem_altmap((unsigned long) page);

	if (altmap) {
		vmem_altmap_free(altmap, nr_pages);
		return;
	}

	/* bootmem page has reserved flag */
	if (PageReserved(page)) {
		__ClearPageReserved(page);

		magic = (unsigned long)page->lru.next;
		if (magic == SECTION_INFO || magic == MIX_SECTION_INFO) {
			while (nr_pages--)
				put_page_bootmem(page++);
		} else {
			while (nr_pages--)
				free_reserved_page(page++);
		}
	} else {
		/*
		 * Only direct pagetable allocation (those allocated via
		 * hotplug) call the pgtable_page_ctor; vmemmap pgtable
		 * allocations don't.
		 */
		if (direct)
			pgtable_page_dtor(page);

		free_pages((unsigned long)page_address(page), order);
	}
}

static void free_pte_table(pmd_t *pmd, bool direct)
{
	pte_t *pte_start, *pte;
	struct page *page;
	int i;

	pte_start =  (pte_t *) pmd_page_vaddr(*pmd);
	/* Check if there is no valid entry in the PMD */
	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte = pte_start + i;
		if (!pte_none(*pte))
			return;
	}

	page = pmd_page(*pmd);

	free_pagetable(page, 0, direct);

	/*
	 * This spin lock could be only taken in _pte_aloc_kernel
	 * in mm/memory.c and nowhere else (for arm64). Not sure if
	 * the function above can be called concurrently. In doubt,
	 * I am living it here for now, but it probably can be removed
	 */
	spin_lock(&init_mm.page_table_lock);
	pmd_clear(pmd);
	spin_unlock(&init_mm.page_table_lock);
}

static void free_pmd_table(pud_t *pud, bool direct)
{
	pmd_t *pmd_start, *pmd;
	struct page *page;
	int i;

	pmd_start = (pmd_t *) pud_page_vaddr(*pud);
	/* Check if there is no valid entry in the PMD */
	for (i = 0; i < PTRS_PER_PMD; i++) {
		pmd = pmd_start + i;
		if (!pmd_none(*pmd))
			return;
	}

	page = pud_page(*pud);

	free_pagetable(page, 0, direct);

	/*
	 * This spin lock could be only taken in _pte_aloc_kernel
	 * in mm/memory.c and nowhere else (for arm64). Not sure if
	 * the function above can be called concurrently. In doubt,
	 * I am living it here for now, but it probably can be removed
	 */
	spin_lock(&init_mm.page_table_lock);
	pud_clear(pud);
	spin_unlock(&init_mm.page_table_lock);
}

/*
 * When the PUD is folded on the PGD (three levels of paging),
 * there's no need to free PUDs
 */
#if CONFIG_PGTABLE_LEVELS > 3
static void free_pud_table(pgd_t *pgd, bool direct)
{
	pud_t *pud_start, *pud;
	struct page *page;
	int i;

	pud_start = (pud_t *) pgd_page_vaddr(*pgd);
	/* Check if there is no valid entry in the PUD */
	for (i = 0; i < PTRS_PER_PUD; i++) {
		pud = pud_start + i;
		if (!pud_none(*pud))
			return;
	}

	page = pgd_page(*pgd);

	free_pagetable(page, 0, direct);

	/*
	 * This spin lock could be only
	 * taken in _pte_aloc_kernel in
	 * mm/memory.c and nowhere else
	 * (for arm64). Not sure if the
	 * function above can be called
	 * concurrently. In doubt,
	 * I am living it here for now,
	 * but it probably can be removed.
	 */
	spin_lock(&init_mm.page_table_lock);
	pgd_clear(pgd);
	spin_unlock(&init_mm.page_table_lock);
}
#endif

static void remove_pte_table(pte_t *pte, unsigned long addr,
	unsigned long end, bool direct)
{
	unsigned long next;
	void *page_addr;

	for (; addr < end; addr = next, pte++) {
		next = (addr + PAGE_SIZE) & PAGE_MASK;
		if (next > end)
			next = end;

		if (!pte_present(*pte))
			continue;

		if (PAGE_ALIGNED(addr) && PAGE_ALIGNED(next)) {
			/*
			 * Do not free direct mapping pages since they were
			 * freed when offlining, or simplely not in use.
			 */
			if (!direct)
				free_pagetable(pte_page(*pte), 0, direct);

			/*
			 * This spin lock could be only
			 * taken in _pte_aloc_kernel in
			 * mm/memory.c and nowhere else
			 * (for arm64). Not sure if the
			 * function above can be called
			 * concurrently. In doubt,
			 * I am living it here for now,
			 * but it probably can be removed.
			 */
			spin_lock(&init_mm.page_table_lock);
			pte_clear(&init_mm, addr, pte);
			spin_unlock(&init_mm.page_table_lock);
		} else {
			/*
			 * If we are here, we are freeing vmemmap pages since
			 * direct mapped memory ranges to be freed are aligned.
			 *
			 * If we are not removing the whole page, it means
			 * other page structs in this page are being used and
			 * we canot remove them. So fill the unused page_structs
			 * with 0xFD, and remove the page when it is wholly
			 * filled with 0xFD.
			 */
			memset((void *)addr, PAGE_INUSE, next - addr);

			page_addr = page_address(pte_page(*pte));
			if (!memchr_inv(page_addr, PAGE_INUSE, PAGE_SIZE)) {
				free_pagetable(pte_page(*pte), 0, direct);

				/*
				 * This spin lock could be only
				 * taken in _pte_aloc_kernel in
				 * mm/memory.c and nowhere else
				 * (for arm64). Not sure if the
				 * function above can be called
				 * concurrently. In doubt,
				 * I am living it here for now,
				 * but it probably can be removed.
				 */
				spin_lock(&init_mm.page_table_lock);
				pte_clear(&init_mm, addr, pte);
				spin_unlock(&init_mm.page_table_lock);
			}
		}
	}

	// I am adding this flush here in simmetry to the x86 code.
	// Why do I need to call it here and not in remove_p[mu]d
	flush_tlb_all();
}

static void remove_pmd_table(pmd_t *pmd, unsigned long addr,
	unsigned long end, bool direct)
{
	unsigned long next;
	void *page_addr;
	pte_t *pte;

	for (; addr < end; addr = next, pmd++) {
		next = pmd_addr_end(addr, end);

		if (!pmd_present(*pmd))
			continue;

		// check if we are using 2MB section mappings
		if (pmd_sect(*pmd)) {
			if (PAGE_ALIGNED(addr) && PAGE_ALIGNED(next)) {
				if (!direct) {
					free_pagetable(pmd_page(*pmd),
						get_order(PMD_SIZE), direct);
				}
				/*
				 * This spin lock could be only
				 * taken in _pte_aloc_kernel in
				 * mm/memory.c and nowhere else
				 * (for arm64). Not sure if the
				 * function above can be called
				 * concurrently. In doubt,
				 * I am living it here for now,
				 * but it probably can be removed.
				 */
				spin_lock(&init_mm.page_table_lock);
				pmd_clear(pmd);
				spin_unlock(&init_mm.page_table_lock);
			} else {
				/* If here, we are freeing vmemmap pages. */
				memset((void *)addr, PAGE_INUSE, next - addr);

				page_addr = page_address(pmd_page(*pmd));
				if (!memchr_inv(page_addr, PAGE_INUSE,
						PMD_SIZE)) {
					free_pagetable(pmd_page(*pmd),
						get_order(PMD_SIZE), direct);

					/*
					 * This spin lock could be only
					 * taken in _pte_aloc_kernel in
					 * mm/memory.c and nowhere else
					 * (for arm64). Not sure if the
					 * function above can be called
					 * concurrently. In doubt,
					 * I am living it here for now,
					 * but it probably can be removed.
					 */
					spin_lock(&init_mm.page_table_lock);
					pmd_clear(pmd);
					spin_unlock(&init_mm.page_table_lock);
				}
			}
			continue;
		}

		BUG_ON(!pmd_table(*pmd));

		pte = pte_offset_map(pmd, addr);
		remove_pte_table(pte, addr, next, direct);
		free_pte_table(pmd, direct);
	}
}

static void remove_pud_table(pud_t *pud, unsigned long addr,
	unsigned long end, bool direct)
{
	unsigned long next;
	pmd_t *pmd;
	void *page_addr;

	for (; addr < end; addr = next, pud++) {
		next = pud_addr_end(addr, end);
		if (!pud_present(*pud))
			continue;
		/*
		 * If we are using 4K granules, check if we are using
		 * 1GB section mapping.
		 */
		if (pud_sect(*pud)) {
			if (PAGE_ALIGNED(addr) && PAGE_ALIGNED(next)) {
				if (!direct) {
					free_pagetable(pud_page(*pud),
						get_order(PUD_SIZE), direct);
				}

				/*
				 * This spin lock could be only
				 * taken in _pte_aloc_kernel in
				 * mm/memory.c and nowhere else
				 * (for arm64). Not sure if the
				 * function above can be called
				 * concurrently. In doubt,
				 * I am living it here for now,
				 * but it probably can be removed.
				 */
				spin_lock(&init_mm.page_table_lock);
				pud_clear(pud);
				spin_unlock(&init_mm.page_table_lock);
			} else {
				/* If here, we are freeing vmemmap pages. */
				memset((void *)addr, PAGE_INUSE, next - addr);

				page_addr = page_address(pud_page(*pud));
				if (!memchr_inv(page_addr, PAGE_INUSE,
						PUD_SIZE)) {

					free_pagetable(pud_page(*pud),
						get_order(PUD_SIZE), direct);

					/*
					 * This spin lock could be only
					 * taken in _pte_aloc_kernel in
					 * mm/memory.c and nowhere else
					 * (for arm64). Not sure if the
					 * function above can be called
					 * concurrently. In doubt,
					 * I am living it here for now,
					 * but it probably can be removed.
					 */
					spin_lock(&init_mm.page_table_lock);
					pud_clear(pud);
					spin_unlock(&init_mm.page_table_lock);
				}
			}
			continue;
		}

		BUG_ON(!pud_table(*pud));

		pmd = pmd_offset(pud, addr);
		remove_pmd_table(pmd, addr, next, direct);
		free_pmd_table(pud, direct);
	}
}

void remove_pagetable(unsigned long start, unsigned long end, bool direct)
{
	unsigned long next;
	unsigned long addr;
	pgd_t *pgd;
	pud_t *pud;

	for (addr = start; addr < end; addr = next) {
		next = pgd_addr_end(addr, end);

		pgd = pgd_offset_k(addr);
		if (pgd_none(*pgd))
			continue;

		pud = pud_offset(pgd, addr);
		remove_pud_table(pud, addr, next, direct);
		/*
		 * When the PUD is folded on the PGD (three levels of paging),
		 * I did already clear the PMD page in free_pmd_table,
		 * and reset the corresponding PGD==PUD entry.
		 */
#if CONFIG_PGTABLE_LEVELS > 3
		free_pud_table(pgd, direct);
#endif
	}

	flush_tlb_all();
}

#endif /* CONFIG_MEMORY_HOTREMOVE */
#endif /* CONFIG_MEMORY_HOTPLUG */

/*
 * Check whether a kernel address is valid (derived from arch/x86/).
 */
int kern_addr_valid(unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	if ((((long)addr) >> VA_BITS) != -1UL)
		return 0;

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return 0;

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return 0;

	if (pud_sect(*pud))
		return pfn_valid(pud_pfn(*pud));

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return 0;

	if (pmd_sect(*pmd))
		return pfn_valid(pmd_pfn(*pmd));

	pte = pte_offset_kernel(pmd, addr);
	if (pte_none(*pte))
		return 0;

	return pfn_valid(pte_pfn(*pte));
}
#ifdef CONFIG_SPARSEMEM_VMEMMAP
#if !ARM64_SWAPPER_USES_SECTION_MAPS
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	return vmemmap_populate_basepages(start, end, node);
}
#else	/* !ARM64_SWAPPER_USES_SECTION_MAPS */
int __meminit vmemmap_populate(unsigned long start, unsigned long end, int node)
{
	unsigned long addr = start;
	unsigned long next;
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	do {
		next = pmd_addr_end(addr, end);

		pgd = vmemmap_pgd_populate(addr, node);
		if (!pgd)
			return -ENOMEM;

		pud = vmemmap_pud_populate(pgd, addr, node);
		if (!pud)
			return -ENOMEM;

		pmd = pmd_offset(pud, addr);
		if (pmd_none(*pmd)) {
			void *p = NULL;

			p = vmemmap_alloc_block_buf(PMD_SIZE, node);
			if (!p)
				return -ENOMEM;

			set_pmd(pmd, __pmd(__pa(p) | PROT_SECT_NORMAL));
		} else
			vmemmap_verify((pte_t *)pmd, node, addr, next);
	} while (addr = next, addr != end);

	return 0;
}
#endif	/* CONFIG_ARM64_64K_PAGES */
void vmemmap_free(unsigned long start, unsigned long end)
{
#ifdef CONFIG_MEMORY_HOTREMOVE
	remove_pagetable(start, end, false);
#endif
}
#endif	/* CONFIG_SPARSEMEM_VMEMMAP */

static inline pud_t * fixmap_pud(unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k(addr);

	BUG_ON(pgd_none(*pgd) || pgd_bad(*pgd));

	return pud_offset_kimg(pgd, addr);
}

static inline pmd_t * fixmap_pmd(unsigned long addr)
{
	pud_t *pud = fixmap_pud(addr);

	BUG_ON(pud_none(*pud) || pud_bad(*pud));

	return pmd_offset_kimg(pud, addr);
}

static inline pte_t * fixmap_pte(unsigned long addr)
{
	return &bm_pte[pte_index(addr)];
}

/*
 * The p*d_populate functions call virt_to_phys implicitly so they can't be used
 * directly on kernel symbols (bm_p*d). This function is called too early to use
 * lm_alias so __p*d_populate functions must be used to populate with the
 * physical address from __pa_symbol.
 */
void __init early_fixmap_init(void)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	unsigned long addr = FIXADDR_START;

	pgd = pgd_offset_k(addr);
	if (CONFIG_PGTABLE_LEVELS > 3 &&
	    !(pgd_none(*pgd) || pgd_page_paddr(*pgd) == __pa_symbol(bm_pud))) {
		/*
		 * We only end up here if the kernel mapping and the fixmap
		 * share the top level pgd entry, which should only happen on
		 * 16k/4 levels configurations.
		 */
		BUG_ON(!IS_ENABLED(CONFIG_ARM64_16K_PAGES));
		pud = pud_offset_kimg(pgd, addr);
	} else {
		if (pgd_none(*pgd))
			__pgd_populate(pgd, __pa_symbol(bm_pud), PUD_TYPE_TABLE);
		pud = fixmap_pud(addr);
	}
	if (pud_none(*pud))
		__pud_populate(pud, __pa_symbol(bm_pmd), PMD_TYPE_TABLE);
	pmd = fixmap_pmd(addr);
	__pmd_populate(pmd, __pa_symbol(bm_pte), PMD_TYPE_TABLE);

	/*
	 * The boot-ioremap range spans multiple pmds, for which
	 * we are not prepared:
	 */
	BUILD_BUG_ON((__fix_to_virt(FIX_BTMAP_BEGIN) >> PMD_SHIFT)
		     != (__fix_to_virt(FIX_BTMAP_END) >> PMD_SHIFT));

	if ((pmd != fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)))
	     || pmd != fixmap_pmd(fix_to_virt(FIX_BTMAP_END))) {
		WARN_ON(1);
		pr_warn("pmd %p != %p, %p\n",
			pmd, fixmap_pmd(fix_to_virt(FIX_BTMAP_BEGIN)),
			fixmap_pmd(fix_to_virt(FIX_BTMAP_END)));
		pr_warn("fix_to_virt(FIX_BTMAP_BEGIN): %08lx\n",
			fix_to_virt(FIX_BTMAP_BEGIN));
		pr_warn("fix_to_virt(FIX_BTMAP_END):   %08lx\n",
			fix_to_virt(FIX_BTMAP_END));

		pr_warn("FIX_BTMAP_END:       %d\n", FIX_BTMAP_END);
		pr_warn("FIX_BTMAP_BEGIN:     %d\n", FIX_BTMAP_BEGIN);
	}
}

void __set_fixmap(enum fixed_addresses idx,
			       phys_addr_t phys, pgprot_t flags)
{
	unsigned long addr = __fix_to_virt(idx);
	pte_t *pte;

	BUG_ON(idx <= FIX_HOLE || idx >= __end_of_fixed_addresses);

	pte = fixmap_pte(addr);

	if (pgprot_val(flags)) {
		set_pte(pte, pfn_pte(phys >> PAGE_SHIFT, flags));
	} else {
		pte_clear(&init_mm, addr, pte);
		flush_tlb_kernel_range(addr, addr+PAGE_SIZE);
	}
}

void *__init __fixmap_remap_fdt(phys_addr_t dt_phys, int *size, pgprot_t prot)
{
	const u64 dt_virt_base = __fix_to_virt(FIX_FDT);
	int offset;
	void *dt_virt;

	/*
	 * Check whether the physical FDT address is set and meets the minimum
	 * alignment requirement. Since we are relying on MIN_FDT_ALIGN to be
	 * at least 8 bytes so that we can always access the magic and size
	 * fields of the FDT header after mapping the first chunk, double check
	 * here if that is indeed the case.
	 */
	BUILD_BUG_ON(MIN_FDT_ALIGN < 8);
	if (!dt_phys || dt_phys % MIN_FDT_ALIGN)
		return NULL;

	/*
	 * Make sure that the FDT region can be mapped without the need to
	 * allocate additional translation table pages, so that it is safe
	 * to call create_mapping_noalloc() this early.
	 *
	 * On 64k pages, the FDT will be mapped using PTEs, so we need to
	 * be in the same PMD as the rest of the fixmap.
	 * On 4k pages, we'll use section mappings for the FDT so we only
	 * have to be in the same PUD.
	 */
	BUILD_BUG_ON(dt_virt_base % SZ_2M);

	BUILD_BUG_ON(__fix_to_virt(FIX_FDT_END) >> SWAPPER_TABLE_SHIFT !=
		     __fix_to_virt(FIX_BTMAP_BEGIN) >> SWAPPER_TABLE_SHIFT);

	offset = dt_phys % SWAPPER_BLOCK_SIZE;
	dt_virt = (void *)dt_virt_base + offset;

	/* map the first chunk so we can read the size from the header */
	create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE),
			dt_virt_base, SWAPPER_BLOCK_SIZE, prot);

	if (fdt_magic(dt_virt) != FDT_MAGIC)
		return NULL;

	*size = fdt_totalsize(dt_virt);
	if (*size > MAX_FDT_SIZE)
		return NULL;

	if (offset + *size > SWAPPER_BLOCK_SIZE)
		create_mapping_noalloc(round_down(dt_phys, SWAPPER_BLOCK_SIZE), dt_virt_base,
			       round_up(offset + *size, SWAPPER_BLOCK_SIZE), prot);

	return dt_virt;
}

void *__init fixmap_remap_fdt(phys_addr_t dt_phys)
{
	void *dt_virt;
	int size;

	dt_virt = __fixmap_remap_fdt(dt_phys, &size, PAGE_KERNEL_RO);
	if (!dt_virt)
		return NULL;

	memblock_reserve(dt_phys, size);
	return dt_virt;
}

int __init arch_ioremap_pud_supported(void)
{
	/* only 4k granule supports level 1 block mappings */
	return IS_ENABLED(CONFIG_ARM64_4K_PAGES);
}

int __init arch_ioremap_pmd_supported(void)
{
	return 1;
}

int pud_set_huge(pud_t *pud, phys_addr_t phys, pgprot_t prot)
{
	BUG_ON(phys & ~PUD_MASK);
	set_pud(pud, __pud(phys | PUD_TYPE_SECT | pgprot_val(mk_sect_prot(prot))));
	return 1;
}

int pmd_set_huge(pmd_t *pmd, phys_addr_t phys, pgprot_t prot)
{
	BUG_ON(phys & ~PMD_MASK);
	set_pmd(pmd, __pmd(phys | PMD_TYPE_SECT | pgprot_val(mk_sect_prot(prot))));
	return 1;
}

int pud_clear_huge(pud_t *pud)
{
	if (!pud_sect(*pud))
		return 0;
	pud_clear(pud);
	return 1;
}

int pmd_clear_huge(pmd_t *pmd)
{
	if (!pmd_sect(*pmd))
		return 0;
	pmd_clear(pmd);
	return 1;
}

int pud_free_pmd_page(pud_t *pud, unsigned long addr)
{
	return pud_none(*pud);
}

int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)
{
	return pmd_none(*pmd);
}
