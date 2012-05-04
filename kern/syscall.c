/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/syscall.h>
#include <kern/console.h>
#include <kern/sched.h>
#include <kern/time.h>
#include <kern/e100.h>

// Print a string to the system console.
// The string is exactly 'len' characters long.
// Destroys the environment on memory errors.
static void
sys_cputs(const char *s, size_t len)
{
	// Check that the user has permission to read memory [s, s+len).
	// Destroy the environment if not.
	user_mem_assert(curenv, s, len, (PTE_P|PTE_U));

	// Print the string supplied by the user.
	cprintf("%.*s", len, s);
}

// Read a character from the system console without blocking.
// Returns the character, or 0 if there is no input waiting.
static int
sys_cgetc(void)
{
	return cons_getc();
}

// Returns the current environment's envid.
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

// Destroy a given environment (possibly the currently running environment).
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	env_destroy(e);
	return 0;
}

// Deschedule current environment and pick a different one to run.
static void
sys_yield(void)
{
	sched_yield();
}

// Allocate a new environment.
// Returns envid of new environment, or < 0 on error.  Errors are:
//	-E_NO_FREE_ENV if no free environment is available.
//	-E_NO_MEM on memory exhaustion.
static envid_t
sys_exofork(void)
{
	// Create the new environment with env_alloc(), from kern/env.c.
	// It should be left as env_alloc created it, except that
	// status is set to ENV_NOT_RUNNABLE, and the register set is copied
	// from the current environment -- but tweaked so sys_exofork
	// will appear to return 0.

	int r;
	struct Env *child = NULL;
	struct Env *parent = curenv;

	if ((r = env_alloc(&child, parent->env_id)) < 0)
		return r;

	child->env_status = ENV_NOT_RUNNABLE;
	child->env_tf = parent->env_tf;
	child->env_tf.tf_regs.reg_eax = 0;

	return child->env_id;
}

// Set envid's env_status to status, which must be ENV_RUNNABLE
// or ENV_NOT_RUNNABLE.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if status is not a valid status for an environment.
static int
sys_env_set_status(envid_t envid, int status)
{
	// Use the 'envid2env' function from kern/env.c to translate an
	// envid to a struct Env.
	// You should set envid2env's third argument to 1, which will
	// check whether the current environment has permission to set
	// envid's status.

	int r;
	struct Env *e = NULL;

	if ((status != ENV_RUNNABLE) && (status != ENV_NOT_RUNNABLE))
		return -E_INVAL;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	e->env_status = status;
	return 0;
}

// Set envid's trap frame to 'tf'.
// tf is modified to make sure that user environments always run at code
// protection level 3 (CPL 3) with interrupts enabled.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	// LAB 5: Your code here.
	// Remember to check whether the user has supplied us with a good
	// address!
	int r;
	struct Env *e = NULL;

	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;
	e->env_tf = *tf;

	// GD_UT is the user text segment selector.
	// External interrupts are controlled by the  
	// FL_IF flag bit of the %eflags register.
	e->env_tf.tf_cs = GD_UT | 3;
	e->env_tf.tf_eflags |= FL_IF;

	return 0;	
}

// Set the page fault upcall for 'envid' by modifying the corresponding struct
// Env's 'env_pgfault_upcall' field.  When 'envid' causes a page fault, the
// kernel will push a fault record onto the exception stack, then branch to
// 'func'.
//
// Returns 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	int r;
	struct Env *e = NULL;

	// Enable permission checking when looking up 
	// the environment ID of the target environment, 
	// since this is a "dangerous" system call.
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	e->env_pgfault_upcall = func;
	return 0;
}

