/*
 * mechanisms for preshared keys (public, private, and preshared secrets)
 *
 * this is the library for reading (and later, writing!) the ipsec.secrets
 * files.
 *
 * Copyright (C) 1998-2004  D. Hugh Redelmeier.
 * Copyright (C) 2005 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2015 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2016-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2018 Sahana Prasad <sahana.prasad07@gmail.com>
 * Copyright (C) 2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2019 D. Hugh Redelmeier <hugh@mimosa.com>
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

#include <pthread.h>	/* pthread.h must be first include file */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>	/* missing from <resolv.h> on old systems */

#include <pk11pub.h>
#include <prerror.h>
#include <cert.h>
#include <cryptohi.h>
#include <keyhi.h>

#include "ttodata.h"
#include "lswglob.h"
#include "sysdep.h"
#include "lswlog.h"
#include "constants.h"
#include "lswalloc.h"
#include "id.h"
#include "x509.h"
#include "secrets.h"
#include "certs.h"
#include "lex.h"

#include "lswnss.h"
#include "ip_info.h"
#include "nss_cert_load.h"
#include "ike_alg.h"
#include "ike_alg_hash.h"
#include "certs.h"
#include "crypt_hash.h"

struct fld {
	const char *name;
	ssize_t offset;
};

static void process_secrets_file(struct file_lex_position *flp,
				 struct secret **psecrets, const char *file_pat);

struct secret {
	enum secret_kind kind;
	/*
	 * The ipsec.secrets line number, but which ipsec.secrets
	 * (include means more than one).
	 *
	 * For NSS, this is the entry number.
	 */
	int line;
	/*
	 * List of IDs, is this part of pubkey stuff?
	 */
	struct id_list *ids;
	union /*kind*/ {
		struct secret_preshared_stuff *preshared;
		struct secret_ppk_stuff *ppk;
		struct secret_pubkey_stuff *pubkey;
	} u;
	/* hope the list doesn't get too long */
	struct secret *next;
};

const struct secret_preshared_stuff *secret_preshared_stuff(const struct secret *secret)
{
	if (secret == NULL) {
		return NULL;
	}

	switch (secret->kind) {
	case SECRET_PSK:
	case SECRET_XAUTH:
		/* some sort of PSK */
		return secret->u.preshared;
	default:
		return NULL;
	}
}

struct secret_pubkey_stuff *secret_pubkey_stuff(const struct secret *secret)
{
	if (secret == NULL) {
		return NULL;
	}

	switch (secret->kind) {
	case SECRET_RSA:
	case SECRET_ECDSA:
	case SECRET_EDDSA:
		/* some sort of PKI */
		return secret->u.pubkey;
	default:
		return NULL;
	}
}

const struct secret_ppk_stuff *secret_ppk_stuff(const struct secret *secret)
{
	if (secret == NULL) {
		return NULL;
	}

	switch (secret->kind) {
	case SECRET_PPK:
		return secret->u.ppk;
	default:
		return NULL;
	}
}

struct id_list *lsw_get_idlist(const struct secret *s)
{
	return s->ids;
}

/*
 * forms the keyid from the public exponent e and modulus n
 */
void form_keyid(chunk_t e, chunk_t n, keyid_t *keyid, size_t *keysize)
{
	struct logger *logger = &global_logger;

	/* eliminate leading zero byte in modulus from ASN.1 coding */
	if (*n.ptr == 0x00) {
		/*
		 * The "adjusted" length of modulus n in octets:
		 * [RSA_MIN_OCTETS, RSA_MAX_OCTETS].
		 *
		 * According to form_keyid() this is the modulus length
		 * less any leading byte added by DER encoding.
		 *
		 * The adjusted length is used in sign_hash() as the
		 * signature length - wouldn't PK11_SignatureLen be
		 * better?
		 *
		 * The adjusted length is used in
		 * same_RSA_public_key() as part of comparing two keys
		 * - but wouldn't that be redundant?  The direct n==n
		 * test would pick up the difference.
		 */
		ldbgf(DBG_CRYPT, logger, "XXX: adjusted modulus length %zu->%zu",
		      n.len, n.len - 1);
		n.ptr++;
		n.len--;
	}

	/* form the Libreswan keyid */
	err_t err = splitkey_to_keyid(e.ptr, e.len, n.ptr, n.len, keyid);
	PASSERT(logger, err == NULL);

	/* return the RSA modulus size in octets */
	*keysize = n.len;
}

const keyid_t *pubkey_keyid(const struct pubkey *pk)
{
	return &pk->content.keyid;
}

const ckaid_t *pubkey_ckaid(const struct pubkey *pk)
{
	return &pk->content.ckaid;
}

const ckaid_t *secret_ckaid(const struct secret *secret)
{
	const struct secret_pubkey_stuff *pki =
		secret_pubkey_stuff(secret);
	if (pki == NULL) {
		return NULL;
	}

	/* some sort of PKI */
	return &pki->content.ckaid;
}

const keyid_t *secret_keyid(const struct secret *secret)
{
	const struct secret_pubkey_stuff *pki =
		secret_pubkey_stuff(secret);
	if (pki == NULL) {
		return NULL;
	}
	/* some sort of PKI */
	return &pki->content.keyid;
}

struct secret *foreach_secret(struct secret *secrets,
			      secret_eval func,
			      struct secret_context *context)
{
	for (struct secret *s = secrets; s != NULL; s = s->next) {
		int result = (*func)(s, s->kind,
				     s->line, context);

		if (result == 0)
			return s;

		if (result == -1)
			break;
	}
	return NULL;
}

