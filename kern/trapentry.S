/* See COPYRIGHT for copyright information. */

#include <inc/mmu.h>
#include <inc/memlayout.h>
#include <inc/trap.h>

#include <kern/picirq.h>


###################################################################
# exceptions/interrupts
###################################################################

/* The TRAPHANDLER macro defines a globally-visible function for handling
 * a trap.  It pushes a trap number onto the stack, then jumps to _alltraps.
 * Use TRAPHANDLER for traps where the CPU automatically pushes an error code.
 */ 
#define TRAPHANDLER(name, num)						\
	.globl name;		/* define global symbol for 'name' */	\
	.type name, @function;	/* symbol type is function */		\
	.align 2;		/* align function definition */		\
	name:			/* function starts here */		\
	pushl $(num);							\
	jmp _alltraps

/* Use TRAPHANDLER_NOEC for traps where the CPU doesn't push an error code.
 * It pushes a 0 in place of the error code, so the trap frame has the same
 * format in either case.
 */
#define TRAPHANDLER_NOEC(name, num)					\
	.globl name;							\
	.type name, @function;						\
	.align 2;							\
	name:								\
	pushl $0;							\
	pushl $(num);							\
	jmp _alltraps

.text
	# exception handlers
	TRAPHANDLER_NOEC(divide_error,           T_DIVIDE)
	TRAPHANDLER_NOEC(debug,                  T_DEBUG)
	TRAPHANDLER_NOEC(nmi,                    T_NMI)
	TRAPHANDLER_NOEC(int3,                   T_BRKPT)
	TRAPHANDLER_NOEC(overflow,               T_OFLOW)
	TRAPHANDLER_NOEC(bounds,                 T_BOUND)
	TRAPHANDLER_NOEC(invalid_op,             T_ILLOP)
	TRAPHANDLER_NOEC(device_not_available,   T_DEVICE)
	TRAPHANDLER     (doublefault_fn,         T_DBLFLT)
	TRAPHANDLER     (invalid_TSS,            T_TSS)
	TRAPHANDLER     (segment_not_present,    T_SEGNP)
	TRAPHANDLER     (stack_segment,          T_STACK)
	TRAPHANDLER     (general_protection,     T_GPFLT)
	TRAPHANDLER     (page_fault,             T_PGFLT)
	TRAPHANDLER_NOEC(coprocessor_error,      T_FPERR)
	TRAPHANDLER_NOEC(alignment_check,        T_ALIGN)
	TRAPHANDLER_NOEC(machine_check,          T_MCHK)
	TRAPHANDLER_NOEC(simd_coprocessor_error, T_SIMDERR)
	TRAPHANDLER_NOEC(system_call,            T_SYSCALL)

	# irq handlers
	TRAPHANDLER_NOEC(irq0  ,IRQ_OFFSET)
	TRAPHANDLER_NOEC(irq1  ,IRQ_OFFSET+1)
	TRAPHANDLER_NOEC(irq2  ,IRQ_OFFSET+2)
	TRAPHANDLER_NOEC(irq3  ,IRQ_OFFSET+3)
	TRAPHANDLER_NOEC(irq4  ,IRQ_OFFSET+4)
	TRAPHANDLER_NOEC(irq5  ,IRQ_OFFSET+5)
	TRAPHANDLER_NOEC(irq6  ,IRQ_OFFSET+6)
	TRAPHANDLER_NOEC(irq7  ,IRQ_OFFSET+7)
	TRAPHANDLER_NOEC(irq8  ,IRQ_OFFSET+8)
	TRAPHANDLER_NOEC(irq9  ,IRQ_OFFSET+9)
	TRAPHANDLER_NOEC(irq10 ,IRQ_OFFSET+10)
	TRAPHANDLER_NOEC(irq11 ,IRQ_OFFSET+11)
	TRAPHANDLER_NOEC(irq12 ,IRQ_OFFSET+12)
	TRAPHANDLER_NOEC(irq13 ,IRQ_OFFSET+13)
	TRAPHANDLER_NOEC(irq14 ,IRQ_OFFSET+14)
	TRAPHANDLER_NOEC(irq15 ,IRQ_OFFSET+15)

_alltraps:
	# Push values to make the stack look like a struct Trapframe
	pushl %ds
	pushl %es		  
	pushal

	# Load GD_KD into %ds and %es
	movw $GD_KD, %ax
	movw %ax, %ds
	movw %ax, %es	

	# Pushl %esp to pass a pointer to the Trapframe as an argument to trap()
	pushl %esp

	# Call trap 
	call trap

	# Restore the saved old CPU state
	popl %esp
	popal
	popl %es
	popl %ds

	# Return from the exception
	iret
