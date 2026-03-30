/* CosmoRT DHCP Client — kernel-level DHCP for boot */
#ifndef DHCP_H
#define DHCP_H

int net_dhcp(void);

void net_dhcp_send_discover(void);

int net_dhcp_check(void);

#endif
