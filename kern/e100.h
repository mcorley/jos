#ifndef JOS_KERN_E100_H
#define JOS_KERN_E100_H

#include <inc/ns.h>
#include <kern/pci.h>

// Device manufacturer and id
#define E100_VENDOR_ID 0x8086
#define E100_DEVICE_ID 0x1209

// Entries in reg_base and reg_size valid for the E100
#define E100_MEMORY 0
#define E100_IO 		1
#define E100_FLASH 	2

// E100 error codes
#define E_CBL_FULL	1
#define E_CBL_EMPTY	2
#define E_RFA_FULL	3
#define E_RFA_EMPTY	4

// CSR (Control/Status Registers)
#define CSR_SCB_STATUS 	0x00
#define CSR_SCB_COMMAND 0x02
#define CSR_SCB_GEN_PTR 0x04
#define CSR_PORT 				0x08
#define CSR_EEPROM 			0x0e

// SCB statuses
#define CUS_IDLE		 		0x00
#define CUS_SUSPENDED 	0x40
#define CUS_LPQ_ACTIVE 	0x80
#define CUS_HQP_ACTIVE 	0xc0
#define CUS_MASK 				0xc0

#define RUS_IDLE		 		0x00
#define RUS_SUSPENDED 	0x04
#define RUS_NO_RES		 	0x08
#define RUS_READY			 	0x10
#define RUS_MASK 				0xc0

// SCB commands
#define CUC_NOP 				0x00
#define CUC_START 			0x10 
#define CUC_RESUME 			0x20 
#define CUC_DUMP_ADDR		0x40
#define CUC_DUMP_STATS	0x50
#define CUC_LOAD_BASE		0x60
#define CUC_DUMP_RESET	0x70

#define RUC_NOP 				0x0
#define RUC_START 			0x1
#define RUC_RESUME 			0x2 
#define RUC_REDIRECT		0x3 
#define RUC_ABORT 			0x4
#define RUC_LOADHDS 		0x5
#define RUC_LOAD_BASE 	0x6

// Port selection function opcodes.
#define PORT_SOFTWARE_RESET 	0x0000
#define PORT_SELFTEST 				0x0001
#define PORT_SELECTIVE_RESET 	0x0002

// CB status
#define CB_COMPLETE	0x8000
#define CB_OK 			0x2000

// CB command
#define CB_NOP 		0x0000
#define CB_IAADDR 0x0001
#define CB_CONFIG 0x0002
#define CB_MULTI 	0x0003
#define CB_TX 		0x0004
#define CB_UCODE 	0x0005
#define CB_DUMP 	0x0006
#define CB_TX_SF 	0x0008
#define CB_I 			0x2000
#define CB_S 			0x4000
#define CB_EL 		0x8000

// RFD command
#define RFD_EL 	0x8000
#define RFD_S 	0x4000
#define RFD_H 	0x0010
#define RFD_SF 	0x0008

// RFD status
#define RFD_COMPLETE 	0x8000
#define RFD_OK 				0x2000
#define RFD_MASK 			0x1fff

// RFD data
#define RFD_SIZE_MASK 0x3fff
#define RFD_AC_MASK 	0x3fff
#define RFD_EOF			 	0x8000
#define RFD_F 				0x4000

// 
// DMA rings.
//
// A DMA ring is a set of buffers allocated in main memory and chained together 
// by pointers. This ring is usually a circular singly-linked list where the 
// pointers are physical addresses of the next buffer in the ring. The pointers 
// need to be physical addresses because a DMA ring is created to be used by 
// the device and a device on the PCI bus does not have access to the CPU's MMU 
// to translate virtual addresses into physical addresses.
//
// The CU and RU use DMA processors built into the 82559ER to read and write 
// packets in the main memory DMA rings instead of using inb/outb operations to
// move blocks of data manually to and from the device and wasting CPU cycles.
//

#define CBLSIZE 10
#define RFASIZE CBLSIZE
#define ETH_FRAME_LEN 1518

// A control DMA ring is composed of buffers called Command Blocks (CB).
// The DMA ring of CBs is called a Command Block List (CBL).
struct cb {
	volatile uint16_t status; // indicates status after a command
	uint16_t command; // type of command the driver is sending the CU
	physaddr_t link; // the 32-bit address of the next CB in the ring
	union {
		struct {
			uint32_t tbd_array; // In flexible mode, points to TBD array
			uint16_t tcb_byte_count; // the number of bytes that will be transmitted	
			uint8_t threshold; // the number of bytes needed before transmitting the frame
			uint8_t tbd_count; // number of transmit buffers in the contiguous TBD array
			char data[ETH_FRAME_LEN]; // the bytes that constitute the packet
		} tcb;
	} u;
	struct cb *next, *prev; // pointers to next and prev in cbl
	physaddr_t pa; // physical address of the cb
};

// Buffers in the receive DMA ring are called Receive Frame Descriptors (RFD).
// The entire receive DMA ring is called a Receive Frame Area (RFA). 
struct rfd {
	volatile uint16_t status; // indicates status after a command
	uint16_t command; // type of command the driver is sending the RU
	physaddr_t link; // the 32-bit address of the next RFD in the ring
	uint32_t rbd; // reserved
	uint16_t actual_size; // the number of bytes written into the data area.	
	uint16_t size; // represents the data buffer size	
	char data[ETH_FRAME_LEN]; // the bytes that constitute the packet

	struct rfd *next, *prev; // pointers to next and prev in cbl
	physaddr_t pa; // physical address of the cb
};

// 
// Network Interface Card
//
struct nic {
	uint32_t io_base; // base I/O port assigned to the device
	uint8_t irq_line; // line to listen to receive interrupts from the device

	// CBL
	int cbs_avail; // keeps track of number of free CB resources available
	struct cb *cbs; // the first cb in the ring
	struct cb *cb_to_clean; // the next CB to check for completion
	struct cb *cb_to_use; // the next CB to use for queuing a command

	// RFA
	int rfds_avail; // keeps track of number of free RFD resources available
	struct rfd *rfds; // the first RFD in the ring
	struct rfd *rfd_to_clean; // the next RFD to check for completion
	struct rfd *rfd_to_use; // the next RFD to use for queuing a command
};

int e100_pci_attach(struct pci_func *pcif);
void e100_init(void);
void e100_software_reset(void);
void udelay(int loops);
void e100_exec_cmd(uint8_t csr, uint8_t cmd);

void e100_cbl_alloc(void);
int e100_xmit_frame(const char *data, uint16_t len);
int e100_xmit_prepare(const char *data, uint16_t len, uint16_t flag);
void e100_tx_clean(void);

void e100_rfa_alloc(void);
int e100_rx(char *data);
int e100_rx_indicate(char* data);
void e100_rx_clean(void);

#endif	// JOS_KERN_E100_H

