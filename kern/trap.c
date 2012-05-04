#include <inc/mmu.h>
#include <inc/x86.h>
#include <inc/assert.h>

#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/env.h>
#include <kern/syscall.h>
#include <kern/sched.h>
#include <kern/kclock.h>
#include <kern/picirq.h>
#include <kern/time.h>

static struct Taskstate ts;

/* Interrupt descriptor table.  (Must be built at run time because
 * shifted function addresses can't be represented in relocation records.)
 */
struct Gatedesc idt[256] = { { 0 } };
struct Pseudodesc idt_pd = {
	sizeof(idt) - 1, (uint32_t) idt
};


static const char *trapname(int trapno)
{
	static const char * const excnames[] = {
		"Divide error",
		"Debug",
		"Non-Maskable Interrupt",
		"Breakpoint",
		"Overflow",
		"BOUND Range Exceeded",
		"Invalid Opcode",
		"Device Not Available",
		"Double Fault",
		"Coprocessor Segment Overrun",
		"Invalid TSS",
		"Segment Not Present",
		"Stack Fault",
		"General Protection",
		"Page Fault",
		"(unknown trap)",
		"x87 FPU Floating-Point Error",
		"Alignment Check",
		"Machine-Check",
		"SIMD Floating-Point Exception"
	};

	if (trapno < sizeof(excnames)/sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call";
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}

void
idt_init(void)
{
	extern struct Segdesc gdt[];
	
	// The IDT must be properly initialized with an exception 
	// handler function for each recognized exception.
	SETGATE(idt[T_DIVIDE],  0, GD_KT, divide_error,           0);
	SETGATE(idt[T_DEBUG],   0, GD_KT, debug,                  0);
	SETGATE(idt[T_NMI],     0, GD_KT, nmi,                    0);
	SETGATE(idt[T_BRKPT],   0, GD_KT, int3,                   3);
	SETGATE(idt[T_OFLOW],   0, GD_KT, overflow,               3);
	SETGATE(idt[T_BOUND],   0, GD_KT, bounds,                 3);
	SETGATE(idt[T_ILLOP],   0, GD_KT, invalid_op,             0);
	SETGATE(idt[T_DEVICE],  0, GD_KT, device_not_available,   0);
	SETGATE(idt[T_DBLFLT],  0, GD_KT, doublefault_fn,         0);
	SETGATE(idt[T_TSS],     0, GD_KT, invalid_TSS,            0);
	SETGATE(idt[T_SEGNP],   0, GD_KT, segment_not_present,    0);
	SETGATE(idt[T_STACK],   0, GD_KT, stack_segment,          0);
	SETGATE(idt[T_GPFLT],   0, GD_KT, general_protection,     0);
	SETGATE(idt[T_PGFLT],   0, GD_KT, page_fault,             0);
	SETGATE(idt[T_FPERR],   0, GD_KT, coprocessor_error,      0);
	SETGATE(idt[T_ALIGN],   0, GD_KT, alignment_check,        0);
	SETGATE(idt[T_MCHK],    0, GD_KT, machine_check,          0);
	SETGATE(idt[T_SIMDERR], 0, GD_KT, simd_coprocessor_error, 0);
	SETGATE(idt[T_SYSCALL], 0, GD_KT, system_call,            3);

	// IRQ handlers
	SETGATE(idt[IRQ_OFFSET],    0, GD_KT, irq0,  0);
	SETGATE(idt[IRQ_OFFSET+1],  0, GD_KT, irq1,  0);
	SETGATE(idt[IRQ_OFFSET+2],  0, GD_KT, irq2,  0);
	SETGATE(idt[IRQ_OFFSET+3],  0, GD_KT, irq3,  0);
	SETGATE(idt[IRQ_OFFSET+4],  0, GD_KT, irq4,  0);
	SETGATE(idt[IRQ_OFFSET+5],  0, GD_KT, irq5,  0);
	SETGATE(idt[IRQ_OFFSET+6],  0, GD_KT, irq6,  0);
	SETGATE(idt[IRQ_OFFSET+7],  0, GD_KT, irq7,  0);
	SETGATE(idt[IRQ_OFFSET+8],  0, GD_KT, irq8,  0);
	SETGATE(idt[IRQ_OFFSET+9],  0, GD_KT, irq9,  0);
	SETGATE(idt[IRQ_OFFSET+10], 0, GD_KT, irq10, 0);
	SETGATE(idt[IRQ_OFFSET+11], 0, GD_KT, irq11, 0);
	SETGATE(idt[IRQ_OFFSET+12], 0, GD_KT, irq12, 0);
	SETGATE(idt[IRQ_OFFSET+13], 0, GD_KT, irq13, 0);
	SETGATE(idt[IRQ_OFFSET+14], 0, GD_KT, irq14, 0);
	SETGATE(idt[IRQ_OFFSET+15], 0, GD_KT, irq15, 0);

	// Setup a TSS so that we get the right stack
	// when we trap to the kernel.
	ts.ts_esp0 = KSTACKTOP;
	ts.ts_ss0 = GD_KD;

	// Initialize the TSS field of the gdt.
	gdt[GD_TSS >> 3] = SEG16(STS_T32A, (uint32_t) (&ts),
					sizeof(struct Taskstate), 0);
	gdt[GD_TSS >> 3].sd_s = 0;

	// Load the TSS
	ltr(GD_TSS);

	// Load the IDT
	asm volatile("lidt idt_pd");
}

void
print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p\n", tf);
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	cprintf("  err  0x%08x\n", tf->tf_err);
	cprintf("  eip  0x%08x\n", tf->tf_eip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	cprintf("  esp  0x%08x\n", tf->tf_esp);
	cprintf("  ss   0x----%04x\n", tf->tf_ss);
}

void
print_regs(struct PushRegs *regs)
{
	cprintf("  edi  0x%08x\n", regs->reg_edi);
	cprintf("  esi  0x%08x\n", regs->reg_esi);
	cprintf("  ebp  0x%08x\n", regs->reg_ebp);
	cprintf("  oesp 0x%08x\n", regs->reg_oesp);
	cprintf("  ebx  0x%08x\n", regs->reg_ebx);
	cprintf("  edx  0x%08x\n", regs->reg_edx);
	cprintf("  ecx  0x%08x\n", regs->reg_ecx);
	cprintf("  eax  0x%08x\n", regs->reg_eax);
}

static void
trap_dispatch(struct Trapframe *tf)
{
	// Handle processor exceptions and interrupts.
	uint32_t trap = tf->tf_trapno;

	switch(trap) {
		case T_PGFLT:
			page_fault_handler(tf);
			return;

		case T_BRKPT:
			monitor(tf);
			return;

		case T_SYSCALL:
			tf->tf_regs.reg_eax = 
				syscall(tf->tf_regs.reg_eax, tf->tf_regs.reg_edx, tf->tf_regs.reg_ecx, 
						tf->tf_regs.reg_ebx, tf->tf_regs.reg_edi, tf->tf_regs.reg_esi);
			return;

		// Handle clock interrupts.
		case IRQ_OFFSET+IRQ_TIMER:
			time_tick(); // time tick increment
			sched_yield(); // run a different environment
			return;

		// Handle spurious interrupts
		// The hardware sometimes raises these because of noise on the
		// IRQ line or other reasons. We don't care.			
		case IRQ_OFFSET+IRQ_SPURIOUS:
			cprintf("Spurious interrupt on irq 7\n");
			print_trapframe(tf);
			return;
	}

	// Unexpected trap: The user process or the kernel has a bug.
	print_trapframe(tf);
	if (tf->tf_cs == GD_KT)
		panic("unhandled trap in kernel");
	else {
		env_destroy(curenv);
		return;
	}
}

void
trap(struct Trapframe *tf)
{
	// The environment may have set DF and some versions
	// of GCC rely on DF being clear
	asm volatile("cld" ::: "cc");

	// Check that interrupts are disabled.  If this assertion
	// fails, DO NOT be tempted to fix it by inserting a "cli" in
	// the interrupt path.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3) {
		// Trapped from user mode.
		// Copy trap frame (which is currently on the stack)
		// into 'curenv->env_tf', so that running the environment
		// will restart at the trap point.
		assert(curenv);
		curenv->env_tf = *tf;
		// The trapframe on the stack should be ignored from here on.
		tf = &curenv->env_tf;
	}
	
	// Dispatch based on what type of trap occurred
	trap_dispatch(tf);

	// If we made it to this point, then no other environment was
	// scheduled, so we should return to the current environment
	// if doing so makes sense.
	if (curenv && curenv->env_status == ENV_RUNNABLE)
		env_run(curenv);
	else
		sched_yield();
}


