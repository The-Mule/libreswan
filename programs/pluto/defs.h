/* misc. universal things, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2001  D. Hugh Redelmeier.
 * Copyright (C) 2018-2019 Andrew Cagney <cagney@gnu.org>
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

#ifndef _DEFS_H
#define _DEFS_H

#include <limits.h>		/* for UINT_MAX */

#include "lswcdefs.h"
#include "lswalloc.h"
#include "realtime.h"

#include "ipsec_spi.h"

/*
 * Per-connection unique ID.
 *
 * Define co_serial_t as an enum so that GCC 10 will detect and
 * complain when code attempts to assign the wrong type.
 *
 * - an enum is always the size of <<signed int> or <<unsigned int>>
 *   (presumably so that the size of <<enum foo f>> is always known)
 *
 * - an enum's sign can be signed or unsigned; specifying a UINT_MAX
 *   value forces it to unsigned
 */

typedef enum { COS_NOBODY = 0, COS_MAX = UINT_MAX, } co_serial_t;

#define PRI_CO "$%u"
#define pri_co(CO) (CO)
size_t jam_co(struct jambuf *buf, co_serial_t co);
#define pri_connection_co(C) ((C) == NULL ? COS_NOBODY : (C)->serialno)

/*
 * Type of serial number of a state object.
 *
 * Used everywhere as a safe proxy for a state object.  Needed in
 * connections.h and state.h; here to simplify dependencies.
 *
 * XXX: like co_connection_t, this should be changed to an enum.
 * Doing this will require updating all the print statements using
 * "#%lu".  Sigh.
 */

typedef enum {
	SOS_NOBODY = 0,		/* null serial number */
	SOS_FIRST = 1,		/* first normal serial number */
	SOS_MAX = UINT_MAX,
} so_serial_t;

#define PRI_SO "#%u"
#define pri_so(SO) (SO)
size_t jam_so(struct jambuf *buf, so_serial_t so);

typedef uint32_t msgid_t;      /* Host byte ordered */
#define PRI_MSGID "%"PRIu32
#define v1_MAINMODE_MSGID  ((msgid_t) 0)		/* network and host order */
#define v2_FIRST_MSGID  ((msgid_t) 0)			/* network and host order */
#define v2_INVALID_MSGID  ((msgid_t) 0xffffffff)	/* network and host order */


/* are all bytes 0? */
extern bool all_zero(const unsigned char *m, size_t len);

/* pad_up(n, m) is the amount to add to n to make it a multiple of m */
#define pad_up(n, m) (((m) - 1) - (((n) + (m) - 1) % (m)))

extern bool in_main_thread(void);	/* in plutomain.c */

void free_pluto_main(void);	/* XXX: better home? */

void check_deltatime(deltatime_t timeout, int lower, int upper, const char *conf, struct logger *logger);

#endif /* _DEFS_H */