// Allocate a page of memory and map it at 'va' with permission
// 'perm' in the address space of 'envid'.
// The page's contents are set to 0.
// If a page is already mapped at 'va', that page is unmapped as a
// side effect.
//
// perm -- PTE_U | PTE_P must be set, PTE_AVAIL | PTE_W may or may not be set,
//         but no other bits may be set.  See PTE_USER in inc/mmu.h.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
//	-E_INVAL if perm is inappropriate (see above).
//	-E_NO_MEM if there's no memory to allocate the new page,
//		or to allocate any necessary page tables.
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// Hint: This function is a wrapper around page_alloc() and
	//   page_insert() from kern/pmap.c.
	//   Most of the new code you write should be to check the
	//   parameters for correctness.
	//   If page_insert() fails, remember to free the page you
	//   allocated!

	int r;
	struct Env *e = NULL;
	struct Page *pp = NULL;

	// check if va >= UTOP, or va is not page-aligned.
	if ((va >= (void *) UTOP) || (PGOFF(va) != 0)) 
		return -E_INVAL;

	// check if perm is inappropriate.
	if (!(perm & (PTE_U|PTE_P)) || (perm & ~PTE_USER)) 
		return -E_INVAL;

	// check if environment envid doesn't currently exist, or
	// the caller doesn't have permission to change envid.
	if ((r = envid2env(envid, &e, 1)) < 0) 
		return r;

	// check if there's no memory to allocate the new page.
	if ((r = page_alloc(&pp)) < 0) 
		return r;

	// check if page table couldn't be allocated
	if ((r = page_insert(e->env_pgdir, pp, va, perm)) < 0) {
		page_decref(pp); // don't forget to free the page!
		return r;
	}

	// Set the page's contents to 0.
	memset(page2kva(pp), 0, PGSIZE);

	// Success!
	return 0;
}

// Map the page of memory at 'srcva' in srcenvid's address space
// at 'dstva' in dstenvid's address space with permission 'perm'.
// Perm has the same restrictions as in sys_page_alloc, except
// that it also must not grant write access to a read-only
// page.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if srcenvid and/or dstenvid doesn't currently exist,
//		or the caller doesn't have permission to change one of them.
//	-E_INVAL if srcva >= UTOP or srcva is not page-aligned,
//		or dstva >= UTOP or dstva is not page-aligned.
//	-E_INVAL is srcva is not mapped in srcenvid's address space.
//	-E_INVAL if perm is inappropriate (see sys_page_alloc).
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in srcenvid's
//		address space.
//	-E_NO_MEM if there's no memory to allocate any necessary page tables.
static int
sys_page_map(envid_t srcenvid, void *srcva,
	     envid_t dstenvid, void *dstva, int perm)
{
	// Hint: This function is a wrapper around page_lookup() and
	//   page_insert() from kern/pmap.c.
	//   Again, most of the new code you write should be to check the
	//   parameters for correctness.
	//   Use the third argument to page_lookup() to
	//   check the current permissions on the page.
	
	int r;
	struct Env *src = NULL, *dst = NULL;
	struct Page *pp = NULL;
	pte_t *pt = NULL;

	// check if srcva >= UTOP or srcva is not page-aligned,
  // or dstva >= UTOP or dstva is not page-aligned.
	if ((srcva >= (void *) UTOP) || (PGOFF(srcva) != 0) ||
	    (dstva >= (void *) UTOP) || (PGOFF(dstva) != 0))
		return -E_INVAL;

	// check if perm is inappropriate (see sys_page_alloc).
	if (!(perm & (PTE_U|PTE_P)) || (perm & ~PTE_USER))
		return -E_INVAL;

	// checkif srcenvid and/or dstenvid doesn't currently exist,
	// or the caller doesn't have permission to change one of them.
	if ((r = envid2env(srcenvid, &src, 1)) < 0) return r;
	if ((r = envid2env(dstenvid, &dst, 1)) < 0) return r;

	// check is srcva mapped in srcenvid's address space.
	pp = page_lookup(src->env_pgdir, srcva, &pt);
	if (!(*pt & PTE_P)) 
		return -E_INVAL;

	// check if (perm & PTE_W), but srcva is read-only in srcenvid's address space.
	if ((perm & PTE_W) && (!(*pt & PTE_W))) 
		return -E_INVAL;

	// Map the page of memory at 'srcva' in srcenvid's address space
	// at 'dstva' in dstenvid's address space with permission 'perm'.
	if ((r = page_insert(dst->env_pgdir, pp, dstva, perm)) < 0) 
		return r;

	// Success!
	return 0;
}