static struct secret *find_secret_by_pubkey_ckaid_1(struct secret *secrets,
						    const ckaid_t *ckaid,/*truncated?*/
						    const struct logger *logger)
{
	struct verbose verbose = VERBOSE(DEBUG_STREAM, logger, NULL);

	ckaid_buf cb;
	vdbg("looking for pubkey secret with ckaid %s", str_ckaid(ckaid, &cb));
	unsigned base_level = verbose.level;

	for (struct secret *secret = secrets; secret != NULL; secret = secret->next) {
		id_buf idb;
		name_buf kb;
		verbose.level = base_level + 1;
		vdbg("trying secret %s:%s",
		     str_enum_long(&secret_kind_names, secret->kind, &kb),
		     (vbad(secret->ids == NULL) ? "<NULL-ID-LIST>" : str_id(&secret->ids->id, &idb)));
		verbose.level++;

		const struct secret_pubkey_stuff *pki = secret_pubkey_stuff(secret);
		if (pki == NULL) {
			/* not a pubkey */
			vdbg("secret has no pubkey stuff");
			continue;
		}

		if (!hunk_eq(pki->content.ckaid, *ckaid)) {
			vdbg("secret has wrong ckaid");
			continue;
		}

		vdbg("secret matched");
		return secret;
	}
	return NULL;
}

bool secret_pubkey_same(const struct secret *lhs,
			const struct secret *rhs,
			const struct logger *logger)
{
	/* should be == SECRET_PKI */
	const struct secret_pubkey_stuff *lpk = secret_pubkey_stuff(lhs);
	if (lpk == NULL) {
		return false;
	}

	const struct secret_pubkey_stuff *rpk = secret_pubkey_stuff(rhs);
	if (rpk == NULL) {
		return false;
	}

	if (lpk->content.type != rpk->content.type) {
		return false;
	}

	return lpk->content.type->pubkey_same(&lpk->content, &rpk->content, logger);
}

struct secret *lsw_find_secret_by_id(struct secret *secrets,
				     enum secret_kind kind,
				     const struct id *local_id,
				     const struct id *remote_id,
				     bool asym)
{
	const struct logger *logger = &global_logger;
	enum {
		match_none = 0,

		/* bits */
		match_default = 1,
		match_any = 2,
		match_remote = 4,
		match_local = 8
	};
	lset_t best_match = match_none;
	struct secret *best = NULL;

	for (struct secret *s = secrets; s != NULL; s = s->next) {
		id_buf idl;
		name_buf kb, skb;
		ldbg(logger, "line %d: key type %s(%s) to type %s",
		     s->line,
		     str_enum_long(&secret_kind_names, kind, &kb),
		     str_id(local_id, &idl),
		     str_enum_long(&secret_kind_names, s->kind, &skb));

		if (s->kind != kind) {
			ldbg(logger, "  wrong kind");
			continue;
		}

		lset_t match = match_none;

		if (s->ids == NULL) {
			/*
			 * a default (signified by lack of ids):
			 * accept if no more specific match found
			 */
			match = match_default;
		} else {
			/* check if both ends match ids */
			struct id_list *i;
			int idnum = 0;

			for (i = s->ids; i != NULL; i = i->next) {
				idnum++;
				if (id_is_any(&i->id)) {
					/*
					 * match any will
					 * automatically match
					 * local and remote so
					 * treat it as its own
					 * match type so that
					 * specific matches
					 * get a higher
					 * "match" value and
					 * are used in
					 * preference to "any"
					 * matches.
					 */
					match |= match_any;
				} else {
					if (same_id(&i->id, local_id)) {
						match |= match_local;
					}

					if (remote_id != NULL &&
					    same_id(&i->id, remote_id)) {
						match |= match_remote;
					}
				}

				id_buf idi;
				id_buf idl;
				id_buf idr;
				ldbg(logger, "%d: compared key %s to %s / %s -> "PRI_LSET,
				     idnum,
				     str_id(&i->id, &idi),
				     str_id(local_id, &idl),
				     (remote_id == NULL ? "" : str_id(remote_id, &idr)),
				     match);
			}

			/*
			 * If our end matched the only id in the list,
			 * default to matching any peer.
			 * A more specific match will trump this.
			 */
			if (match == match_local &&
			    s->ids->next == NULL)
				match |= match_default;
		}

		if (match == match_none) {
			ldbg(logger, "  id didn't match");
			continue;
		}

		ldbg(logger, "  match="PRI_LSET, match);
		if (match == best_match) {
			/*
			 * Two good matches are equally good: do they
			 * agree?
			 */
			bool same = false;

			switch (kind) {
			case SECRET_NULL:
				same = true;
				break;
			case SECRET_PSK:
				same = hunk_eq(s->u.preshared[0],
					       best->u.preshared[0]);
				break;
			case SECRET_RSA:
			case SECRET_ECDSA:
			case SECRET_EDDSA:
				same = secret_pubkey_same(s, best, logger);
				break;
			case SECRET_XAUTH:
				/*
				 * We don't support this yet,
				 * but no need to die.
				 */
				break;
			case SECRET_PPK:
				same = hunk_eq(s->u.ppk->key,
					       best->u.ppk->key);
				break;
			default:
				bad_case(kind);
			}
			if (!same) {
				ldbg(logger, "  multiple ipsec.secrets entries with distinct secrets match endpoints: first secret used");
				/*
				 * list is backwards: take latest in
				 * list
				 */
				best = s;
			}
			continue;
		}

		if (match == match_local && !asym) {
			/*
			 * Only when this is an asymmetric (eg. public
			 * key) system, allow this-side-only match to
			 * count, even when there are other ids in the
			 * list.
			 */
			ldbg(logger, "  local match not asymmetric");
			continue;
		}

		switch (match) {
		case match_local:
		case match_default:	/* default all */
		case match_any:	/* a wildcard */
		case match_local | match_default:	/* default peer */
		case match_local | match_any: /* %any/0.0.0.0 and local */
		case match_remote | match_any: /* %any/0.0.0.0 and remote */
		case match_local | match_remote:	/* explicit */
			/*
			 * XXX: what combinations are missing?
			 */
			if (match > best_match) {
				ldbg(logger, "  match "PRI_LSET" beats previous best_match "PRI_LSET" match=%p (line=%d)",
				    match, best_match, s, s->line);
				/* this is the best match so far */
				best_match = match;
				best = s;
			} else {
				ldbg(logger, "  match "PRI_LSET" loses to best_match "PRI_LSET,
				    match, best_match);
			}
		}
	}

	ldbg(logger, "concluding with best_match="PRI_LSET" best=%p (lineno=%d)",
	    best_match, best,
	    best == NULL ? -1 : best->line);

	return best;
}

