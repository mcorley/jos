#include <inc/x86.h>
#include <inc/string.h>

#include <kern/e100.h>
#include <kern/pmap.h>

struct nic e100; // E100 network interface card data

int 
e100_pci_attach(struct pci_func *pcif)
{ 
	// The device has been found but needs to be enabled.
	pci_func_enable(pcif);

	// Record the IRQ line and base I/O port assigned to the device 
	// so we'll be able to communicate with the E100.
	e100.io_base = pcif->reg_base[E100_IO];
	e100.irq_line = pcif->irq_line;

	e100_init();
	return 0;
}

void 
e100_init(void)
{
	// Reset the device preparing it for normal operation.
	e100_software_reset();

	// Create the receive and transmit DMA rings.
	e100_cbl_alloc();
	e100_rfa_alloc();
	
	// Tell the CU where to find the CBL by sending it the
	// physical address of the first buffer in the ring.
	outl(e100.io_base + CSR_SCB_GEN_PTR, e100.cbs->pa);
	// When the CU detects the CU Start (CU_START) command, it begins 
	// executing the first action command in the list.
	e100_exec_cmd(CSR_SCB_COMMAND, CUC_START);

	// Tell the RU where to find the RFA by sending it the
	// physical address of the first buffer in the ring.
	outl(e100.io_base + CSR_SCB_GEN_PTR, e100.rfds->pa);
	// For the RU start (RU_START) command, the CPU activates the RU 
	// for frame reception.
	e100_exec_cmd(CSR_SCB_COMMAND, RUC_START);	
}

void 
e100_software_reset(void)
{
	outl(e100.io_base + CSR_PORT, PORT_SOFTWARE_RESET);
	// Software must wait for ten system clocks and five transmit 
	// clocks before accessing the device (approximately 10us in 
	// software) after a reset is performed.
	udelay(10);
}

void
e100_exec_cmd(uint8_t csr, uint8_t cmd)
{
	int scb_command;
	outb(e100.io_base + csr, cmd);
	do {
		scb_command = inb(e100.io_base + CSR_SCB_COMMAND);
	} while (scb_command != 0);
}

void udelay(int loops)
{
	int i;
	for (i = 0; i < loops; ++i) 
		inb(0x84); // Approximately 1.25 us;
}

// --------------------------------------------------------------
// Packet TX 
// --------------------------------------------------------------

void
e100_tx_clean(void)
{
	// Clean CBs marked complete. The C bit indicates that the 
	// transmit DMA has completed processing the last byte of 
	// data associated with the TCB.	
	while ((e100.cbs_avail <= CBLSIZE) && 
			((e100.cb_to_clean->status & CB_COMPLETE))) {
		e100.cb_to_clean = e100.cb_to_clean->next;
		++e100.cbs_avail;
	}
}

int
e100_xmit_prepare(const char *data, uint16_t len, uint16_t flag)
{	
	// 
	// Place the packet into the next available buffer in the ring
	// and prepare it to be sent to the CU for transport.
	//

	e100.cb_to_use = e100.cb_to_use->next;
	--e100.cbs_avail;

	e100.cb_to_use->status = 0;
	e100.cb_to_use->command = CB_TX | flag;
	e100.cb_to_use->u.tcb.tbd_array = 0xffffffff;
	e100.cb_to_use->u.tcb.tcb_byte_count = len;
	e100.cb_to_use->u.tcb.threshold = 0xe0;
	e100.cb_to_use->u.tcb.tbd_count = 0;
	memmove(e100.cb_to_use->u.tcb.data, data, len);

	return 0;
}

//
// Transmits a packet of data in simple mode. The simplified structure expects 
// the transmit data to reside entirely in the memory space immediately after 
// the transmit command block (TCB).
// If there are no more empty slots in the transmit DMA ring we simply 
// drop the packet to avoid possible deadlock situations that might rise
// up if we pause the environment to allow the card to catch up.
//
// RETURNS
// 	0 on success
// 	-E_CBL_FULL if no more empty slots in cbl
//
int
e100_xmit_frame(const char *data, uint16_t len)
{
	// Reclaim CBs in the CBL that were successfully 
	// sent over the wire by the CU.
	e100_tx_clean();

	// If there are no more empty slots in the 
	// transmit DMA ring drop the packet.
	if (e100.cbs_avail == 0)
		return -E_CBL_FULL;

	// Places the packet into the next available buffer in the ring.
	// Clear the S bit on the current CB so the CU proceeds to execute 
	// the new CB when we resume CU operation.	
	e100.cb_to_use->command &= ~CB_S;
	e100_xmit_prepare(data, len, CB_S);

	int scb_status = inb(e100.io_base + CSR_SCB_STATUS);
	if ((scb_status & CUS_MASK) == CUS_SUSPENDED) {
		// If the CU is in the suspended state the CU Resume 
		// command resumes CU operation and requests the beginning 
		// of the next CB if the S bit is clear on current CB.		
		e100_exec_cmd(CSR_SCB_COMMAND, CUC_RESUME);
	}

	return 0;
}

