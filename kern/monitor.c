// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/mmu.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>
#include <kern/trap.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line


struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display a backtrace of the stack", mon_backtrace },
	{ "showmappings", "Display physical mappings for a range of virtual addresses", mon_showmappings },
	{ "chperm", "Change permissions for a given virtual addresse", mon_chperm },
	{ "hexdump", "Dump contents of a range of memory", mon_hexdump },
	{ "palloc", "Allocate a page of physical memory", mon_palloc },
	{ "pfree", "Free a page of physical memory", mon_pfree },
	{ "pstatus", "Display the status of a page of physical memory", mon_pstatus }
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

unsigned read_eip();

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start %08x (virt)  %08x (phys)\n", _start, _start - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		(end-_start+1023)/1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	struct Eipdebuginfo info;
	uint32_t *ebp = (uint32_t *) read_ebp();

	cprintf("Stack backtrace:\n");
	while (ebp != 0) {
		// Get address data from the function call frame
		uint32_t eip  = ebp[1];
		uint32_t arg1 = ebp[2];
		uint32_t arg2 = ebp[3];
		uint32_t arg3 = ebp[4];
		uint32_t arg4 = ebp[5];
		uint32_t arg5 = ebp[6];

		// Display function call frame addresses
		cprintf("ebp %08x eip %08x args %08x %08x %08x %08x %08x\n", 
				ebp, eip, arg1, arg2, arg3, arg4, arg5);

		// Get more data to include in output from current eip address
		debuginfo_eip(eip, &info);

		// Get function name
		char eip_fn_name[30];
		strncpy(eip_fn_name, info.eip_fn_name, info.eip_fn_namelen);
		eip_fn_name[info.eip_fn_namelen] = '\0';

		// Display additional information
		cprintf("      %s:%d: %s+%x\n", 
				info.eip_file, 
				info.eip_line,
				eip_fn_name,
				eip - info.eip_fn_addr);
		
		// Follow the chain of saved ebp pointers up to the next caller
		ebp = (uint32_t *) *ebp;
	}

	return 0;
}

// Challenge!
// Display all of the physical mappings (or lack thereof) that apply to a
// particular range of virtual/linear addresses in the currently active address
// space.
//
// Examaple usage: 'showmappings 0x3000 0x5000'
// This would display the physical page mappings and the corresponding
// permission bits that apply to the pages at virtual addresses 0x3000, 0x4000,
// 0x5000.
int
mon_showmappings(int argc, char **argv, struct Trapframe *tf) 
{
	if (argc != 3) {
		cprintf("Usage: showmappings [LOWER] [UPPER]\n");
		return 0;
	}

	uintptr_t  va = 0;
	physaddr_t pa = 0;

	// Get the lower and upper bound on the range we want to display.
	// see /lib/string.c for strtol definition.
	uintptr_t lower = strtol(argv[1], NULL, 0);
	uintptr_t upper = strtol(argv[2], NULL, 0);

	// Align addresses to be page aligned
	upper = ROUNDUP(upper, PGSIZE);
	lower = ROUNDDOWN(lower, PGSIZE);

	// Display header
	cprintf("----------------------------------\n"
	        "VIRTUAL    PHYSICAL   P W U A D PS\n"
	        "----------------------------------\n");

	for (va = lower; va <= upper; va += PGSIZE) {		

		// Walk the two-level page table structure and get a pointer 
		// to the page table entry (PTE) for linear address 'va'.
		pte_t *pte = pgdir_walk(boot_pgdir, (void *) va, 0);
			
		// The Table field acts as an index and determines the entry in the
		// page table that contains the physical address of the page frame 
		// containing the page. The offset field then determines the relative 
		// position within the page giving us the physical address we want.
		if ((pte) && (*pte & PTE_P)) {
			pa = PTE_ADDR(*pte) + PGOFF(va);			

			// Page Table Entry fields to display
			char p = *pte & PTE_P ? '1' : '0';
			char w = *pte & PTE_W ? '1' : '0';
			char u = *pte & PTE_U ? '1' : '0';
			char a = *pte & PTE_A ? '1' : '0';
			char d = *pte & PTE_D ? '1' : '0';
			char ps = *pte & PTE_PS ? '1' : '0';

			// Display page mapping and permission fields.
			cprintf("0x%08x 0x%08x %c %c %c %c %c  %c\n", va, pa, p, w, u, a, d, ps);			
		}             
		else cprintf("0x%08x ---------- - - - - -  -\n", va);		
	}

	return 0;
}

