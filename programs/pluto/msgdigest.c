/* demultiplex incoming IKE messages
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2002  D. Hugh Redelmeier.
 * Copyright (C) 2019 Andrew Cagney <cagney@gnu.org>
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
 *
 */

#include "defs.h"
#include "log.h"
#include "demux.h"      /* needs packet.h */
#include "iface.h"

struct msg_digest *alloc_md(struct iface_endpoint *ifp,
			    const ip_endpoint *sender,
			    const uint8_t *packet, size_t packet_len,
			    where_t where)
{
	struct logger *logger = &global_logger;
	struct msg_digest *md = refcnt_alloc(struct msg_digest, logger, where);
	md->iface = iface_endpoint_addref_where(ifp, where);
	md->sender = *sender;
	md->logger = alloc_logger(md, &logger_message_vec,
				  /*debugging*/LEMPTY, where);
	md->packet = (packet == NULL ? alloc_chunk(packet_len, "packet") :
		      clone_bytes_as_chunk(packet, packet_len, "packet"));
	return md;
}

struct msg_digest *clone_raw_md(struct msg_digest *md, where_t where)
{
	struct msg_digest *clone = alloc_md(md->iface, &md->sender,
					    md->packet.ptr, md->packet.len,
					    where);
	clone->fake_clone = true;
	clone->md_inception = threadtime_start();
	return clone;
}

struct msg_digest *md_addref_where(struct msg_digest *md, where_t where)
{
	return refcnt_addref(md, NULL, where);
}

void md_delref_where(struct msg_digest **mdp, where_t where)
{
	/* need non-NULL so .logger is available */
	const struct logger *logger = (*mdp != NULL ? (*mdp)->logger : &global_logger);
	struct msg_digest *md = delref_where(mdp, logger, where);
	if (md != NULL) {
		free_chunk_content(&md->raw_packet);
		free_chunk_content(&md->packet);
		free_logger(&md->logger, where);
		iface_endpoint_delref_where(&md->iface, where);
		pfree(md);
	}
}
