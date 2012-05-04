#include "ns.h"

extern union Nsipc nsipcbuf;

//
// Accept NSREQ_OUTPUT IPC messages from the core network server and send the 
// packets accompanying these IPC message to the network device driver
//
void
output(envid_t ns_envid)
{
	binaryname = "ns_output";

	while (1) {
		// When servicing user environment socket calls, lwIP will generate packets 
		// for the network card to transmit. LwIP will send each packet to be 
		// transmitted to this output helper environment using the NSREQ_OUTPUT IPC 
		// message with the packet attached in the page argument of the IPC message.
		if (ipc_recv(NULL, &nsipcbuf, NULL) != NSREQ_OUTPUT)
			continue;

		// Forward the packet to the E100 device driver to transmit.
		sys_xmit_frame(nsipcbuf.pkt.jp_data, nsipcbuf.pkt.jp_len);
	}	
}
