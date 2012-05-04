// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

// Assembly language pgfault entrypoint defined in lib/pfentry.S.
extern void _pgfault_upcall(void);

//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	int r, perm;
	void *addr = (void *) utf->utf_fault_va;	

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at vpt
	//   (see <inc/memlayout.h>).
	if (!(utf->utf_err & FEC_WR)) 
		panic("pgfault: faulting access not a write\n");

	if (!(vpt[VPN(addr)] & PTE_COW)) 
		panic("pgfault: faulting access not to a copy-on-write page\n");	

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.
	//   No need to explicitly delete the old page's mapping.
	addr = ROUNDDOWN(addr, PGSIZE);
	perm = PTE_P|PTE_U|PTE_W;

	if ((r = sys_page_alloc(0, PFTEMP, perm)) < 0) 
		panic("sys_page_alloc: %e\n", r);

	memmove(PFTEMP, addr, PGSIZE);

	if ((r = sys_page_map(0, PFTEMP, 0, addr, perm)) < 0) 
		panic("sys_page_map: %e\n", r);

	if ((r = sys_page_unmap(0, PFTEMP)) < 0) 
		panic("sys_page_unmap: %e\n", r);
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
// 
static int
duppage(envid_t envid, unsigned pn)
{
	int r, perm;
	pte_t pte = vpt[pn];
	void *addr = (void *) (pn*PGSIZE);

	if ((pte & PTE_W) || (pte & PTE_COW)) {
		perm =  PTE_P|PTE_U|PTE_COW; // mapping must be copy-on-write.
		// Map the page pn into the child at the same virtual address.
		if ((r = sys_page_map(0, addr, envid, addr, perm)) < 0)
			panic("sys_page_map: %e\n", r);

		// Our mapping must be marked copy-on-write in as well.
		if ((r = sys_page_map(0, addr, 0, addr, perm)) < 0)
    	panic("sys_page_map: %e\n", r);

	} else { // The page is not writable or copy-on-write.
		perm =  PTE_P|PTE_U;
		if ((r = sys_page_map(0, addr, envid, addr, perm)) < 0)
			panic("sys_page_map: %e\n", r);
	}
	// Success!
	return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use vpd, vpt, and duppage.
//   Remember to fix "env" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	int r;
	envid_t child;
	uint32_t pdeno, pteno, pn;	

	// Installs pgfault() as the C-level page fault handler.
	set_pgfault_handler(pgfault);

	// Allocate a new child environment.
	// The kernel will initialize it with a copy of our register state,
	// so that the child will appear to have called sys_exofork() too -
	// except that in the child, this "fake" call to sys_exofork()
	// will return 0 instead of the envid of the child.
	child = sys_exofork();
	if (child < 0)
		panic("sys_exofork: %e\n", child);

	if (child == 0) {
		// We're the child.
		// The copied value of the global variable 'env'
		// is no longer valid (it refers to the parent!).
		// Fix it and return 0.
		env = &envs[ENVX(sys_getenvid())];
		return child;
	}

	// We're the parent.
	for (pdeno = 0; pdeno < PDX(UTOP); ++pdeno) {
		// For each writable or copy-on-write page in its address space below UTOP, 
		// the parent maps the page copy-on-write into the address space of the 
		// child and then remaps the page copy-on-write in its own address space. 
		// The parent sets both PTEs so that the page is not writeable, and to 
		// contain PTE_COW in the "avail" field to distinguish copy-on-write pages 
		// from genuine read-only pages. 

		// Only look at mapped page tables.
		if (!(vpd[pdeno] & PTE_P))
			continue;

		// Map all PTEs in this page table.
		// The exception stack is not remapped this way, see below.
		for (pteno = 0; pteno <= PTX(~0); ++pteno) {
			pn = pteno + (pdeno << (PDXSHIFT - PTXSHIFT));
      if ((vpt[pn] & PTE_P) && (pn < VPN(UXSTACKTOP - PGSIZE))) 
      	duppage(child, pn);             
		}
	}

	// Allocate a fresh page in the child for the exception stack.
	// Since the page fault handler will be doing the actual copying 
	// and the page fault handler runs on the exception stack, the 
	// exception stack cannot be made copy-on-write.
	if ((r = sys_page_alloc(child, (void *) (UXSTACKTOP - PGSIZE), PTE_P|PTE_U|PTE_W)) < 0)
		panic("sys_page_alloc: %e\n", r);

	// Set the user page fault entrypoint for the child to look like its own.
	if ((r = sys_env_set_pgfault_upcall(child, _pgfault_upcall)) < 0)
		panic("sys_env_set_pgfault_upcall: %e\n", r);

	// Start the child environment running
	if ((r = sys_env_set_status(child, ENV_RUNNABLE)) < 0)
		panic("sys_env_set_status: %e\n", r);

	return child;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