void
page_fault_handler(struct Trapframe *tf)
{
	uint32_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.	
	if ((tf->tf_cs & 3) == 0)
		panic("page_fault_handler: page fault occured in kernel");

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// The trap handler needs one word of scratch space at the top of the
	// trap-time stack in order to return.  In the non-recursive case, we
	// don't have to worry about this because the top of the regular user
	// stack is free.  In the recursive case, this means we have to leave
	// an extra word between the current top of the exception stack and
	// the new stack frame because the exception stack _is_ the trap-time
	// stack.
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').
	
	if (curenv->env_pgfault_upcall) { // A page fault handler is registered.
		void *utf_va;
		struct UTrapframe *utf;

		// If the user environment is already running on the user exception stack
		// when an exception ocurs, then the page fault handler itself has faulted.
		// We can test for this if tf->tf_esp is already on the user exception stack.
		if ((UXSTACKTOP - PGSIZE <= tf->tf_esp) && (tf->tf_esp <= UXSTACKTOP)) {
			// Need one word of scratch space at the top of the trap-time stack.
			uint32_t scratch = 4; // bytes
			utf_va = (void *) tf->tf_esp - scratch - sizeof(struct UTrapframe);
		} else { // Switch our ESP to point to the user exception stack.
			utf_va = (void *) UXSTACKTOP - sizeof(struct UTrapframe);
		}
		utf = (struct UTrapframe *) utf_va;

		// Do we have enough space on the user exception stack?
		user_mem_assert(curenv, utf, sizeof(struct UTrapframe), PTE_P|PTE_U|PTE_W);	

		// Set up a trap frame on the exception stack 
		// that looks like a struct UTrapframe.
		utf->utf_fault_va = fault_va;
		utf->utf_err      = tf->tf_err;
		utf->utf_regs     = tf->tf_regs;		
		utf->utf_eip      = tf->tf_eip;
		utf->utf_eflags   = tf->tf_eflags;
		utf->utf_esp      = tf->tf_esp;

		// Push the UTrapframe onto the user exception stack.
		tf->tf_esp = (uintptr_t) utf;

		// Call up to the appropriate page fault handler.
		tf->tf_eip = (uintptr_t) curenv->env_pgfault_upcall;
		env_run(curenv);
	} 	

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n",
		curenv->env_id, fault_va, tf->tf_eip);
	print_trapframe(tf);
	env_destroy(curenv);
}