/*
 * digest a secrets file
 *
 * The file is a sequence of records.  A record is a maximal sequence of
 * tokens such that the first, and only the first, is in the first column
 * of a line.
 *
 * Tokens are generally separated by whitespace and are key words, ids,
 * strings, or data suitable for ttodata(3).  As a nod to convention,
 * a trailing ":" on what would otherwise be a token is taken as a
 * separate token.  If preceded by whitespace, a "#" is taken as starting
 * a comment: it and the rest of the line are ignored.
 *
 * One kind of record is an include directive.  It starts with "include".
 * The filename is the only other token in the record.
 * If the filename does not start with /, it is taken to
 * be relative to the directory containing the current file.
 *
 * The other kind of record describes a key.  It starts with a
 * sequence of ids and ends with key information.  Each id
 * is an IP address, a Fully Qualified Domain Name (which will immediately
 * be resolved), or @FQDN which will be left as a name.
 *
 * The form starts the key part with a ":".
 *
 * For Preshared Key, use the "PSK" keyword, and follow it by a string
 * or a data token suitable for ttodata(3).
 *
 * For raw RSA Keys in NSS, use the "RSA" keyword, followed by a
 * brace-enclosed list of key field keywords and data values.
 * The data values are large integers to be decoded by ttodata(3).
 * The fields are a subset of those used by BIND 8.2 and have the
 * same names.
 *
 * For XAUTH passwords, use @username followed by ":XAUTH" followed by the password
 *
 * For Post-Quantum Preshared Keys, use the "PPKS" keyword if the PPK is static.
 *
 * PIN for smartcard is no longer supported - use NSS with smartcards
 */

/* parse PSK from file */
static diag_t process_preshared_secret(const char *what, size_t minlen,
				       struct file_lex_position *flp,
				       struct secret_preshared_stuff **psk)
{
	if (flp->quote == '"' || flp->quote == '\'') {
		size_t len = strlen(flp->tok);
		if (minlen > 0 && len < minlen) {
			llog(RC_LOG, flp->logger,
			     "WARNING: using a weak secret (%s)", what);
		}
		*psk = clone_bytes_as_hunk(struct secret_preshared_stuff,
					   flp->tok, len);
		shift(flp);
		return NULL;
	}

	chunk_t secret;
	err_t ugh = ttochunk(shunk2(flp->tok, flp->cur - flp->tok), 0, &secret);
	if (ugh != NULL) {
		/* ttodata didn't like PSK data */
		return diag("%s data malformed (%s): %s",
			    what, ugh, flp->tok);
	}

	*psk = clone_bytes_as_hunk(struct secret_preshared_stuff,
				   secret.ptr, secret.len);
	free_chunk_content(&secret);

	shift(flp);
	return NULL;
}

/* parse static PPK */
static diag_t process_ppk_static_secret(struct file_lex_position *flp,
					struct secret_ppk_stuff **ppk)
{
	if (flp->quote != '"' && flp->quote != '\'') {
		return diag("no quotation marks found, PPK ID should be in quotation marks");
	}

	chunk_t id = clone_bytes_as_chunk(flp->tok, strlen(flp->tok), "PPK ID");
	if (!shift(flp)) {
		free_chunk_content(&id);
		return diag("No PPK found. PPK should be specified after PPK ID");
	}

	chunk_t key;
	if (flp->quote == '"' || flp->quote == '\'') {
		key = clone_bytes_as_chunk(flp->tok, strlen(flp->tok), "PPK");
		shift(flp);
	} else {
		err_t ugh = ttochunk(shunk2(flp->tok, flp->cur - flp->tok), 0, &key);
		if (ugh != NULL) {
			/* ttodata didn't like PPK data */
			free_chunk_content(&id);
			return diag("PPK data malformed (%s): %s", ugh, flp->tok);
		}

		shift(flp);
	}

	/* merge the fields */
	(*ppk) = overalloc_thing(struct secret_ppk_stuff, id.len + key.len);

	uint8_t *dst = (*ppk)->data;
	(*ppk)->id.ptr = dst;
	(*ppk)->id.len = id.len;
	memcpy(dst, id.ptr, id.len);

	dst += id.len;
	(*ppk)->key.ptr = dst;
	(*ppk)->key.len = key.len;
	memcpy(dst, key.ptr, key.len);

	free_chunk_content(&key);
	free_chunk_content(&id);

	return NULL;
}

const struct secret_ppk_stuff *secret_ppk_stuff_by_id(const struct secret *s, shunk_t ppk_id)
{
	while (s != NULL) {
		if (s->kind == SECRET_PPK &&
		    hunk_eq(s->u.ppk->id, ppk_id))
			return s->u.ppk;
		s = s->next;
	}
	return NULL;
}

static SECKEYPrivateKey *copy_private_key(SECKEYPrivateKey *private_key,
					  const struct logger *logger)
{
	SECKEYPrivateKey *unpacked_key = NULL;
	if (private_key->pkcs11Slot != NULL) {
		PK11SlotInfo *slot = PK11_ReferenceSlot(private_key->pkcs11Slot);
		if (slot != NULL) {
			ldbg(logger, "copying key using reference slot");
			unpacked_key = PK11_CopyTokenPrivKeyToSessionPrivKey(slot, private_key);
			PK11_FreeSlot(slot);
		}
	}
	if (unpacked_key == NULL) {
		CK_MECHANISM_TYPE mech = PK11_MapSignKeyType(private_key->keyType);
		PK11SlotInfo *slot = PK11_GetBestSlot(mech, NULL);
		if (slot != NULL) {
			ldbg(logger, "copying key using mech/slot");
			unpacked_key = PK11_CopyTokenPrivKeyToSessionPrivKey(slot, private_key);
			PK11_FreeSlot(slot);
		}
	}
	if (unpacked_key == NULL) {
		ldbg(logger, "copying key using SECKEY_CopyPrivateKey()");
		unpacked_key = SECKEY_CopyPrivateKey(private_key);
	}
	return unpacked_key;
}

