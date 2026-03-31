/* CosmoRT DHCP Client — kernel-level DHCP for boot
 * Extracted from net.c (Phase D1).
 */
#ifndef DHCP_H
#define DHCP_H

/* Run full DHCP discovery (blocking, with timeout).
 * Returns 0 on success, -1 on timeout. */
int net_dhcp(void);

/* Send a single DHCP DISCOVER packet. */
void net_dhcp_send_discover(void);

/* Check for pending DHCP reply. Non-blocking.
 * Returns 1 if IP assigned, 0 if no reply yet. */
int net_dhcp_check(void);

#endif