//
// Allocate the command block list.
//
// Initialize the CBL setting each cb's link pointer to point to the next CB 
// in the ring. The pointers need to be physical addresses because a DMA ring 
// is created to be used by the device and a device on the PCI bus does not 
// have access to the CPU's MMU to translate virtual addresses into physical 
// addresses.
//
void
e100_cbl_alloc(void) 
{
	int i, r;
	struct Page *pp;
	struct cb *cb = NULL, *tail = NULL;

	for (i = 0; i < CBLSIZE; i++) {
		// Allocate a page for each command block.
		// Must zero out the contents of the page and
		// increment the reference count for it.
		if ((r = page_alloc(&pp)) < 0) 
			panic("e100_cbl_alloc: %e\n", r);		
		memset(page2kva(pp), 0, PGSIZE);
		++pp->pp_ref;

		cb = page2kva(pp);
		cb->pa = page2pa(pp);

		if (i == 0) {
			e100.cbs = cb;
		} else {
			// Extend the CBL by inserting the CB 
			// after the current tail in the list.
			tail->link = cb->pa;
			tail->next = cb;
			cb->prev = tail;
		}
		// Set the new tail.
		tail = cb;
	}
	// Complete the ring.
	tail->link = e100.cbs->pa;
	tail->next = e100.cbs;
	e100.cbs->prev = tail;

	e100.cbs_avail = CBLSIZE;	
	e100.cb_to_clean = e100.cbs;
	e100.cb_to_use = tail;

	// Append a nop command in the cbl with suspend bit set.
	e100.cb_to_use->command = CB_NOP|CB_S;
	--e100.cbs_avail;
}

// --------------------------------------------------------------
// Packet RX
// --------------------------------------------------------------

void
e100_rx_clean(void)
{
	// Clean RFDs marked complete. This bit indicates the completion 
	// of frame reception. It is set by the device.	
	while ((e100.rfds_avail <= RFASIZE) && 
			(e100.rfd_to_clean->next->status & RFD_COMPLETE)) {
		e100.rfd_to_clean = e100.rfd_to_clean->next;
		e100.rfds_avail++;
	}
}

int
e100_rx_indicate(char* data)
{
	// 
	// Place the packet into the next available buffer in the ring.
	//

	int r;

	e100.rfd_to_use->prev->command &= ~RFD_S;
	e100.rfd_to_use->command = RFD_S;
	e100.rfd_to_use->status = 0;
	r = e100.rfd_to_use->actual_size & RFD_AC_MASK;
	memmove(data, e100.rfd_to_use->data, r);

	--e100.rfds_avail;
	e100.rfd_to_use = e100.rfd_to_use->next;	

	return r;	
}

// 
// Frames arrive at the device independent of the state of the RU. When a frame 
// is arriving, the E100 is referred to as actively receiving, even when the RU 
// is not in the ready state and the frame is being discarded.
// 
int
e100_rx(char *data)
{
	int r, scb_status;

	// Clean up any RFDs in the ring that are complete.
	e100_rx_clean();

	// If there are no more empty slots in the 
	// receive DMA ring drop the packet.
	if (e100.rfds_avail == 0)
		return -E_RFA_FULL;

	// If there are no packets in the receive DMA ring signal
	// to the asking the user environment to go to sleep and
	// try to re-execute the system call at a later time.
	if (e100.rfds_avail == RFASIZE)
		return -E_RFA_EMPTY;

	// Indicate newly arrived packets.
	r = e100_rx_indicate(data);

	scb_status = inb(e100.io_base + CSR_SCB_STATUS);	
	if ((scb_status & RUS_MASK) == RUS_SUSPENDED) {
		// if the RU is in the suspended state and not actively 
		// discarding a frame the RU goes to the ready state and 
		// configures a new RFD when we issue the resume command.		
		e100_exec_cmd(CSR_SCB_COMMAND, RUC_RESUME);
	}

	return r;	
}

//
// Allocate the receive frame area.
//
// Initialize the RFA setting each rds's link pointer to point to the next RFD 
// in the ring. The pointers need to be physical addresses because a DMA ring 
// is created to be used by the device and a device on the PCI bus does not 
// have access to the CPU's MMU to translate virtual addresses into physical 
// addresses.
//
void
e100_rfa_alloc(void) 
{
	int i, r;
	struct Page *pp;
	struct rfd *rfd = NULL, *tail = NULL;

	for (i = 0; i < RFASIZE; i++) {
		// Allocate a page for each command block.
		// Must zero out the contents of the page and
		// increment the reference count for it.
		if ((r = page_alloc(&pp)) != 0)
			panic("e100_rfa_alloc: %e\n", r);
		memset(page2kva(pp), 0, PGSIZE);
		++pp->pp_ref;

		// Initialize the RFD
		rfd = page2kva(pp);
		rfd->pa = page2pa(pp);
		rfd->size = ETH_FRAME_LEN;

		if (i == 0) {
			e100.rfds = rfd;
		} else {
			// Extend the RFA by inserting the RFD 
			// after the current tail in the list.
			tail->link = rfd->pa;
			tail->next = rfd;
			rfd->prev = tail;
		}
		// Set the new tail.
		tail = rfd;
	}
	// Complete the ring.
	tail->link = e100.rfds->pa;
	tail->next = e100.rfds;
	e100.rfds->prev = tail;

	e100.rfds_avail = RFASIZE;
	e100.rfd_to_clean = tail;
	e100.rfd_to_use = e100.rfds;
}