// Challenge!
// Explicitly set, clear, or change the permissions of any mapping in the 
// current address space. 
//
// Permissions are represented by a one-digit octal number:
//     3 = user/read
//     2 = user/write
//     1 = supervisor/read
//     0 = supervisor/write
//
// Examaple usage: 'chperm 3 0x3000'
int
mon_chperm(int argc, char **argv, struct Trapframe *tf) 
{
	if (argc != 3) {
		cprintf("Usage: chperm [PERM] [ADDRESS]\n"
				    "  PERM is represented by a number:\n"
						"  3  user/read\n"
						"  2  user/write\n"
						"  1  supervisor/read\n"
						"  0  supervisor/write\n");
		return 0;
	}

	// Get the new permissions and the address to change.
	// see /lib/string.c for strtol definition.
	uint32_t perm = strtol(argv[1], NULL, 0);
	uintptr_t va  = strtol(argv[2], NULL, 0);

	// Walk the two-level page table structure and get a pointer 
	// to the page table entry (PTE) for linear address 'va'.
	pte_t *pte = pgdir_walk(boot_pgdir, (void *) va, 0);

	if ((pte) && (*pte & PTE_P)) {

		// Set permissions
		switch (perm) {
			case 0: // S/W
				*pte |= PTE_W;
				*pte |= PTE_U;
				break;
			case 1: // S/R
				*pte &= ~PTE_W;
				*pte |= PTE_U;
				break;
			case 2: // U/W
				*pte |= PTE_W;
				*pte &= ~PTE_U;
				break;
			case 3: // U/R
				*pte &= ~PTE_W;
				*pte &= ~PTE_U;
				break;
			default:
				cprintf("chperm: invalid permission\n", va);
		}			
	} 
	else cprintf("chperm: 0x%08x unmapped\n", va);	

	return 0;
}

// Challenge!
// Dumps the contents of a range of memory given either a virtual 
// or physical address range.
// Examaple usage: 'hexdump 0x3000 0x5000'
int
mon_hexdump(int argc, char **argv, struct Trapframe *tf) 
{
	if (argc < 3) {
		cprintf("Usage: hexdump [LOWER] [UPPER] [OPTION]\n"
						"  -p  treat address range as physical addresses\n");
		return 0;
	}

	// Get the lower and upper bound on the range we want to display.
	// see /lib/string.c for strtol definition.
	uintptr_t lower = strtol(argv[1], NULL, 0);
	uintptr_t upper = strtol(argv[2], NULL, 0);
	uintptr_t va;

	// If the range of addresses are physical addresses, we need to get 
	// the corresponding kernel virtual addresses.
	if ((argc == 4) && (strncmp(argv[3], "-p", 2) == 0)) {
		lower = (uintptr_t) KADDR(lower);
		upper = (uintptr_t) KADDR(upper);
	}

	// Align addresses to be 16-byte aligned
	upper = ROUNDUP(upper, 16);
	lower = ROUNDDOWN(lower, 16);

	int i = 0;
	char ascii[17];
	ascii[16] = '\0';

	for (va = lower; va <= upper-1; ++va) {

		// Walk the two-level page table structure and get a pointer 
		// to the page table entry (PTE) for linear address 'va'.
		pte_t *pte = pgdir_walk(boot_pgdir, (void *) va, 0);

		// If start of a 16-byte row, print the address.
		if (i == 0) cprintf("%08x ", va);

		// If page is present, display the two digit hex number representing
		// it and store the asci value if it is a printable character.
		if ((pte) && (*pte & PTE_P)) {
			uint8_t val = *(uint8_t *) va;
			cprintf("%02x", val);
			ascii[i] = (0x21 <= val && val <= 0x7e) ? val : '.';
		}
		else cprintf("--");
		++i;

		// If end of current 16-byte row, display ascii text translation
		if ((i % 16) == 0) {
			cprintf("  %s\n", ascii);
			i = 0;
		}
		else cprintf(" ");
	}
	// Need another newline?
	if ((i % 16) != 0) cprintf("\n");

	return 0;
}

// Challenge!
// Explicitly allocate a page.
int
mon_palloc(int argc, char **argv, struct Trapframe *tf)
{
	if (argc != 1) {
		cprintf("Usage: palloc\n");
		return 0;
	}

	struct Page *pp = NULL;

	// If allocation successfull, increment the reference 
	// count and return the physical address of the page.
	if (page_alloc(&pp) == 0) {
		++(pp->pp_ref);
		cprintf("palloc: 0x%08x\n", page2pa(pp));
	}
	else // Allocation failed, out of memory!
		cprintf("palloc: allocation failed\n");

	return 0;
}

// Challenge!
// Explicitly free a page.
int
mon_pfree(int argc, char **argv, struct Trapframe *tf)
{
	if (argc != 2) {
		cprintf("Usage: pfree [PA]\n");
		return 0;
	}

	// Get the physical address of the page to free.
	physaddr_t pa = strtol(argv[1], NULL, 0);

	// Get a pointer to the page and decrement the reference
	// count on it freeing it if there are no more refs.
	struct Page *pp = pa2page(pa);
	page_decref(pp);

	return 0;
}

// Challenge!
// Display whether or not any given page of physical memory
// is currently allocated.
int
mon_pstatus(int argc, char **argv, struct Trapframe *tf)
{
	if (argc != 2) {
		cprintf("Usage: pstatus [PA]\n");
		return 0;
	}

	// Get the physical address of the page to get status of.
	physaddr_t pa = strtol(argv[1], NULL, 0);

	// Get a pointer to the page. If the reference count is
	// not zero the page is allocated, otherwise its free.
	struct Page *pp = pa2page(pa);
	if (pp->pp_ref) cprintf("pstatus: allocated\n");
	else cprintf("pstatus: free\n");

	return 0;
}

/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}

// return EIP of caller.
// does not work if inlined.
// putting at the end of the file seems to prevent inlining.
unsigned
read_eip()
{
	uint32_t callerpc;
	__asm __volatile("movl 4(%%ebp), %0" : "=r" (callerpc));
	return callerpc;
}
