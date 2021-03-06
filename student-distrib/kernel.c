/* kernel.c - the C part of the kernel
 * vim:ts=4 noexpandtab
 */

#include "multiboot.h"
#include "x86_desc.h"
#include "lib.h"
#include "i8259.h"
#include "debug.h"
#include "idt_funcs.h"
#include "idt_linkage.h"
#include "rtc.h"
#include "page.h"
#include "terminal.h"
#include "zbigfs.h"
#include "syscall.h"
#include "syscall_funcs.h"
#include "tasking_funcs.h"

/* Macros. */
/* Check if the bit BIT in FLAGS is set. */
#define CHECK_FLAG(flags,bit)   ((flags) & (1 << (bit)))

/*Magic Numbers*/ 
#define EXCEPTION_INDEX 0x20
#define KEYBOARD_INDEX 0x21
#define RTC_INDEX 0x28
#define SYS_CALLS_INDEX 0x80
#define PIT_INDEX 0x20
#define PIT_CTRL_ADDR 0x43
#define PIT_PORT0_CTRL_ADDR 0x40
#define PIT_DEFAULT_RATE_LOW 0
#define PIT_DEFAULT_RATE_HIGH 0
#define PIT_MODIFY_LOW  0x10
#define PIT_MODIFY_HIGH 0x20
#define PIT_MODE_2 (1<<2)

/* Check if MAGIC is valid and print the Multiboot information structure
   pointed by ADDR. */


/*
* void populate_idt();
*   Inputs: none
*   Return Value: void
*	Function: Fills the idt with the correct functions
*/
void populate_idt()
{
	int i;
	/*initializing the specific interrupts 0 - 20*/ 
	void (* arr[NUM_VEC]) = 
	{ &divide_by_zero, &reserved_1, &non_maskable_interrupt, 
	&breakpoint, &overflow, &BOUND_range_exceeded, &invalid_opcode,
	&device_not_available, &double_fault, &coprocessor_segment_overrun, 
	&invalid_TSS, &segment_not_present, &stack_segment_fault, &general_protection,
	&page_fault, &common_interrupt, &floating_point_error, &alignment_check, 
	&machine_check, &SIMD_floating_point_exception
	};

	/*iterating through idt, populating with correct interrupts*/
	for(i = 0; i< NUM_VEC; i++)
	{
		idt[i].present = 1;
		idt[i].dpl = 0;
		idt[i].reserved0 = 0;
		idt[i].size = 1;
		idt[i].reserved1 = 1;
		idt[i].reserved2 = 1;
		idt[i].reserved3 = 0;
		idt[i].reserved4 = 0;
		idt[i].seg_selector = KERNEL_CS;

		/*Do not set IDT_ENTRY for 20 - 32 (Intel Reserved) */ 
		if(i >= EXCEPTION_INDEX) 
		{
			/*Set common interrupt for user defined*/ 
			SET_IDT_ENTRY(idt[i],&common_interrupt);	
		}
		else if(i < 20)
		{
			if( i == 15 )
			{
				/*DO NOTHING, Intel Reserved*/ 
			}else{
				SET_IDT_ENTRY(idt[i], arr[i]); 
			}	
		}
	}
	/*setting more specific interrupts*/ 
	SET_IDT_ENTRY(idt[RTC_INDEX],&asm_rtc);
	SET_IDT_ENTRY(idt[KEYBOARD_INDEX],&asm_keyboard);
	SET_IDT_ENTRY(idt[SYS_CALLS_INDEX],&syscall);
	idt[SYS_CALLS_INDEX].dpl = 3;
	idt[SYS_CALLS_INDEX].reserved3 = 1;
	SET_IDT_ENTRY(idt[PIT_INDEX],&next_task);

	/*loading IDTR*/ 
	lidt(idt_desc_ptr);
}