static pthread_mutex_t certs_and_keys_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * lock access to my certs and keys
 */
static void lock_certs_and_keys(const char *who, const struct logger *logger)
{
	pthread_mutex_lock(&certs_and_keys_mutex);
	ldbg(logger, "certs and keys locked by '%s'", who);
}

/*
 * unlock access to my certs and keys
 */
static void unlock_certs_and_keys(const char *who, const struct logger *logger)
{
	ldbg(logger, "certs and keys unlocked by '%s'", who);
	pthread_mutex_unlock(&certs_and_keys_mutex);
}

static void add_secret(struct secret **slist,
		       struct secret *s,
		       const char *story,
		       const struct logger *logger)
{
	/*
	 * If the ID list is empty, add two empty IDs.
	 *
	 * XXX: The below seem to be acting as a sentinel so that
	 * lsw_find_secret_by_id() always finds something (they act as
	 * wildcards) in the ID list.
	 *
	 * But why are two needed?!?.
	 */
	if (s->ids == NULL) {
		struct id_list *idl = alloc_bytes(sizeof(struct id_list), "id list");
		idl->next = NULL;
		idl->id = empty_id;
		idl->id.kind = ID_NONE;
		idl->id.ip_addr = unset_address;

		struct id_list *idl2 = alloc_bytes(sizeof(struct id_list), "id list");
		idl2->next = idl;
		idl2->id = empty_id;
		idl2->id.kind = ID_NONE;
		idl2->id.ip_addr = unset_address;

		s->ids = idl2;
	}

	lock_certs_and_keys(story, logger);
	s->next = *slist;
	*slist = s;
	unlock_certs_and_keys(story, logger);
}

static void process_secret(struct file_lex_position *flp,
			   struct secret **psecrets, struct secret *s)
{
	diag_t ugh = NULL;

	if (tokeqword(flp, "psk")) {
		s->kind = SECRET_PSK;
		/* preshared key: quoted string or ttodata format */
		ugh = (!shift(flp) ? diag("ERROR: unexpected end of record in PSK") :
		       process_preshared_secret("PSK", 8, flp,
						&s->u.preshared));
		ldbg(flp->logger, "processing PSK at line %d: %s",
		     s->line,
		     (ugh == NULL ? "passed" : str_diag(ugh)));
	} else if (tokeqword(flp, "xauth")) {
		/* xauth key: quoted string or ttodata format */
		s->kind = SECRET_XAUTH;
		ugh = (!shift(flp) ? diag("ERROR: unexpected end of record in PSK") :
		       process_preshared_secret("XAUTH", 0, flp,
						&s->u.preshared));
		ldbg(flp->logger, "processing XAUTH at line %d: %s",
		     s->line,
		     (ugh == NULL ? "passed" : str_diag(ugh)));
	} else if (tokeqword(flp, "ppks")) {
		s->kind = SECRET_PPK;
		ugh = (!shift(flp) ? diag("ERROR: unexpected end of record in static PPK") :
		       process_ppk_static_secret(flp, &s->u.ppk));
		ldbg(flp->logger, "processing PPK at line %d: %s",
		     s->line,
		     (ugh == NULL ? "passed" : str_diag(ugh)));
	} else {
		ugh = diag("WARNING: ignored unrecognized keyword: %s", flp->tok);
	}

	if (ugh != NULL) {
		llog(RC_LOG, flp->logger, "\"%s\" line %d: %s",
		     flp->filename, s->line, str_diag(ugh));
		pfree_diag(&ugh);
		/* free id's that should have been allocated */
		if (s->ids != NULL) {
			struct id_list *i, *ni;
			for (i = s->ids; i != NULL; i = ni) {
				ni = i->next;	/* grab before freeing i */
				free_id_content(&i->id);
				pfree(i);
			}
		}
		/* finally free s */
		pfree(s);
		return;
	}

	if (flushline(flp, "expected record boundary in key")) {
		/* gauntlet has been run: install new secret */
		add_secret(psecrets, s, "process_secret", flp->logger);
	}
}

static void process_secret_records(struct file_lex_position *flp,
				   struct secret **psecrets)
{
	/* const struct secret *secret = *psecrets; */

