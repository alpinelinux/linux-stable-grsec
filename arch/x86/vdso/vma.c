/*
 * Set up the VMAs to tell the VM about the vDSO.
 * Copyright 2007 Andi Kleen, SUSE Labs.
 * Subject to the GPL, v.2
 */
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/random.h>
#include <linux/elf.h>
#include <asm/vsyscall.h>
#include <asm/vgtod.h>
#include <asm/proto.h>
#include <asm/vdso.h>
#include <asm/page.h>
#include <asm/hpet.h>
#include <asm/mman.h>

#if defined(CONFIG_X86_64)
extern unsigned short vdso_sync_cpuid;
#endif

void __init init_vdso_image(const struct vdso_image *image)
{
	int i;
	int npages = (image->size) / PAGE_SIZE;

	BUG_ON(image->size % PAGE_SIZE != 0);
	for (i = 0; i < npages; i++)
		image->text_mapping.pages[i] =
			virt_to_page(image->data + i*PAGE_SIZE);

	apply_alternatives((struct alt_instr *)(image->data + image->alt),
			   (struct alt_instr *)(image->data + image->alt +
						image->alt_len));
}

#if defined(CONFIG_X86_64)
static int __init init_vdso(void)
{
	init_vdso_image(&vdso_image_64);

#ifdef CONFIG_X86_X32_ABI
	init_vdso_image(&vdso_image_x32);
#endif

	return 0;
}
subsys_initcall(init_vdso);
#endif

struct linux_binprm;

/*
 * Put the vdso above the (randomized) stack with another randomized
 * offset.  This way there is no hole in the middle of address space.
 * To save memory make sure it is still in the same PTE as the stack
 * top.  This doesn't give that many random bits.
 *
 * Note that this algorithm is imperfect: the distribution of the vdso
 * start address within a PMD is biased toward the end.
 *
 * Only used for the 64-bit and x32 vdsos.
 */
static unsigned long vdso_addr(unsigned long start, unsigned len)
{
#ifdef CONFIG_X86_32
	return 0;
#else
	unsigned long addr, end;
	unsigned offset;

	/*
	 * Round up the start address.  It can start out unaligned as a result
	 * of stack start randomization.
	 */
	start = PAGE_ALIGN(start);

	/* Round the lowest possible end address up to a PMD boundary. */
	end = (start + len + PMD_SIZE - 1) & PMD_MASK;
	if (end >= TASK_SIZE_MAX)
		end = TASK_SIZE_MAX;
	end -= len;

	if (end > start) {
		offset = get_random_int() % (((end - start) >> PAGE_SHIFT) + 1);
		addr = start + (offset << PAGE_SHIFT);
	} else {
		addr = start;
	}

	/*
	 * Forcibly align the final address in case we have a hardware
	 * issue that requires alignment for performance reasons.
	 */
	addr = align_vdso_addr(addr);

	/*
	 * Forcibly align the final address in case we have a hardware
	 * issue that requires alignment for performance reasons.
	 */
	addr = align_vdso_addr(addr);

	return addr;
#endif
}

static int map_vdso(const struct vdso_image *image, bool calculate_addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned long addr, text_start;
	int ret = 0;
	static struct page *no_pages[] = {NULL};
	static struct vm_special_mapping vvar_mapping = {
		.name = "[vvar]",
		.pages = no_pages,
	};

#ifdef CONFIG_PAX_RANDMMAP
	if (mm->pax_flags & MF_PAX_RANDMMAP)
		calculate_addr = false;
#endif

	if (calculate_addr) {
		addr = vdso_addr(current->mm->start_stack,
				 image->size - image->sym_vvar_start);
	} else {
		addr = 0;
	}

	down_write(&mm->mmap_sem);

	addr = get_unmapped_area(NULL, addr,
				 image->size - image->sym_vvar_start, 0, MAP_EXECUTABLE);
	if (IS_ERR_VALUE(addr)) {
		ret = addr;
		goto up_fail;
	}

	text_start = addr - image->sym_vvar_start;
	mm->context.vdso = text_start;

	/*
	 * MAYWRITE to allow gdb to COW and set breakpoints
	 */
	vma = _install_special_mapping(mm,
				       text_start,
				       image->size,
				       VM_READ|VM_EXEC|
				       VM_MAYREAD|VM_MAYWRITE|VM_MAYEXEC,
				       &image->text_mapping);

	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto up_fail;
	}

	vma = _install_special_mapping(mm,
				       addr,
				       -image->sym_vvar_start,
				       VM_READ|VM_MAYREAD,
				       &vvar_mapping);

	if (IS_ERR(vma)) {
		ret = PTR_ERR(vma);
		goto up_fail;
	}

	if (image->sym_vvar_page)
		ret = remap_pfn_range(vma,
				      text_start + image->sym_vvar_page,
				      __pa_symbol(&__vvar_page) >> PAGE_SHIFT,
				      PAGE_SIZE,
				      PAGE_READONLY);

	if (ret)
		goto up_fail;

#ifdef CONFIG_HPET_TIMER
	if (hpet_address && image->sym_hpet_page) {
		ret = io_remap_pfn_range(vma,
			text_start + image->sym_hpet_page,
			hpet_address >> PAGE_SHIFT,
			PAGE_SIZE,
			pgprot_noncached(PAGE_READONLY));
	}
#endif

up_fail:
	if (ret)
		current->mm->context.vdso = 0;

	up_write(&mm->mmap_sem);
	return ret;
}

#if defined(CONFIG_X86_32) || defined(CONFIG_COMPAT)
static int load_vdso32(void)
{
	int ret;

	if (vdso32_enabled != 1)  /* Other values all mean "disabled" */
		return 0;

	ret = map_vdso(selected_vdso32, false);
	if (ret)
		return ret;

	if (selected_vdso32->sym_VDSO32_SYSENTER_RETURN)
		current_thread_info()->sysenter_return =
			(void __force_user *)(current->mm->context.vdso +
			selected_vdso32->sym_VDSO32_SYSENTER_RETURN);

	return 0;
}
#endif

#ifdef CONFIG_X86_64
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	return map_vdso(&vdso_image_64, true);
}

#ifdef CONFIG_COMPAT
int compat_arch_setup_additional_pages(struct linux_binprm *bprm,
				       int uses_interp)
{
#ifdef CONFIG_X86_X32_ABI
	if (test_thread_flag(TIF_X32))
		return map_vdso(&vdso_image_x32, true);
#endif

	return load_vdso32();
}
#endif
#else
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
	return load_vdso32();
}
#endif
