#include "ns.h"
#include <inc/lib.h>

extern union Nsipc nsipcbuf;

void
input(envid_t ns_envid)
{
	int len;
	char data[1518];
	binaryname = "ns_input";

	while (1) {
		// Packets received by the network card need to be injected into lwIP.
		// For every packet received by the device driver, this input environment 
		// pulls the packet out of kernel space and sends the packet to the core 
		// server environment using the NSREQ_INPUT IPC message. The core network 
		// server expects all IPC input messages to have a page attached with a 
		// union Nsipc with its struct jif_pkt pkt field filled in.

		// Poll the device driver for recieved packets.
		while ((len = sys_rx(data)) < 0)
			sys_yield();

		// Allocate a new page for every packet we recieve from the
		// device driver before IPCing it to the network server.
		sys_page_alloc(0, &nsipcbuf, PTE_P|PTE_W|PTE_U);
		nsipcbuf.pkt.jp_len = len;
		memmove(nsipcbuf.pkt.jp_data, data, len);

		// Forward each packet to the core network server environment.
		ipc_send(ns_envid, NSREQ_INPUT, &nsipcbuf, PTE_P|PTE_W|PTE_U);
	}
}