	/* read records from ipsec.secrets and load them into our table */
	for (;; ) {
		flushline(flp, NULL);	/* silently ditch leftovers, if any */
		if (flp->bdry == B_file)
			break;

		flp->bdry = B_none;	/* eat the Record Boundary */
		shift(flp);	/* get real first token */

		if (tokeqword(flp, "include")) {
			/* an include directive */
			char fn[MAX_TOK_LEN];	/*
						 * space for filename
						 * (I hope)
						 */
			char *p = fn;
			char *end_prefix = strrchr(flp->filename, '/');

			if (!shift(flp)) {
				llog(RC_LOG, flp->logger,
					    "\"%s\" line %d: unexpected end of include directive",
					    flp->filename, flp->lino);
				continue;	/* abandon this record */
			}

			/*
			 * if path is relative and including file's pathname has
			 * a non-empty dirname, prefix this path with that
			 * dirname.
			 */
			if (flp->tok[0] != '/' && end_prefix != NULL) {
				size_t pl = end_prefix - flp->filename + 1;

				/*
				 * "clamp" length to prevent problems now;
				 * will be rediscovered and reported later.
				 */
				if (pl > sizeof(fn))
					pl = sizeof(fn);
				memcpy(fn, flp->filename, pl);
				p += pl;
			}
			if (flp->cur - flp->tok >= &fn[sizeof(fn)] - p) {
				llog(RC_LOG, flp->logger,
					    "\"%s\" line %d: include pathname too long",
					    flp->filename, flp->lino);
				continue;	/* abandon this record */
			}
			/*
			 * The above test checks that there is enough space for strcpy
			 * but clang 3.4 thinks the destination will overflow.
			 *	strcpy(p, flp->tok);
			 * Rewrite as a memcpy in the hope of calming it.
			 */
			memcpy(p, flp->tok, flp->cur - flp->tok + 1);
			shift(flp);	/* move to Record Boundary, we hope */
			if (flushline(flp, "ignoring malformed INCLUDE -- expected Record Boundary after filename")) {
				process_secrets_file(flp, psecrets, fn);
				flp->tok = NULL;	/* redundant? */
			}
		} else {
			/*
			 * Expecting a list of indices and then the
			 * key info.
			 */
			struct secret *s = alloc_thing(struct secret, "secret");
			s->kind = 0;	/* invalid */
			s->line = flp->lino;

			for (;;) {
				struct id id;
				err_t ugh;

				if (tokeq(flp, ":")) {
					/* found key part */
					shift(flp);	/* eat ":" */
					process_secret(flp, psecrets, s);
					break;
				}

				/*
				 * an id
				 * See RFC2407 IPsec Domain of
				 * Interpretation 4.6.2
				 */
				if (tokeq(flp, "%any")) {
					id = empty_id;
					id.kind = ID_IPV4_ADDR;
					id.ip_addr = ipv4_info.address.zero;
					ugh = NULL;
				} else if (tokeq(flp, "%any6")) {
					id = empty_id;
					id.kind = ID_IPV6_ADDR;
					id.ip_addr = ipv6_info.address.zero;
					ugh = NULL;
				} else {
					ugh = atoid(flp->tok, &id);
				}

				if (ugh != NULL) {
					llog(RC_LOG, flp->logger,
						    "ERROR \"%s\" line %d: index \"%s\" %s",
						    flp->filename,
						    flp->lino, flp->tok,
						    ugh);
				} else {
					struct id_list *i = alloc_thing(
						struct id_list,
						"id_list");

					i->id = id;
					i->next = s->ids;
					s->ids = i;
					id_buf b;
					name_buf skb;
					ldbg(flp->logger, "id type added to secret(%p) %s: %s",
					     s, str_enum_long(&secret_kind_names, s->kind, &skb),
					     str_id(&id, &b));
				}
				if (!shift(flp)) {
					/* unexpected Record Boundary or EOF */
					llog(RC_LOG, flp->logger,
					     "\"%s\" line %d: unexpected end of id list",
					     flp->filename, flp->lino);
					pfree(s);
					break;
				}
			}
		}
	}
}

struct lswglob_context {
	struct file_lex_position *oflp;
	struct secret **psecrets;
};

static void process_secret_files(unsigned count, char **files,
				 struct lswglob_context *context,
				 struct logger *logger UNUSED)
{
	for (unsigned i = 0; i < count; i++) {
		const char *file = files[i];
		struct file_lex_position *flp = NULL;
		if (lexopen(&flp, file, false, context->oflp)) {
			llog(RC_LOG, flp->logger,
			     "loading secrets from \"%s\"", file);
			flushline(flp, "file starts with indentation (continuation notation)");
			process_secret_records(flp, context->psecrets);
			lexclose(&flp);
		}
	}
}

static void process_secrets_file(struct file_lex_position *oflp,
				 struct secret **psecrets, const char *file_pat)
{
	if (oflp->depth > 10) {
		llog(RC_LOG, oflp->logger,
			    "preshared secrets file \"%s\" nested too deeply",
			    file_pat);
		return;
	}

	struct lswglob_context context = {
		.oflp = oflp,
		.psecrets = psecrets,
	};
	if (!lswglob(file_pat, "secrets", process_secret_files,
		     &context, oflp->logger)) {
		llog(RC_LOG, oflp->logger, "no secrets filename matched \"%s\"", file_pat);
	}
}

void lsw_free_preshared_secrets(struct secret **psecrets, struct logger *logger)
{
	lock_certs_and_keys("free_preshared_secrets", logger);

	if (*psecrets != NULL) {
		struct secret *s, *ns;

		llog(RC_LOG, logger, "forgetting secrets");

		for (s = *psecrets; s != NULL; s = ns) {
			struct id_list *i, *ni;

			ns = s->next;	/* grab before freeing s */
			for (i = s->ids; i != NULL; i = ni) {
				ni = i->next;	/* grab before freeing i */
				free_id_content(&i->id);
				pfree(i);
			}
			switch (s->kind) {
			case SECRET_PSK:
				pfreeany(s->u.preshared);
				break;
			case SECRET_PPK:
				pfreeany(s->u.ppk);
				break;
			case SECRET_XAUTH:
				pfreeany(s->u.preshared);
				break;
			case SECRET_RSA:
			case SECRET_ECDSA:
			case SECRET_EDDSA:
				secret_pubkey_stuff_delref(&s->u.pubkey, HERE);
				break;
			default:
				bad_case(s->kind);
			}
			pfree(s);
		}
		*psecrets = NULL;
	}

	unlock_certs_and_keys("free_preshared_secrets", logger);
}

void lsw_load_preshared_secrets(struct secret **psecrets, const char *secrets_file,
				struct logger *logger)
{
	lsw_free_preshared_secrets(psecrets, logger);
	struct file_lex_position flp = {
		.logger = logger,
		.depth = 0,
	};
	process_secrets_file(&flp, psecrets, secrets_file);
}

struct pubkey *pubkey_addref_where(struct pubkey *pk, where_t where)
{
	struct logger *logger = &global_logger;
	return addref_where(pk, logger, where);
}

/*
 * free a public key struct
 */

void pubkey_delref_where(struct pubkey **pkp, where_t where)
{
	const struct logger *logger = &global_logger;
	struct pubkey *pk = delref_where(pkp, logger, where);
	if (pk != NULL) {
		free_id_content(&pk->id);
		/* algorithm-specific freeing */
		pk->content.type->free_pubkey_content(&pk->content, logger);
		pfree(pk);
	}
}

