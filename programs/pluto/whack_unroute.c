/* route connections, for libreswan
 *
 * Copyright (C) 2023  Andrew Cagney
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

#include "whack_unroute.h"
#include "show.h"
#include "log.h"
#include "terminate.h"
#include "visit_connection.h"
#include "whack.h"
#include "connections.h"

static unsigned whack_unroute_connections(const struct whack_message *m UNUSED,
					  struct show *s, struct connection *c)
{
	struct logger *logger = show_logger(s);
	connection_addref(c, logger);
	{
		connection_attach(c, logger);
		terminate_and_down_and_unroute_connections(c, HERE);
		connection_detach(c, logger);
	}
	connection_delref(&c, logger);
	return 1;
}

void whack_unroute(const struct whack_message *m, struct show *s)
{
	if (m->name == NULL) {
		/* leave bread crumb */
		whack_log(RC_FATAL, s,
			  "received command to unroute connection, but did not receive the connection name - ignored");
		return;
	}

	whack_connection(m, s, whack_unroute_connections,
			 /*alias_order*/OLD2NEW,
			 (struct each) {
				 .log_unknown_name = true,
			 });
}