void
entry (unsigned long magic, unsigned long addr)
{
	multiboot_info_t *mbi;

	/* Clear the screen. */
	clear();

	/* Am I booted by a Multiboot-compliant boot loader? */
	if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
	{
		printf ("Invalid magic number: 0x%#x\n", (unsigned) magic);
		return;
	}

	/* Set MBI to the address of the Multiboot information structure. */
	mbi = (multiboot_info_t *) addr;

	/* Print out the flags. */
	printf ("flags = 0x%#x\n", (unsigned) mbi->flags);

	/* Are mem_* valid? */
	if (CHECK_FLAG (mbi->flags, 0))
		printf ("mem_lower = %uKB, mem_upper = %uKB\n",
				(unsigned) mbi->mem_lower, (unsigned) mbi->mem_upper);

	/* Is boot_device valid? */
	if (CHECK_FLAG (mbi->flags, 1))
		printf ("boot_device = 0x%#x\n", (unsigned) mbi->boot_device);

	/* Is the command line passed? */
	if (CHECK_FLAG (mbi->flags, 2))
		printf ("cmdline = %s\n", (char *) mbi->cmdline);

	void * zbigfs_location = NULL;
	if (CHECK_FLAG (mbi->flags, 3)) {
		int mod_count = 0;
		int i;
		module_t* mod = (module_t*)mbi->mods_addr;
		while(mod_count < mbi->mods_count) {
			printf("Module %d loaded at address: 0x%#x\n", mod_count, (unsigned int)mod->mod_start);
			printf("Module %d ends at address: 0x%#x\n", mod_count, (unsigned int)mod->mod_end);
			printf("First few bytes of module:\n");
			for(i = 0; i < 16; i++) {
				printf("0x%x ", *((char*)(mod->mod_start+i)));
			}
			printf("\n");
			mod_count++;
			mod++;
		}
		if (mod_count){
			module_t* zbigfsmod = (module_t*)mbi->mods_addr;
			zbigfs_location = (void *) zbigfsmod->mod_start;
		} else {
			printf("No module 0, zbigfs will FAIL!");
		}
	}
	/* Bits 4 and 5 are mutually exclusive! */
	if (CHECK_FLAG (mbi->flags, 4) && CHECK_FLAG (mbi->flags, 5))
	{
		printf ("Both bits 4 and 5 are set.\n");
		return;
	}

	/* Is the section header table of ELF valid? */
	if (CHECK_FLAG (mbi->flags, 5))
	{
		elf_section_header_table_t *elf_sec = &(mbi->elf_sec);

		printf ("elf_sec: num = %u, size = 0x%#x,"
				" addr = 0x%#x, shndx = 0x%#x\n",
				(unsigned) elf_sec->num, (unsigned) elf_sec->size,
				(unsigned) elf_sec->addr, (unsigned) elf_sec->shndx);
	}

	/* Are mmap_* valid? */
	if (CHECK_FLAG (mbi->flags, 6))
	{
		memory_map_t *mmap;

		printf ("mmap_addr = 0x%#x, mmap_length = 0x%x\n",
				(unsigned) mbi->mmap_addr, (unsigned) mbi->mmap_length);
		for (mmap = (memory_map_t *) mbi->mmap_addr;
				(unsigned long) mmap < mbi->mmap_addr + mbi->mmap_length;
				mmap = (memory_map_t *) ((unsigned long) mmap
					+ mmap->size + sizeof (mmap->size)))
			printf (" size = 0x%x,     base_addr = 0x%#x%#x\n"
					"     type = 0x%x,  length    = 0x%#x%#x\n",
					(unsigned) mmap->size,
					(unsigned) mmap->base_addr_high,
					(unsigned) mmap->base_addr_low,
					(unsigned) mmap->type,
					(unsigned) mmap->length_high,
					(unsigned) mmap->length_low);
	}

	/* Construct an LDT entry in the GDT */
	{
		seg_desc_t the_ldt_desc;
		the_ldt_desc.granularity    = 0;
		the_ldt_desc.opsize         = 1;
		the_ldt_desc.reserved       = 0;
		the_ldt_desc.avail          = 0;
		the_ldt_desc.present        = 1;
		the_ldt_desc.dpl            = 0x0;
		the_ldt_desc.sys            = 0;
		the_ldt_desc.type           = 0x2;

		SET_LDT_PARAMS(the_ldt_desc, &ldt, ldt_size);
		ldt_desc_ptr = the_ldt_desc;
		lldt(KERNEL_LDT);
	}

	/* Construct a TSS entry in the GDT */
	{
		seg_desc_t the_tss_desc;
		the_tss_desc.granularity    = 0;
		the_tss_desc.opsize         = 0;
		the_tss_desc.reserved       = 0;
		the_tss_desc.avail          = 0;
		the_tss_desc.seg_lim_19_16  = TSS_SIZE & 0x000F0000;
		the_tss_desc.present        = 1;
		the_tss_desc.dpl            = 0x0;
		the_tss_desc.sys            = 0;
		the_tss_desc.type           = 0x9;
		the_tss_desc.seg_lim_15_00  = TSS_SIZE & 0x0000FFFF;

		SET_TSS_PARAMS(the_tss_desc, &tss, tss_size);

		tss_desc_ptr = the_tss_desc;

		tss.ldt_segment_selector = KERNEL_LDT;
		tss.ss0 = KERNEL_DS;
		tss.esp0 = 0x800000; //8MB
		ltr(KERNEL_TSS);
	}
	//printf("Enabling Interrupts\n");


	
	/* Init the PIC */

	populate_idt();

	i8259_init();
	rtc_init();
	outb(PIT_MODIFY_LOW | PIT_MODIFY_HIGH | PIT_MODE_2 ,PIT_CTRL_ADDR);
	outb(PIT_DEFAULT_RATE_LOW,PIT_PORT0_CTRL_ADDR); //set low byte
	outb(PIT_DEFAULT_RATE_HIGH,PIT_PORT0_CTRL_ADDR); //set high byte

/*
 * pc speaker code from osdev
 *
	uint32_t Div;
	uint8_t tmp;
		 
	//Set the PIT to the desired frequency
	Div = 1193180 / 1000;
	outb(0x43, 0xb6);
	outb(0x42, (uint8_t) (Div) );
	outb(0x42, (uint8_t) (Div >> 8));
							 
	 //And play the sound using the PC speaker
	tmp = inb(0x61);
	if (tmp != (tmp | 3)) {
		outb(0x61, tmp | 3);
	}
*/
	disable_irq(0);



	/* Initialize devices, memory, filesystem, enable device interrupts on the
	 * PIC, any other initialization stuff... */

	/* Enable interrupts */
	/* Do not enable the following until after you have set up your
	 * IDT correctly otherwise QEMU will triple fault and simple close
	 * without showing you any output */


	//Enable interrupts for the first time
	printf("Enabling Interrupts\n");
	sti();

	//Initialize paging and mount the filesysem
	init_paging();
	printf("Mounting module 0 as read-only zbigfs filesystem\n");
	zbigfs_mount(zbigfs_location);


	//Initialize PCBs and then clear the terminal so it looks nice
	init_pcbs();
	term_clear();

	/* Spin (nicely, so we don't chew up cycles) */
	asm volatile(".1: hlt; jmp .1;");
}