/*
 * Free a public key record.
 * As a convenience, this returns a pointer to next.
 */
struct pubkey_list *free_public_keyentry(struct pubkey_list *p)
{
	struct pubkey_list *nxt = p->next;

	if (p->key != NULL)
		pubkey_delref_where(&p->key, HERE);
	pfree(p);
	return nxt;
}

void free_public_keys(struct pubkey_list **keys)
{
	while (*keys != NULL)
		*keys = free_public_keyentry(*keys);
}

/*
 * XXX: this gets called, via replace_pubkey() with a PK that is still
 * pointing into a cert.  Hence the "how screwed up is this?"  :-(
 *
 * XXX: Old comment?
 */

void add_pubkey(struct pubkey *pubkey, struct pubkey_list **head)
{
	struct pubkey_list *p = alloc_thing(struct pubkey_list, "pubkey entry");
	/* install new key at front */
	p->key = pubkey_addref(pubkey);
	p->next = *head;
	*head = p;
}

void delete_public_keys(struct pubkey_list **head,
			const struct id *id,
			const struct pubkey_type *type)
{
	struct pubkey_list **pp, *p;

	for (pp = head; (p = *pp) != NULL; ) {
		struct pubkey *pk = p->key;

		if (same_id(id, &pk->id) && pk->content.type == type)
			*pp = free_public_keyentry(p);
		else
			pp = &p->next;
	}
}

void replace_pubkey(struct pubkey *pubkey, struct pubkey_list **pubkey_db)
{
	delete_public_keys(pubkey_db, &pubkey->id, pubkey->content.type);
	add_pubkey(pubkey, pubkey_db);
}

static struct pubkey *alloc_pubkey(const struct id *id, /* ASKK */
				   enum dns_auth_level dns_auth_level,
				   realtime_t install_time, realtime_t until_time,
				   uint32_t ttl,
				   const struct pubkey_content *pkc,
				   shunk_t issuer,
				   const struct logger *logger, where_t where)
{
	PEXPECT(logger, pkc->keyid.keyid[0] != '\0');
	PEXPECT(logger, pkc->ckaid.len > 0);

	struct pubkey *pk = refcnt_overalloc(struct pubkey, issuer.len, logger, where);
	pk->content = *pkc;
	pk->id = clone_id(id, "public key id");
	pk->dns_auth_level = dns_auth_level;
	pk->installed_time = install_time;
	pk->until_time = until_time;
	pk->dns_ttl = ttl;

	/* Append any issuer to the end */
	if (issuer.len > 0) {
		memcpy(pk->end, issuer.ptr, issuer.len);
		pk->issuer = shunk2(pk->end, issuer.len);
	}

	return pk;
}

diag_t unpack_dns_pubkey_content(enum ipseckey_algorithm_type algorithm_type,
				 const shunk_t dnssec_pubkey,
				 struct pubkey_content *pubkey_content,
				 const struct logger *logger)
{
	if (algorithm_type == IPSECKEY_ALGORITHM_X_PUBKEY) {
		diag_t d = pubkey_der_to_pubkey_content(dnssec_pubkey, pubkey_content);
		if (d != NULL) {
			return d;
		}
		PASSERT(logger, pubkey_content->type != NULL);
		return NULL;
	}

	const struct pubkey_type *pubkey_type = pubkey_type_from_ipseckey_algorithm(algorithm_type);
	if (pubkey_type == NULL) {
		return diag("invalid IPSECKEY Algorithm Type %u", algorithm_type);
	}

	*pubkey_content = (struct pubkey_content) {
		.type = pubkey_type,
	};
	diag_t d = pubkey_type->extract_pubkey_content_from_ipseckey(dnssec_pubkey,
								     pubkey_content,
								     logger);
	if (d != NULL) {
		return d;
	}

	PASSERT(logger, pubkey_content->type != NULL);
	return NULL;
}

diag_t unpack_dns_pubkey(const struct id *id, /* ASKK */
			 enum dns_auth_level dns_auth_level,
			 enum ipseckey_algorithm_type algorithm_type,
			 realtime_t install_time, realtime_t until_time,
			 uint32_t ttl,
			 const shunk_t dnssec_pubkey,
			 struct pubkey **pubkey,
			 const struct logger *logger)
{

	/*
	 * First: unpack the raw public key.
	 */

	struct pubkey_content scratch_pkc;
	diag_t d = unpack_dns_pubkey_content(algorithm_type, dnssec_pubkey,
					     &scratch_pkc, logger);
	if (d != NULL) {
		return d;
	}

	/*
	 * Second: use extracted information to create the pubkey.
	 */

	(*pubkey) = alloc_pubkey(id, dns_auth_level,
				 install_time, until_time, ttl,
				 &scratch_pkc,
				 null_shunk,	/* raw keys have no issuer */
				 logger, HERE);
	return NULL;
}

const struct pubkey_type *pubkey_type_from_SECKEYPublicKey(SECKEYPublicKey *pubk)
{
	KeyType key_type = SECKEY_GetPublicKeyType(pubk);
	switch (key_type) {
	case rsaKey:
		return &pubkey_type_rsa;
	case ecKey:
	        return &pubkey_type_ecdsa;
	case edKey:
	        return &pubkey_type_eddsa;
	default:
		return NULL;
	}
}

struct secret_pubkey_stuff *secret_pubkey_stuff_addref(struct secret_pubkey_stuff *pks,
						       where_t where)
{
	struct logger *logger = &global_logger;
	return addref_where(pks, logger, where);
}

void secret_pubkey_stuff_delref(struct secret_pubkey_stuff **pks, where_t where)
{
	const struct logger *logger = &global_logger;
	struct secret_pubkey_stuff *last = delref_where(pks, logger, where);
	if (last != NULL) {
		SECKEY_DestroyPrivateKey(last->private_key);
		free_pubkey_content(&last->content, logger);
		pfree(last);
	}
}

