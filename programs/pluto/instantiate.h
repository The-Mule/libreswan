/* information about connections between hosts and clients
 *
 * Copyright (C) 2003 Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef INSTANTIATE_H
#define INSTANTIATE_H

#include "ip_port.h"
#include "ip_subnet.h"
#include "ip_selector.h"
#include "ip_packet.h"
#include "where.h"
#include "shunk.h"

struct ip_protocol;
struct ike_sa;
struct connection;
struct id;
struct kernel_acquire;
struct verbose;

/*
 * Given some sort of connection template or group, instantiate it.
 */

struct connection *group_instantiate(struct connection *group,
				     const ip_subnet remote_subnet,
				     const struct ip_protocol *protocol,
				     ip_port local_port,
				     ip_port remote_port,
				     where_t where);

/*
 * A packet arrived, no clue other than the initiator's address.
 *
 * - peer's identity is unknown
 * - peer's selectors are unknown
 */

struct connection *rw_responder_instantiate(struct connection *t,
					    const ip_address peer_addr,
					    where_t where);

/*
 * Now, in addition to the initiator's address, the initiator's ID is
 * known (and proven).
 *
 * - peer's selectors are still unknown.
 */

struct connection *rw_responder_id_instantiate(struct connection *t,
					       const ip_address peer_addr,
					       const struct id *peer_id,
					       where_t where);

/*
 * IKEv1, where the initiator's address, ID and selectors are known.
 *
 * However:
 * - the peer may not have taken a lease (skipped MODECFG)
 * - tunnel VS transport is resolved later; big oops
 *
 * IKEv2 uses spd_instantiate() below.
 */

struct connection *rw_responder_v1_quick_n_dirty_instantiate(struct connection *t,
							     const ip_address peer_addr,
							     const ip_selector peer_subnet,
							     const struct id *peer_id,
							     struct verbose verbose,
							     where_t where);

struct connection *oppo_initiator_instantiate(struct connection *t,
					      ip_packet packet,
					      where_t where);
struct connection *oppo_responder_instantiate(struct connection *t,
					      const ip_address remote_address,
					      where_t where);

struct connection *spd_instantiate(struct connection *t,
				   const ip_address peer_addr,
				   where_t where);

struct connection *labeled_parent_instantiate(struct ike_sa *ike,
					      shunk_t sec_label,
					      where_t where);
struct connection *labeled_template_instantiate(struct connection *t,
						const ip_address remote_address,
						where_t where);

#endif