// Unmap the page of memory at 'va' in the address space of 'envid'.
// If no page is mapped, the function silently succeeds.
//
// Return 0 on success, < 0 on error.  Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist,
//		or the caller doesn't have permission to change envid.
//	-E_INVAL if va >= UTOP, or va is not page-aligned.
static int
sys_page_unmap(envid_t envid, void *va)
{
	// Hint: This function is a wrapper around page_remove().	
	int r;
	struct Env *e = NULL;

	// check if va >= UTOP, or va is not page-aligned.
	if ((va >= (void *) UTOP) || (PGOFF(va) != 0))
		return -E_INVAL;

	// check if environment envid doesn't currently exist,
	// or the caller doesn't have permission to change envid.
	if ((r = envid2env(envid, &e, 1)) < 0) 
		return r;

	// unmap the page of memory.
	page_remove(e->env_pgdir, va);

	// success!
	return 0;
}

// Try to send 'value' to the target env 'envid'.
// If srcva < UTOP, then also send page currently mapped at 'srcva',
// so that receiver gets a duplicate mapping of the same page.
//
// The send fails with a return value of -E_IPC_NOT_RECV if the
// target is not blocked, waiting for an IPC.
//
// The send also can fail for the other reasons listed below.
//
// Otherwise, the send succeeds, and the target's ipc fields are
// updated as follows:
//    env_ipc_recving is set to 0 to block future sends;
//    env_ipc_from is set to the sending envid;
//    env_ipc_value is set to the 'value' parameter;
//    env_ipc_perm is set to 'perm' if a page was transferred, 0 otherwise.
// The target environment is marked runnable again, returning 0
// from the paused sys_ipc_recv system call.  (Hint: does the
// sys_ipc_recv function ever actually return?)
//
// If the sender wants to send a page but the receiver isn't asking for one,
// then no page mapping is transferred, but no error occurs.
// The ipc only happens when no errors occur.
//
// Returns 0 on success, < 0 on error.
// Errors are:
//	-E_BAD_ENV if environment envid doesn't currently exist.
//		(No need to check permissions.)
//	-E_IPC_NOT_RECV if envid is not currently blocked in sys_ipc_recv,
//		or another environment managed to send first.
//	-E_INVAL if srcva < UTOP but srcva is not page-aligned.
//	-E_INVAL if srcva < UTOP and perm is inappropriate
//		(see sys_page_alloc).
//	-E_INVAL if srcva < UTOP but srcva is not mapped in the caller's
//		address space.
//	-E_INVAL if (perm & PTE_W), but srcva is read-only in the
//		current environment's address space.
//	-E_NO_MEM if there's not enough memory to map srcva in envid's
//		address space.
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	int r;
	struct Env *dst = NULL;
	struct Page *pp = NULL;
	pte_t *pte = NULL;

	if ((r = envid2env(envid, &dst, 0)) < 0)
		// target environment doesn't currently exist
		return r;

	if (!dst->env_ipc_recving || (dst->env_ipc_from != 0))
		// not currently blocked or another env managed to send first
		return -E_IPC_NOT_RECV;

	if ((srcva < (void *) UTOP) && (PGOFF(srcva) != 0))
		// srcva < UTOP but srcva is not page-aligned
		return -E_INVAL;

	if (srcva < (void *) UTOP) {
		// check that permissions are appropriate
		if (!(perm & PTE_U) || !(perm & PTE_P) || (perm & ~PTE_USER))
			return -E_INVAL;
	}

	if ((srcva < (void *) UTOP) && 
			((pp = page_lookup(curenv->env_pgdir, srcva, &pte)) == NULL))
		// page not mapped in the caller's address space.
		return -E_INVAL;

	if ((srcva < (void *) UTOP) && !(*pte & PTE_W) && (perm & PTE_W))
		// srcva is read-only in the current environment's address space.
		return -E_INVAL;

	if ((srcva < (void *) UTOP) && (dst->env_ipc_dstva)) {
		// send a page
		if ((r = page_insert(dst->env_pgdir, pp, dst->env_ipc_dstva, perm)) < 0)
			return r;
		dst->env_ipc_perm = perm;
	} else {
		dst->env_ipc_perm = 0;
	}

	// Deliver the message.
	dst->env_ipc_recving = 0; // block future requests
	dst->env_ipc_from = curenv->env_id;
	dst->env_ipc_value = value;
	dst->env_tf.tf_regs.reg_eax = 0;
	dst->env_status = ENV_RUNNABLE;

	return 0;
}