static err_t add_private_key(struct secret **secrets,
			     struct secret_pubkey_stuff **pks,
			     const struct logger *logger,
			     SECKEYPublicKey *pubk, SECItem *ckaid_nss,
			     const struct pubkey_type *type,
			     SECKEYPrivateKey *private_key)
{
	struct pubkey_content content = {
		.type = type,
	};

	err_t err = type->extract_pubkey_content_from_SECKEYPublicKey(&content, pubk, ckaid_nss, logger);
	if (err != NULL) {
		return err;
	}

	PASSERT(logger, content.type == type); /* don't change */
	PEXPECT(logger, content.ckaid.len > 0);
	PEXPECT(logger, content.keyid.keyid[0] != '\0');

	struct secret *s = alloc_thing(struct secret, "pubkey secret");
	s->kind = type->private_key_kind;
	s->line = 0;
	/* make an unpacked copy of the private key */
	s->u.pubkey = refcnt_alloc(struct secret_pubkey_stuff, logger, HERE);
	s->u.pubkey->private_key = copy_private_key(private_key, logger);
	s->u.pubkey->content = content;

	add_secret(secrets, s, "lsw_add_rsa_secret", logger);
	*pks = s->u.pubkey;
	return NULL;
}

static err_t find_or_load_private_key_by_cert_3(struct secret **secrets, CERTCertificate *cert,
						struct secret_pubkey_stuff **pks,
						const struct logger *logger,
						SECKEYPublicKey *pubk, SECItem *ckaid_nss,
						const struct pubkey_type *type)
{

	SECKEYPrivateKey *private_key = PK11_FindKeyByAnyCert(cert, lsw_nss_get_password_context(logger));
	if (private_key == NULL)
		return "NSS: cert private key not found";
	err_t err = add_private_key(secrets, pks, logger,
				    pubk, ckaid_nss, type, private_key);
	SECKEY_DestroyPrivateKey(private_key);
	return err;
}

static err_t find_or_load_private_key_by_cert_2(struct secret **secrets, CERTCertificate *cert,
						struct secret_pubkey_stuff **pks, bool *load_needed,
						const struct logger *logger,
						SECKEYPublicKey *pubk, SECItem *ckaid_nss)
{
	/* XXX: see also nss_cert_key_kind(cert) */
	const struct pubkey_type *type = pubkey_type_from_SECKEYPublicKey(pubk);
	if (type == NULL) {
		return "NSS cert not supported";
	}

	ckaid_t ckaid = ckaid_from_secitem(ckaid_nss);
	struct secret *s = find_secret_by_pubkey_ckaid_1(*secrets, &ckaid, logger);
	if (s != NULL) {
		if (s->u.pubkey->content.type != type) {
			return "NSS private key as wrong type";
		}

		ldbg(logger, "secrets entry for certificate already exists: %s", cert->nickname);
		*pks = s->u.pubkey;
		*load_needed = false;
		return NULL;
	}

	ldbg(logger, "adding %s secret for certificate: %s", type->name, cert->nickname);
	*load_needed = true;
	err_t err = find_or_load_private_key_by_cert_3(secrets, cert, pks, logger,
						       /* extracted fields */
						       pubk, ckaid_nss, type);
	return err;
}

static err_t find_or_load_private_key_by_cert_1(struct secret **secrets, CERTCertificate *cert,
						struct secret_pubkey_stuff **pks,
						bool *load_needed,
						const struct logger *logger,
						SECKEYPublicKey *pubk)
{
	/*
	 * Getting a SECItem ptr from PK11_GetLowLevelKeyID doesn't
	 * mean that the private key exists - it is just a hash formed
	 * from the cert's public key.
	 */
	SECItem *ckaid_nss =
		PK11_GetLowLevelKeyIDForCert(NULL, cert, lsw_nss_get_password_context(logger)); /* MUST FREE */
	if (ckaid_nss == NULL) {
		return "NSS: key ID not found";
	}

	err_t err = find_or_load_private_key_by_cert_2(secrets, cert,
						       pks, load_needed, logger,
						       /* extracted fields */
						       pubk, ckaid_nss);
	SECITEM_FreeItem(ckaid_nss, PR_TRUE);
	return err;
}

err_t find_or_load_private_key_by_cert(struct secret **secrets, const struct cert *cert,
				       struct secret_pubkey_stuff **pks, bool *load_needed,
				       const struct logger *logger)
{
	*load_needed = false;

	if (cert == NULL || cert->nss_cert == NULL) {
		return "NSS cert not found";
	}

	SECKEYPublicKey *pubk = SECKEY_ExtractPublicKey(&cert->nss_cert->subjectPublicKeyInfo);
	if (pubk == NULL) {
		return "NSS: could not determine certificate kind; SECKEY_ExtractPublicKey() failed";
	}

	err_t err = find_or_load_private_key_by_cert_1(secrets, cert->nss_cert,
						       pks, load_needed, logger,
						       /* extracted fields */
						       pubk);
	SECKEY_DestroyPublicKey(pubk);
	return err;
}

static err_t find_or_load_private_key_by_ckaid_1(struct secret **secrets,
						 struct secret_pubkey_stuff **pks,
						 const struct logger *logger,
						 SECItem *ckaid_nss,
						 SECKEYPrivateKey *private_key)
{
	SECKEYPublicKey *public_key = SECKEY_ConvertToPublicKey(private_key); /* must free */
	if (public_key == NULL) {
		return "NSS private key has no public key";
	}

	const struct pubkey_type *type = pubkey_type_from_SECKEYPublicKey(public_key);
	if (type == NULL) {
		SECKEY_DestroyPublicKey(public_key);
		return "NSS private key not supported (unknown type)";
	}

	err_t err = add_private_key(secrets, pks, logger,
				    public_key, ckaid_nss,
				    type, private_key);
	SECKEY_DestroyPublicKey(public_key);
	return err;
}

