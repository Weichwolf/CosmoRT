/* CosmoRT IPv6 — well-known address constants.
 *
 * Held as r/o globals so address-of comparisons work and so they have a
 * symbol address other code can refer to (procfs sysctls etc.). */

#include "net/in6.h"

const struct in6_addr in6addr_any      = IN6ADDR_ANY_INIT;
const struct in6_addr in6addr_loopback = IN6ADDR_LOOPBACK_INIT;