// Block until a value is ready.  Record that you want to receive
// using the env_ipc_recving and env_ipc_dstva fields of struct Env,
// mark yourself not runnable, and then give up the CPU.
//
// If 'dstva' is < UTOP, then you are willing to receive a page of data.
// 'dstva' is the virtual address at which the sent page should be mapped.
//
// This function only returns on error, but the system call will eventually
// return 0 on success.
// Return < 0 on error.  Errors are:
//	-E_INVAL if dstva < UTOP but dstva is not page-aligned.
static int
sys_ipc_recv(void *dstva)
{
	// If setting up shared page mapping, dstva must be page-aligned.
	if ((dstva < (void *) UTOP) && (PGOFF(dstva) != 0))
		return -E_INVAL;

	if (dstva < (void *) UTOP)
    curenv->env_ipc_dstva = dstva;
  else
    curenv->env_ipc_dstva = NULL;

	// Update fields of the current environment.
	curenv->env_ipc_recving = 1;
	curenv->env_ipc_from = 0;
  curenv->env_ipc_value = 0;
  curenv->env_ipc_perm = 0;	
	curenv->env_status = ENV_NOT_RUNNABLE;

	// Give up the CPU.
	curenv->env_tf.tf_regs.reg_eax = 0;
	sched_yield();
	
	// Success!
	return 0;
}

// Return the current time.
static int
sys_time_msec(void) 
{
	return time_msec();
}

// Transmit a packet with the E100 nic.
static int
sys_xmit_frame(const char *data, uint16_t len) {
	user_mem_assert(curenv, data, len, PTE_P);
	return e100_xmit_frame(data, len);
}

// Transmit a packet with the E100 nic.
static int
sys_rx(char *data) {
	return e100_rx(data);
}

// Dispatches to the correct kernel function, passing the arguments.
int32_t
syscall(uint32_t syscallno, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
	// Call the function corresponding to the 'syscallno' parameter.
	// Return any appropriate return value.
	switch(syscallno) {
		case SYS_cgetc:
			return sys_cgetc();

		case SYS_cputs:
			sys_cputs((const char *) a1, (size_t) a2);
			return 0;

		case SYS_env_destroy:
			return sys_env_destroy((envid_t) a1);

		case SYS_env_set_pgfault_upcall: 
			return sys_env_set_pgfault_upcall((envid_t) a1, (void *) a2);

		case SYS_env_set_status:
			return sys_env_set_status((envid_t) a1, (int) a2);

		case SYS_exofork:
			return sys_exofork();

		case SYS_getenvid:
			return sys_getenvid();

		case SYS_ipc_try_send:
			return sys_ipc_try_send((envid_t) a1, (uint32_t) a2, (void *) a3, (unsigned) a4);

		case SYS_ipc_recv:
			return sys_ipc_recv((void *) a1);

		case SYS_xmit_frame:
			return sys_xmit_frame((const char *) a1, (uint16_t) a2);

		case SYS_page_alloc:
			return sys_page_alloc((envid_t) a1, (void *) a2, (int) a3);

		case SYS_page_map:       	
			return sys_page_map((envid_t) a1, (void *) a2, (envid_t) a3, (void *) a4, (int) a5);

		case SYS_page_unmap:     	
			return sys_page_unmap((envid_t) a1, (void *) a2);

		case SYS_env_set_trapframe:
			return sys_env_set_trapframe((envid_t) a1, (struct Trapframe *) a2);

		case SYS_time_msec:
			return sys_time_msec();

		case SYS_rx:
			return sys_rx((char *) a1);

		case SYS_yield:
			sys_yield();
			return 0;

		default: 
			return -E_INVAL;
	}
}