/*
 * CKAIDs are unique; there should be only one match.
 *
 * XXX: caller is responsible for checking that the returned key is
 * useful.  For instance, given auth=ecdsa ckaid=<eddsa>, this will
 * return the <eddsa> key, which isn't useful.
 */
err_t find_or_load_private_key_by_ckaid(struct secret **secrets,
					const ckaid_t *ckaid,
					struct secret_pubkey_stuff **pks,
					bool *load_needed,
					const struct logger *logger)
{
	*load_needed = false;
	PASSERT(logger, ckaid != NULL);

	struct secret *s = find_secret_by_pubkey_ckaid_1(*secrets, ckaid, logger);
	if (s != NULL) {
		ldbg(logger, "secrets entry for ckaid already exists");
		*pks = s->u.pubkey;
		*load_needed = false;
		return NULL;
	}

	*load_needed = true;
	PK11SlotInfo *slot = PK11_GetInternalKeySlot();
	if (PBAD(logger, slot == NULL)) {
		return "NSS: has no internal slot ....";
	}

	SECItem ckaid_nss = same_ckaid_as_secitem(ckaid);
	/* must free */
	SECKEYPrivateKey *private_key = PK11_FindKeyByKeyID(slot, &ckaid_nss,
							    lsw_nss_get_password_context(logger));
	if (private_key == NULL) {
		/*
		 * XXX: The code loading ipsec.secrets also tries to
		 * use the CKAID to find the certificate, and then
		 * uses that to find the private key?  Why?
		 */
		return "can't find the private key matching the NSS CKAID";
	}

	ckaid_buf ckb;
	ldbg(logger, "loaded private key matching CKAID %s", str_ckaid(ckaid, &ckb));
	err_t err = find_or_load_private_key_by_ckaid_1(secrets, pks, logger,
							&ckaid_nss, private_key);
	SECKEY_DestroyPrivateKey(private_key);
	return err;
}

static diag_t create_pubkey_from_cert_1(const struct id *id,
					CERTCertificate *cert,
					SECKEYPublicKey *pubkey_nss,
					struct pubkey **pk,
					const struct logger *logger)
{
	const struct pubkey_type *type = pubkey_type_from_SECKEYPublicKey(pubkey_nss);
	if (type == NULL) {
		id_buf idb;
		return diag("NSS: could not create public key with ID '%s': certificate '%s' has an unknown key kind",
			    str_id(id, &idb),
			    cert->nickname);
	}

	SECItem *ckaid_nss = PK11_GetLowLevelKeyIDForCert(NULL, cert,
							  lsw_nss_get_password_context(logger)); /* must free */
	if (ckaid_nss == NULL) {
		/* someone deleted CERT from the NSS DB */
		id_buf idb;
		return diag("NSS: could not create public key with ID '%s': extract CKAID from certificate '%s' failed",
			    str_id(id, &idb),
			    cert->nickname);
	}

	struct pubkey_content pkc = {
		.type = type,
	};

	err_t err = type->extract_pubkey_content_from_SECKEYPublicKey(&pkc, pubkey_nss, ckaid_nss, logger);
	if (err != NULL) {
		SECITEM_FreeItem(ckaid_nss, PR_TRUE);
		id_buf idb;
		return diag("NSS: could not create public key with ID '%s': %s",
			    str_id(id, &idb),
			    err);
	}
	PASSERT(logger, pkc.type != NULL);

	realtime_t install_time = realnow();
	realtime_t until_time;
	PRTime not_before, not_after;
	if (CERT_GetCertTimes(cert, &not_before, &not_after) != SECSuccess) {
		until_time = realtime(-1);
	} else {
		until_time = realtime(not_after / PR_USEC_PER_SEC);
	}
	*pk = alloc_pubkey(id, /*dns_auth_level*/0/*default*/,
			   install_time, until_time,
			   /*ttl*/0, &pkc,
			   same_secitem_as_shunk(cert->derIssuer),
			   logger, HERE);
	SECITEM_FreeItem(ckaid_nss, PR_TRUE);
	return NULL;
}

diag_t create_pubkey_from_cert(const struct id *id,
			       CERTCertificate *cert, struct pubkey **pk,
			       const struct logger *logger)
{
	if (PBAD(logger, cert == NULL)) {
		return NULL;
	}

	id_buf idb;
	ldbg(logger, "creating pubkey for ID %s", str_id(id, &idb));

	/*
	 * Try to convert CERT to an internal PUBKEY object.  If
	 * someone, in parallel, deletes the underlying cert from the
	 * NSS DB, then this will fail.
	 */
	SECKEYPublicKey *pubkey_nss = SECKEY_ExtractPublicKey(&cert->subjectPublicKeyInfo); /* must free */
	if (pubkey_nss == NULL) {
		id_buf idb;
		return diag("NSS: could not create public key with ID '%s': extracting public key from certificate '%s' failed",
			    str_id(id, &idb), cert->nickname);
	}

	diag_t d = create_pubkey_from_cert_1(id, cert, pubkey_nss, pk, logger);
	SECKEY_DestroyPublicKey(pubkey_nss);
	return d;
}

void free_pubkey_content(struct pubkey_content *pubkey_content,
			 const struct logger *logger)
{
	pubkey_content->type->free_pubkey_content(pubkey_content, logger);
}

struct hash_signature pubkey_hash_then_sign(const struct pubkey_signer *signer,
					    const struct hash_desc *hasher,
					    const struct secret_pubkey_stuff *pks,
					    const struct hash_hunks *hunks,
					    struct logger *logger)
{
	struct crypt_mac hash_to_sign = crypt_hash_hunks("hash-to-sign", hasher,
							 hunks, logger);
	struct hash_signature sig = signer->sign_hash(pks,
						      hash_to_sign.ptr,
						      hash_to_sign.len,
						      hasher,
						      logger);
	passert(sig.len <= sizeof(sig.ptr/*array*/));
	return sig;
}
