/* IKEv2 message routines, for Libreswan
 *
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2008-2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2010,2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012-2017 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Sahana Prasad <sahana.prasad07@gmail.com>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2017 Mayank Totale <mtotale@gmail.com>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
 * Copyright (C) 2020 Nupur Agrawal <nupur202000@gmail.com>
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


#include "defs.h"

#include "log.h"
#include "ikev2_message.h"
#include "server.h"
#include "state.h"
#include "connections.h"
#include "ike_alg.h"
#include "crypt_cipher.h"
#include "pluto_stats.h"
#include "demux.h"	/* for struct msg_digest */
#include "rnd.h"
#include "crypt_prf.h"
#include "send.h"	/* record_outbound_ike_message() */
#include "ip_info.h"
#include "iface.h"
#include "ip_protocol.h"
#include "ikev2_send.h"
#include "ikev2_notification.h"

static bool encrypt_v2SK_payload(struct v2SK_payload *sk);

/*
 * Determine the IKE version we will use for the IKE packet
 * Normally, this is "2.0", but in the future we might need to
 * change that. Version used is the minimum 2.x version both
 * sides support. So if we support 2.1, and they support 2.0,
 * we should sent 2.0 (not implemented until we hit 2.1 ourselves)
 * We also have some impair functions that modify the major/minor
 * version on purpose - for testing
 *
 * rcv_version: the received IKE version, 0 if we don't know
 *
 * top 4 bits are major version, lower 4 bits are minor version
 */
static uint8_t build_ikev2_version(void)
{
	/* TODO: if bumping, we should also set the Version flag in the ISAKMP header */
	return ((IKEv2_MAJOR_VERSION + (impair.major_version_bump ? 1 : 0))
			<< ISA_MAJ_SHIFT) |
	       (IKEv2_MINOR_VERSION + (impair.minor_version_bump ? 1 : 0));
}

uint8_t build_ikev2_critical(bool impaired, struct logger *logger)
{
	uint8_t octet = 0;
	if (impaired) {
		/* flip the expected bit */
		llog(RC_LOG, logger, "IMPAIR: setting (should be off) critical payload bit");
		octet = ISAKMP_PAYLOAD_CRITICAL;
	} else {
		octet = ISAKMP_PAYLOAD_NONCRITICAL;
	}
	if (impair.send_bogus_payload_flag) {
		llog(RC_LOG, logger, "IMPAIR: adding bogus bit to critical octet");
		octet |= ISAKMP_PAYLOAD_FLAG_LIBRESWAN_BOGUS;
	}
	return octet;
}

/*
 * Open an IKEv2 message, return the message body.
 *
 * Request: IKE, which must be non-NULL, is used to determine the IKE
 * SA Initiator / Responder role; MD must be NULL (after all a request
 * has no response).
 *
 * Response: If IKE is non-NULL then it is used to determine the IKE
 * SA Initiator / Responder role (NULL implies IKE SA Initiator); MD
 * must be non-NULL (the message being responded to).
 */

static bool open_v2_message_body(struct pbs_out *message,
				 const struct ike_sa *ike,
				 const struct msg_digest *md,
				 enum ikev2_exchange exchange_type,
				 struct pbs_out *body)
{
	*body = (struct pbs_out) {0};

	/* at least one, possibly both */
	PASSERT(ike->sa.logger, ike != NULL || md != NULL);

	struct isakmp_hdr hdr = {
		.isa_flags = impair.send_bogus_isakmp_flag ? ISAKMP_FLAGS_RESERVED_BIT6 : LEMPTY,
		.isa_version = build_ikev2_version(),
		.isa_xchg = exchange_type,
		.isa_length = 0, /* filled in when PBS is closed */
	};

	/*
	 * I (Initiator) flag
	 *
	 * If there is an IKE SA, the sa_role can be used.
	 *
	 * If there is no IKE SA, then, presumably, this is a response
	 * to an initial exchange and the flag should be clear.
	 *
	 * The other possibility is that this is a response to an
	 * IKEv++ message, just assume this is the initial exchange
	 * and the I flag should be clear (see 1.5.  Informational
	 * Messages outside of an IKE SA).  The other option would be
	 * to flip MD's I bit, but since this is IKEv++, there may not
	 * even be an I bit.
	 */
	enum sa_role sa_role = (ike != NULL ? ike->sa.st_sa_role : SA_RESPONDER);
	if (sa_role == SA_INITIATOR) {
		hdr.isa_flags |= ISAKMP_FLAGS_v2_IKE_I;
	}

	/*
	 * R (Response) flag
	 *
	 * If there's no MD, then this must be a new exchange request
	 * - R(Responder) flag clear.
	 *
	 * If there is an MD, then this must be a response -
	 * R(Responder) flag set.
	 *
	 * Note that when MD!= NULL, v2_msg_role() can't be called (as
	 * a cross check) as this code used to force a response to a
	 * message that is close to bogus requests (1.5.
	 * Informational Messages outside of an IKE SA - where the
	 * response is forced.
	 */
	enum message_role message_role = (md != NULL ? MESSAGE_RESPONSE : MESSAGE_REQUEST);
	if (message_role == MESSAGE_RESPONSE) {
		hdr.isa_flags |= ISAKMP_FLAGS_v2_MSG_R;
	}

	/*
	 * SPI (aka cookies).
	 */
	if (ike != NULL) {
		/*
		 * Note that when the original initiator sends the
		 * IKE_SA_INIT request, the still zero SPIr will be
		 * copied.
		 */
		hdr.isa_ike_initiator_spi = ike->sa.st_ike_spis.initiator;
		hdr.isa_ike_responder_spi = ike->sa.st_ike_spis.responder;
	} else {
		/*
		 * Either error response notification to IKE_SA_INIT
		 * or "Informational Messages outside of an IKE SA".
		 * Use the IKE SPIs from the request.
		 */
		PASSERT(ike->sa.logger, md != NULL);
		hdr.isa_ike_initiator_spi = md->hdr.isa_ike_initiator_spi;
		hdr.isa_ike_responder_spi = md->hdr.isa_ike_responder_spi;
	}

	/*
	 * Message ID
	 */
	if (md != NULL) {
		/*
		 * Since there is a message digest (MD) it is assumed
		 * to contain a message request.  Presumably this open
		 * is for the message response - use the Message ID
		 * from the request.  A better choice would be
		 * .st_v2_msgid_windows.responder.recv+1, but it isn't
		 * clear if/when that value is updated and the IKE SA
		 * isn't always available.
		 */
		hdr.isa_msgid = md->hdr.isa_msgid;
	} else {
		/*
		 * If it isn't a response then use the IKE SA's
		 * .st_v2_msgid_windows.initiator.sent+1.  The field
		 * will be updated as part of finishing the state
		 * transition and sending the message.
		 */
		PASSERT(md->logger, ike != NULL);
		hdr.isa_msgid = ike->sa.st_v2_msgid_windows.initiator.sent + 1;
	}

	if (impair.bad_ike_auth_xchg) {
		llog(RC_LOG, ike->sa.logger, "IMPAIR: Instead of replying with IKE_AUTH, forging an INFORMATIONAL reply");
		if ((hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) && exchange_type == ISAKMP_v2_IKE_AUTH) {
			hdr.isa_xchg = ISAKMP_v2_INFORMATIONAL;
		}
	}

	if (!pbs_out_struct(message, hdr, &isakmp_hdr_desc, body)) {
		/* already logged */
		return false;
	}

	return true;
}

/*
 * This code assumes that the encrypted part of an IKE message starts
 * with an Initialization Vector (IV) of enc_blocksize of random
 * octets.  The IV will subsequently be discarded after decryption.
 * This is true of Cipher Block Chaining mode (CBC).
 */
static bool pad_v2SK_iv(struct v2SK_payload *sk)
{
	/* compute location/size */
	sk->wire_iv = chunk2(sk->pbs.cur, sk->ike->sa.st_oakley.ta_encrypt->wire_iv_size);
	/* make space */
	if (!pbs_out_zero(&sk->pbs, sk->wire_iv.len, "IV")) {
		/* already logged */
		return false; /*fatal*/
	}
	return true;
}

static bool open_body_v2SK_payload(struct pbs_out *container,
				   struct ike_sa *ike,
				   struct logger *logger,
				   struct v2SK_payload *sk)
{
	*sk = (struct v2SK_payload) {
		.logger = logger,
		.ike = ike,
		.payload = {
		    .ptr = container->cur,
		    .len = 0,	/* computed at end; set here to silence GCC 6.10 */
		}
	};

	/* emit Encryption Payload header */

	struct ikev2_generic e = {
		.isag_length = 0, /* filled in later */
		.isag_critical = build_ikev2_critical(false, ike->sa.logger),
	};
	sk->header = shunk2(container->cur, sizeof(e));
	if (!pbs_out_struct(container, e, &ikev2_sk_desc, &sk->pbs)) {
		llog(RC_LOG, logger,
		     "error initializing SK header for encrypted %s message",
		     container->name);
		return false;
	}

	/*
	 * Additional Authenticated Data - AAD - is everything so far:
	 * i.e., the IKE header and payload headers.
	 *
	 * RFC5282 says: The Initialization Vector and Ciphertext
	 * fields [...] MUST NOT be included in the associated data.
	 */

	sk->aad = shunk2(sk->pbs.container->start, sk->pbs.cur - sk->pbs.container->start);

	/* emit space for IV, and save location */

	if (!pad_v2SK_iv(sk)) {
		/* already logged */
		return false;
	}

	/* save cleartext start */

	sk->cleartext.ptr = sk->pbs.cur;
	sk->cleartext.len = 0; /* to be determined */

	PASSERT(sk->logger, sk->wire_iv.ptr <= sk->cleartext.ptr);

	/*
	 * XXX: coverity thinks .container (set to E by
	 * pbs_out_struct() above) can be NULL.
	 */
	PASSERT(sk->logger, sk->pbs.container != NULL && sk->pbs.container->name == container->name);

	ldbg(sk->logger, "%s() %s=[%zu,%zu) %s=[%zu,%zu) %s=[%zu,%zu)",
	     __func__,
#define R(N) #N, (const uint8_t *)sk->N.ptr - (const uint8_t *)sk->payload.ptr, sk->N.len
	     R(aad),
	     R(header),
	     R(wire_iv));
#undef R

	return true;
}

static bool close_v2SK_payload(struct v2SK_payload *sk)
{
	/*
	 * Save cleartext end, excluding any padding.
	 *
	 * When chopping .cleartext into fragments, the last fragment
	 * will get its own padding.
	 */

	sk->cleartext.len = sk->pbs.cur - sk->cleartext.ptr;

	/* emit padding + pad-length */

	size_t padding;
	if (sk->ike->sa.st_oakley.ta_encrypt->pad_to_blocksize) {
		const size_t blocksize = sk->ike->sa.st_oakley.ta_encrypt->enc_blocksize;
		padding = pad_up(sk->pbs.cur - sk->cleartext.ptr, blocksize);
		if (padding == 0) {
			padding = blocksize;
		}
	} else {
		padding = 1;
	}
	sk->padding = chunk2(sk->pbs.cur, padding);
	ldbg(sk->logger, "adding %zd bytes of padding (including 1 byte padding-length)", padding);
	for (unsigned i = 0; i < padding; i++) {
		if (!pbs_out_repeated_byte(&sk->pbs, i, 1, "padding and length")) {
			/* already logged */
			return false; /*fatal*/
		}
	}
	PASSERT(sk->logger, sk->padding.ptr + sk->padding.len == sk->pbs.cur);

	/* emit space for integrity checksum data; save location */

	size_t integ_size = (encrypt_desc_is_aead(sk->ike->sa.st_oakley.ta_encrypt)
			     ? sk->ike->sa.st_oakley.ta_encrypt->aead_tag_size
			     : sk->ike->sa.st_oakley.ta_integ->integ_output_size);
	if (integ_size == 0) {
		llog_pexpect(sk->logger, HERE,
			     "error initializing integrity checksum for encrypted %s payload",
			     sk->pbs.container->name);
		return false;
	}
	sk->integrity = chunk2(sk->pbs.cur, integ_size);
	if (!pbs_out_zero(&sk->pbs, integ_size, "length of truncated HMAC/KEY")) {
		/* already logged */
		return false; /*fatal*/
	}

	/* close the SK payload */

	sk->payload.len = sk->pbs.cur - sk->payload.ptr;
	close_pbs_out(&sk->pbs);

	ldbg(sk->logger, "%s() payload=%zu bytes %s=[%zu,%zu) %s=[%zu,%zu) %s=[%zu,%zu) %s=[%zu,%zu) %s=[%zu,%zu) %s=[%zu,%zu)",
	     __func__, sk->padding.len,
#define R(N) #N, (const uint8_t *)sk->N.ptr - (const uint8_t *)sk->payload.ptr, sk->N.len
	     R(aad),
	     R(header),
	     R(wire_iv),
	     R(cleartext),
	     R(padding),
	     R(integrity));
#undef R
	return true;
}

bool encrypt_v2SK_payload(struct v2SK_payload *sk)
{
	struct ike_sa *ike = sk->ike;
	struct logger *logger = sk->logger;

	/*
	 * Will encrypt .cleartext + .padding (they are assumed to be
	 * contiguous).
	 *
	 * Just note that .cleartext doesn't include the padding.
	 * This is because when .cleartext gets chopped up into
	 * fragments the last fragment does its own padding.
	 */
	ldbg(sk->logger, "%p %zu %p", sk->aad.ptr, sk->aad.len, sk->wire_iv.ptr);
	PASSERT(sk->logger, sk->aad.ptr + sk->aad.len == sk->wire_iv.ptr);
	PASSERT(sk->logger, sk->wire_iv.ptr + sk->wire_iv.len == sk->cleartext.ptr);
	PASSERT(sk->logger, sk->cleartext.ptr + sk->cleartext.len == sk->padding.ptr);
	PASSERT(sk->logger, sk->padding.ptr + sk->padding.len == sk->integrity.ptr);
	chunk_t enc = chunk2(sk->cleartext.ptr, sk->cleartext.len + sk->padding.len);

	chunk_t salt;
	PK11SymKey *authkey;
	/* encrypt with our end's key */
	switch (ike->sa.st_sa_role) {
	case SA_INITIATOR:
		authkey = ike->sa.st_skey_ai_nss;
		salt = ike->sa.st_skey_initiator_salt;
		break;
	case SA_RESPONDER:
		authkey = ike->sa.st_skey_ar_nss;
		salt = ike->sa.st_skey_responder_salt;
		break;
	default:
		bad_case(ike->sa.st_sa_role);
	}

	/* now, encrypt */
	if (LDBGP(DBG_CRYPT, logger)) {
		LDBG_log(sk->logger, "data before [authenticated] encryption:");
		LDBG_hunk(sk->logger, &enc);
		LDBG_log(sk->logger, "integ before [authenticated] encryption:");
		LDBG_hunk(sk->logger, &sk->integrity);
	}

	/* encrypt and authenticate the block */
	if (encrypt_desc_is_aead(ike->sa.st_oakley.ta_encrypt)) {
		PEXPECT(sk->logger, sk->integrity.len == ike->sa.st_oakley.ta_encrypt->aead_tag_size);
		chunk_t text_and_tag = chunk2(enc.ptr, enc.len + sk->integrity.len);

		/* now, encrypt */
		if (LDBGP(DBG_CRYPT, logger)) {
		    LDBG_log_hunk(logger, "salt before authenticated encryption:", &salt);
		    LDBG_log_hunk(logger, "IV before authenticated encryption:", &sk->wire_iv);
		    LDBG_log_hunk(logger, "AAD before authenticated encryption:", &sk->aad);
		}

		if (!cipher_context_op_aead(ike->sa.st_ike_encrypt_cipher_context,
					    sk->wire_iv, HUNK_AS_SHUNK(&sk->aad),
					    text_and_tag, enc.len, sk->integrity.len,
					    sk->logger)) {
			return false;
		}

	} else {
		/* note: no iv is longer than MAX_CBC_BLOCK_SIZE */
		cipher_context_op_normal(ike->sa.st_ike_encrypt_cipher_context,
					 sk->wire_iv, enc, /*ikev1_iv*/NULL,
					 sk->logger);

		/* note: saved_iv's updated value is discarded */

		/* okay, authenticate from beginning of IV */
		struct crypt_prf *ctx = crypt_prf_init_symkey("integ", ike->sa.st_oakley.ta_integ->prf,
							      "authkey", authkey, sk->logger);
		shunk_t message = shunk2(sk->aad.ptr, sk->integrity.ptr - (const uint8_t*)sk->aad.ptr);
		crypt_prf_update_hunk(ctx, "message", message);
		PASSERT(sk->logger, sk->integrity.len == ike->sa.st_oakley.ta_integ->integ_output_size);
		struct crypt_mac mac = crypt_prf_final_mac(&ctx, ike->sa.st_oakley.ta_integ);
		memcpy_hunk(sk->integrity.ptr, mac, sk->integrity.len);

		if (LDBGP(DBG_CRYPT, logger)) {
			LDBG_log(sk->logger, "data being hmac:");
			LDBG_hunk(sk->logger, &message);
			LDBG_log(sk->logger, "out calculated auth:");
			LDBG_hunk(sk->logger, &sk->integrity);
		}
	}

	if (LDBGP(DBG_CRYPT, logger)) {
		LDBG_log(sk->logger, "data after [authenticated] encryption:");
		LDBG_hunk(sk->logger,  &enc);
		LDBG_log(sk->logger, "integ after [authenticated] encryption:");
		LDBG_hunk(sk->logger, &sk->integrity);
	}

	return true;
}

/*
 * ikev2_decrypt_msg: decode the payload.
 * The result is stored in-place.
 * Calls ikev2_process_payloads to decode the payloads within.
 *
 * This code assumes that the encrypted part of an IKE message starts
 * with an Initialization Vector (IV) of WIRE_IV_SIZE random octets.
 * We will discard the IV after decryption.
 *
 * The (optional) salt, wire-iv, and (optional) 1 are combined to form
 * the actual starting-variable (a.k.a. IV).
 */

static bool verify_and_decrypt_v2_message(struct ike_sa *ike,
					  chunk_t text,
					  shunk_t *plain,
					  size_t iv_offset)
{
	struct logger *logger = ike->sa.logger;

	if (!ike->sa.hidden_variables.st_skeyid_calculated) {
		endpoint_buf b;
		llog_pexpect(ike->sa.logger, HERE,
			     "received encrypted packet from %s but no exponents for state "PRI_SO" to decrypt it",
			     str_endpoint_sensitive(&ike->sa.st_remote_endpoint, &b),
			     pri_so(ike->sa.st_serialno));
		return false;
	}

	chunk_t wire_iv = chunk2(text.ptr + iv_offset, ike->sa.st_oakley.ta_encrypt->wire_iv_size);
	size_t integ_size = (encrypt_desc_is_aead(ike->sa.st_oakley.ta_encrypt)
			     ? ike->sa.st_oakley.ta_encrypt->aead_tag_size
			     : ike->sa.st_oakley.ta_integ->integ_output_size);

	/*
	 * check to see if length is plausible:
	 * - wire-IV
	 * - encoded data (possibly empty)
	 * - at least one padding-length byte
	 * - truncated integrity digest / tag
	 */
	uint8_t *payload_end = text.ptr + text.len;
	if (payload_end < (wire_iv.ptr + wire_iv.len + 1 + integ_size)) {
		llog_sa(RC_LOG, ike,
			  "encrypted payload impossibly short (%tu)",
			  payload_end - wire_iv.ptr);
		return false;
	}

	uint8_t *auth_start = text.ptr;
	chunk_t integ = chunk2(payload_end - integ_size, integ_size);
	chunk_t enc = chunk2(wire_iv.ptr + wire_iv.len,
			     integ.ptr - wire_iv.ptr - wire_iv.len);

	/*
	 * Check that the payload is block-size aligned.
	 *
	 * Per rfc7296 "the recipient MUST accept any length that
	 * results in proper alignment".
	 *
	 * Do this before the payload's integrity has been verified as
	 * block-alignment requirements aren't exactly secret
	 * (originally this was being done between integrity and
	 * decrypt).
	 */
	size_t enc_blocksize = ike->sa.st_oakley.ta_encrypt->enc_blocksize;
	bool pad_to_blocksize = ike->sa.st_oakley.ta_encrypt->pad_to_blocksize;
	if (pad_to_blocksize) {
		if (enc.len % enc_blocksize != 0) {
			llog_sa(RC_LOG, ike,
				  "discarding invalid packet: %zu octet payload length is not a multiple of encryption block-size (%zu)",
				  enc.len, enc_blocksize);
			return false;
		}
	}

	chunk_t salt;
	PK11SymKey *authkey;
	switch (ike->sa.st_sa_role) {
	case SA_INITIATOR:
		/* need responders key */
		authkey = ike->sa.st_skey_ar_nss;
		salt = ike->sa.st_skey_responder_salt;
		break;
	case SA_RESPONDER:
		/* need initiators key */
		authkey = ike->sa.st_skey_ai_nss;
		salt = ike->sa.st_skey_initiator_salt;
		break;
	default:
		bad_case(ike->sa.st_sa_role);
	}

	/* authenticate and decrypt the block. */

	if (encrypt_desc_is_aead(ike->sa.st_oakley.ta_encrypt)) {
		/*
		 * Additional Authenticated Data - AAD - size.
		 * RFC5282 says: The Initialization Vector and Ciphertext
		 * fields [...] MUST NOT be included in the associated
		 * data.
		 */
		shunk_t aad = shunk2(auth_start, enc.ptr - auth_start - wire_iv.len);
		chunk_t text_and_tag = chunk2(enc.ptr, enc.len + integ.len);

		/* decrypt */
		if (LDBGP(DBG_CRYPT, logger)) {
			LDBG_log_hunk(logger, "salt before authenticated decryption:", &salt);
			LDBG_log_hunk(logger, "IV before authenticated decryption:", &wire_iv);
			LDBG_log_hunk(logger, "AAD before authenticated decryption:", &aad);
			LDBG_log_hunk(logger, "integ before authenticated decryption:", &integ);
			LDBG_log_hunk(logger, "payload before decryption:", &enc);
		}

		if (!cipher_context_op_aead(ike->sa.st_ike_decrypt_cipher_context,
					    wire_iv, aad,
					    text_and_tag, enc.len, integ.len,
					    ike->sa.logger)) {
			return false;
		}

		if (LDBGP(DBG_CRYPT, logger)) {
			LDBG_log(ike->sa.logger, "data after authenticated decryption:");
			LDBG_hunk(ike->sa.logger, &enc);
			LDBG_hunk(ike->sa.logger, &integ);
		}

	} else {
		/*
		 * check authenticator.  The last INTEG_SIZE bytes are
		 * the truncated digest.
		 */
		struct crypt_prf *ctx = crypt_prf_init_symkey("auth", ike->sa.st_oakley.ta_integ->prf,
							      "authkey", authkey, ike->sa.logger);
		crypt_prf_update_bytes(ctx, "message", auth_start, integ.ptr - auth_start);
		struct crypt_mac td = crypt_prf_final_mac(&ctx, ike->sa.st_oakley.ta_integ);

		if (!hunk_memeq(td, integ.ptr, integ.len)) {
			llog_sa(RC_LOG, ike, "failed to match authenticator");
			return false;
		}

		ldbg(ike->sa.logger, "authenticator matched");

		if (LDBGP(DBG_CRYPT, logger)) {
			LDBG_log(ike->sa.logger, "payload before decryption:");
			LDBG_hunk(ike->sa.logger, &enc);
		}

		/* note: no iv is longer than MAX_CBC_BLOCK_SIZE */
		cipher_context_op_normal(ike->sa.st_ike_decrypt_cipher_context,
					 wire_iv, enc, /*ikev1_iv*/NULL,
					 ike->sa.logger);

		if (LDBGP(DBG_CRYPT, logger)) {
			LDBG_log(ike->sa.logger, "payload after decryption:");
			LDBG_hunk(ike->sa.logger, &enc);
		}

	}

	/*
	 * Check the padding.
	 *
	 * Per rfc7296 "The sender SHOULD set the Pad Length to the
	 * minimum value that makes the combination of the payloads,
	 * the Padding, and the Pad Length a multiple of the block
	 * size, but the recipient MUST accept any length that results
	 * in proper alignment."
	 *
	 * Notice the "should".  RACOON, for instance, sends extra
	 * blocks of padding that contain random bytes.
	 */
	uint8_t padlen = enc.ptr[enc.len - 1] + 1;
	if (padlen > enc.len) {
		llog_sa(RC_LOG, ike,
			  "discarding invalid packet: padding-length %u (octet 0x%02x) is larger than %zu octet payload length",
			  padlen, padlen - 1, enc.len);
		return false;
	}
	if (pad_to_blocksize) {
		if (padlen > enc_blocksize) {
			/* probably racoon */
			ldbg(ike->sa.logger, "payload contains %zu blocks of extra padding (padding-length: %d (octet 0x%2x), encryption block-size: %zu)",
			    (padlen - 1) / enc_blocksize,
			    padlen, padlen - 1, enc_blocksize);
		}
	} else {
		if (padlen > 1) {
			ldbg(ike->sa.logger, "payload contains %u octets of extra padding (padding-length: %u (octet 0x%2x))",
			    padlen - 1, padlen, padlen - 1);
		}
	}

	/*
	 * Don't check the contents of the pad octets; racoon, for
	 * instance, sets them to random values.
	 */
	ldbg(ike->sa.logger, "stripping %u octets as pad", padlen);
	*plain = shunk2(enc.ptr, enc.len - padlen);

	return true;
}

/*
 * Incoming IKEv2 fragments.
 */

static const char *ignore_v2_incoming_fragment(struct v2_incoming_fragments *frags,
					       struct msg_digest *md,
					       struct ikev2_skf *skf_hdr)
{
	/* Sanity check header */

	if (skf_hdr->isaskf_number == 0) {
		return "fragment number must be 1 or greater (not 0)";
	}

	if (skf_hdr->isaskf_number > skf_hdr->isaskf_total) {
		return "fragment number must be no greater than the total number of fragments";
	}

	if (skf_hdr->isaskf_total > MAX_IKE_FRAGMENTS) {
		return "total number of fragments must be no more than MAX_IKE_FRAGMENTS";
	}

	if (skf_hdr->isaskf_number == 1 && skf_hdr->isaskf_np == ISAKMP_NEXT_v2NONE) {
		return "first fragment's next payload must not be ISAKMP_NEXT_v2NONE";
	}

	if (skf_hdr->isaskf_number > 1 && skf_hdr->isaskf_np != ISAKMP_NEXT_v2NONE) {
		return "later fragment's next payload must be ISAKMP_NEXT_v2NONE";
	}

	/* header's ok; if it's the first it is always good */

	if (frags == NULL) {
		return NULL;
	}

	/* is it consistent with previous fragments */

	if (md->hdr.isa_xchg != frags->xchg) {
		return "message exchange does not match";
	}

	if (frags->total == 0) {
		return "message is not fragmented";
	}

	if (skf_hdr->isaskf_total != frags->total) {
		return "fragment total changed";
	}

	if (frags->frags[skf_hdr->isaskf_number].text.ptr != NULL) {
		/* retain earlier fragment with same index */
		return "repeat";
	}

	return NULL;
}

/*
 * IKEv2 message fragment:
 *
 *                       1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * message_pbs.start (AAD):
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                       IKE SA Initiator's SPI                  |
 *   |                                                               |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                       IKE SA Responder's SPI                  |
 *   |                                                               |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |  Next Payload | MjVer | MnVer | Exchange Type |     Flags     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                          Message ID                           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                            Length                             |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * skf_pbs.start:
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | Next Payload  |C|  RESERVED   |         Payload Length        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |        Fragment Number        |        Total Fragments        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * skf_pbs.cur:
 * pointed to by iv_offset; IV length determined by crypto suite
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                     Initialization Vector                     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * cipher text (plain text after trimming):
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   ~                      Encrypted content                        ~
 *   +               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |               |             Padding (0-255 octets)            |
 *   +-+-+-+-+-+-+-+-+                               +-+-+-+-+-+-+-+-+
 *   |                                               |  Pad Length   |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   ~                    Integrity Checksum Data                    ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * skf_pbs.roof:
 * message_pbs.roof:
 */

enum collected_fragment collect_v2_incoming_fragment(struct ike_sa *ike,
						     struct msg_digest *md,
						     struct v2_incoming_fragments **frags)
{
	if (!ike->sa.st_v2_ike_fragmentation_enabled) {
		llog_sa(RC_LOG, ike, "ignoring fragment as peer never proposed fragmentation");
		return FRAGMENT_IGNORED;
	}

	struct ikev2_skf *skf_hdr = &md->chain[ISAKMP_NEXT_v2SKF]->payload.v2skf;

	ldbg(ike->sa.logger, "received IKE encrypted fragment number '%u', total number '%u', next payload '%u'",
	     skf_hdr->isaskf_number, skf_hdr->isaskf_total, skf_hdr->isaskf_np);

	const char *why = ignore_v2_incoming_fragment((*frags), md, skf_hdr);
	if (why != NULL) {
		llog_sa(RC_LOG, ike, "dropping fragment %u of %u as %s",
			skf_hdr->isaskf_number, skf_hdr->isaskf_total, why);
		return FRAGMENT_IGNORED;
	}

	/* locate what is interesting */

	/* entire SKF pbs; cur points past SKF header */
	const struct pbs_in *skf_pbs = &md->chain[ISAKMP_NEXT_v2SKF]->pbs;
	/* entire payload: AAD+SKF-header+IV+cipher-text) */
	chunk_t text = chunk2(md->packet.ptr, skf_pbs->roof - md->packet.ptr);
	/* first thing after SKF header is Initialization Vector */
	unsigned iv_offset = skf_pbs->cur - md->packet.ptr;

	/* if possible, decrypt (in place) */

	shunk_t plain = null_shunk;
	if (ike->sa.hidden_variables.st_skeyid_calculated) {
		/*
		 * Try to decrypt in-place.
		 *
		 * If this fails, don't even bother saving the
		 * packet.
		 *
		 * After the call, PLAIN is pointing at the
		 * decrypted plain-text within TEXT.
		 */
		if (!verify_and_decrypt_v2_message(ike, text, &plain,
						   iv_offset)) {
			llog_sa(RC_LOG, ike,
				"fragment %u of %u invalid",
				skf_hdr->isaskf_number,
				skf_hdr->isaskf_total);
			return FRAGMENT_IGNORED;
		}
		ldbg(ike->sa.logger, "fragment %u of %u decrypted",
		     skf_hdr->isaskf_number,
		     skf_hdr->isaskf_total);
	}

	/* make space for the fragment (it's worth saving) */

	if ((*frags) == NULL) {
		*frags = alloc_thing(struct v2_incoming_fragments, "incoming v2_ike_rfrags");
		(*frags)->total = skf_hdr->isaskf_total;
		(*frags)->xchg = md->hdr.isa_xchg;
		/* may not be fragment 1; will replace when it arrives */
		(*frags)->md = md_addref(md);
	}

	/* save the fragment */

	PASSERT(ike->sa.logger, (*frags)->count < (*frags)->total);
	(*frags)->count++;
	struct v2_incoming_fragment *frag = &(*frags)->frags[skf_hdr->isaskf_number];
	PASSERT(ike->sa.logger, skf_hdr->isaskf_number < elemsof((*frags)->frags));
	PASSERT(ike->sa.logger, frag->text.ptr == NULL);
	PASSERT(ike->sa.logger, frag->plain.ptr == NULL);
	PASSERT(ike->sa.logger, frag->text.len == 0);
	PASSERT(ike->sa.logger, frag->plain.len == 0);
	frag->text = clone_hunk_as_chunk(&text, "incoming IKEv2 encrypted fragment");
	frag->iv_offset = iv_offset;
	if (ike->sa.hidden_variables.st_skeyid_calculated) {
		/*
		 * Since TEXT has been decrypted (PLAIN points into
		 * TEXT at the unencrypted blob), update frag's .plain
		 * so it points in frag's .text.
		 */
		ptrdiff_t plain_offset = (const uint8*)plain.ptr - (const uint8_t*)text.ptr;
		frag->plain = shunk2(frag->text.ptr + plain_offset, plain.len);
	}

	/*
	 * Additionally, save fragment 1.  The first fragment's
	 * message is needed for two reasons:
	 *
	 * - the next-payload field in the first fragment's SKF header
	 * is used as the next-payload field of the reconstituted SK
	 * payload header
	 *
	 * - any unencrypted payloads from the first message are
	 * included in the reconstituted message
	 *
	 * RFC 7383:
	 *  2.5.3.  Fragmenting Messages Containing Unprotected Payloads
	 *
	 *  Currently, there are no IKEv2 exchanges that define
	 *  messages, containing both unprotected payloads and
	 *  payloads, that are protected by the Encrypted payload.
	 *  However, IKEv2 does not prohibit such construction.  If
	 *  some future IKEv2 extension defines such a message and it
	 *  needs to be fragmented, all unprotected payloads MUST be
	 *  placed in the first fragment (with the Fragment Number
	 *  field equal to 1), along with the Encrypted Fragment
	 *  payload, which MUST be present in every IKE Fragment
	 *  message and be the last payload in it.
	 *
	 * XXX: to be honest, the use of "protected" here seems
	 * confused.  The entire message is protected, it is just that
	 * the SKF payload is encrypted.
	 *
	 * If .md contains some other fragment saved earlier by the
	 * above, replace it now.
	 */
	if (skf_hdr->isaskf_number == 1) {
		(*frags)->first_np = skf_hdr->isaskf_np;
		md_delref(&(*frags)->md);
		(*frags)->md = md_addref(md);
	}

	return (*frags)->count == (*frags)->total ? FRAGMENTS_COMPLETE : FRAGMENTS_MISSING;
}

bool decrypt_v2_incoming_fragments(struct ike_sa *ike,
				   struct v2_incoming_fragments **frags)
{
	for (unsigned i = 1; i <= (*frags)->total; i++) {
		struct v2_incoming_fragment *frag = &(*frags)->frags[i];
		if (frag->text.ptr != NULL) {
			/*
			 * Point PLAIN at the encrypted fragment and
			 * then decrypt in-place.  After the
			 * decryption, PLAIN will have been adjusted
			 * to just point at the data.
			 *
			 * For moment log the individual fragments
			 * that are invalid (too verbose VS helping
			 * responder figure out where things go
			 * wrong).
			 */
			if (!verify_and_decrypt_v2_message(ike, frag->text,
							   &frag->plain,
							   frag->iv_offset)) {
				llog_sa(RC_LOG, ike,
					"saved fragment %u of %u invalid; dropped",
					i, (*frags)->total);
				/* release the frag */
				(*frags)->count--;
				free_chunk_content(&frag->text);
				frag->text = empty_chunk;
				frag->plain = null_shunk;
				frag->iv_offset = 0;
			}
			ldbg(ike->sa.logger, "saved fragment %u of %u decrypted",
			     i, (*frags)->total);
		}
	}

	/* see what, if anything, is left */

	if ((*frags)->count == 0) {
		/*
		 * Without a valid fragment there's no way to trust
		 * .md and .total, start again from scratch.
		 */
		ldbg(ike->sa.logger, "all fragments were invalid, .total can NOT be trusted");
		free_v2_incoming_fragments(frags);
		return false;
	}

	if ((*frags)->count < (*frags)->total) {
		/* more to do */
		ldbg(ike->sa.logger, "some, but not all fragments were invalid, .total can be trusted");
		return false;
	}

	return true;
}

struct msg_digest *reassemble_v2_incoming_fragments(struct v2_incoming_fragments **frags,
						    struct logger *logger)
{
	ldbg(logger, "reassembling incoming fragments");

	/*
	 * MD is the first message fragment; it;s going to be
	 * scribbled on turning it into an unfragmented message.
	 */
	struct msg_digest *md = md_addref((*frags)->md);

	PASSERT(logger, md->chain[ISAKMP_NEXT_v2SK] == NULL);
	PASSERT(logger, md->digest_roof < elemsof(md->digest));
	struct payload_digest *old_skf = md->chain[ISAKMP_NEXT_v2SKF];
	PASSERT(logger, old_skf != NULL);
	PEXPECT(logger, old_skf->payload.v2skf.isaskf_number == 1);

	/*
	 * Determine the lengh of the re-assembled packet.
	 *
	 * Per IKE_INTERMEDIATE, it will contain the IKE Header,
	 * unencrypted payloads, SK payload header, and the
	 * reassembled fragments.
	 *
	 * It will not contain the IV (normally follows the SK payload
	 * header) and padding / checksum (normally at end of packet.
	 */
	size_t length = 0;

	/* IKE HEADER */
	chunk_t old_ike_header = chunk_slice(md->packet, 0, sizeof(struct isakmp_hdr));
	ldbg(logger, "old IKE header %zu bytes", old_ike_header.len);
	length += old_ike_header.len;

	/* unencrypted payloads */
	ldbg(logger, "skf start %p packet ptr %p header %zu",
	     old_skf->pbs.start, md->packet.ptr, sizeof(struct ikev2_skf));
	chunk_t old_unencrypted_payloads = chunk_slice(md->packet, old_ike_header.len,
						       old_skf->pbs.start - md->packet.ptr);
	ldbg(logger, "old unencrypted payloads %zu bytes", old_unencrypted_payloads.len);
	length += old_unencrypted_payloads.len;

	/* just the generic bits of SKF header */
	chunk_t old_skf_generic_header = chunk_slice(md->packet,
						     old_ike_header.len + old_unencrypted_payloads.len,
						     old_ike_header.len + old_unencrypted_payloads.len + sizeof(struct ikev2_generic));
	ldbg(logger, "old generic part of SKF header %zu bytes", old_skf_generic_header.len);
	length += old_skf_generic_header.len;

	/*
	 * Compute the space needed to re-assemble the fragments; add
	 * that to length.
	 */
	size_t encrypted_payloads_length = 0;
	for (unsigned i = 1; i <= (*frags)->total; i++) {
		struct v2_incoming_fragment *frag = &(*frags)->frags[i];
		PEXPECT(logger, frag->plain.ptr != NULL);
		encrypted_payloads_length += frag->plain.len;
	}
	ldbg(logger, "reassembled encrypted payloads length %zu", encrypted_payloads_length);
	length += encrypted_payloads_length;

	/*
	 * Allocate the packet
	 */
	PEXPECT(logger, md->v2_sk_packet.ptr == NULL); /* empty */
	md->v2_sk_packet = alloc_chunk(length, "IKEv2 fragments buffer");
	ldbg(logger, "reassembled total packet length %zu", md->v2_sk_packet.len);

	/*
	 * Copy over the packet start (IKE Header + unencrypted
	 * payloads + generic part of SKF payload header).
	 *
	 * The code that follows will need to patch up:
	 *
	 * - IKE Header's length field
	 * - if no unencrypted payloads,IKE Header's next payload field (SKF->SK)
	 * - if unencrypted payloads, last header's SKF->SK
	 * - the SK payload header's length
	 *
	 * Use a cursor.
	 */
	chunk_t packet_cursor = md->v2_sk_packet;
	hunk_put_hunk(&packet_cursor, &old_ike_header);
	hunk_put_hunk(&packet_cursor, &old_unencrypted_payloads);
	hunk_put_hunk(&packet_cursor, &old_skf_generic_header);

	/*
	 * Pass 2: Append the fragments to the re-constructed packet.
	 */
	shunk_t sk_body = HUNK_AS_SHUNK(&packet_cursor); /* the remainder */
	for (unsigned i = 1; i <= (*frags)->total; i++) {
		struct v2_incoming_fragment *frag = &(*frags)->frags[i];
		hunk_put_hunk(&packet_cursor, &frag->plain);
	}

	/* filled to the brim */
	PASSERT(logger, packet_cursor.len == 0);

	/*
	 * Fake up enough of an SK payload_digest to fool the caller
	 * and then scribble that all over the SKF (updating the SK
	 * and SKF .chain[] pointers).
	 */
	struct payload_digest sk = {
		.pbs = pbs_in_from_shunk(sk_body, "decrypted SFK payloads"),
		.payload_type = ISAKMP_NEXT_v2SK,
		.payload.generic.isag_np = (*frags)->first_np,
	};
	struct payload_digest *skf = md->chain[ISAKMP_NEXT_v2SKF];
	md->chain[ISAKMP_NEXT_v2SKF] = NULL;
	md->chain[ISAKMP_NEXT_v2SK] = skf;
	*skf = sk; /* scribble */

	/* save the fragment total; used later by duplicate code */
	md->v2_frags_total = (*frags)->total;

	/* clean up */
	free_v2_incoming_fragments(frags);
	return md;
}

/*
 * Decrypt the, possibly fragmented message intended for ST.
 *
 * Since the message fragments are stored in the recipient's ST
 * (either IKE or CHILD SA), it, and not the IKE SA is needed.
 *
 * The bytes to be decryted are roughly .cursor + sizeof(IV) - .roof.
 */
bool ikev2_decrypt_msg(struct ike_sa *ike, struct msg_digest *md)
{
	struct pbs_in *sk_pbs = &md->chain[ISAKMP_NEXT_v2SK]->pbs;
	/*
	 * If so impaired, clone the encrypted message before it gets
	 * decrypted in-place (but only once).
	 */
	if (impair.replay_encrypted && !md->fake_clone) {
		llog(RC_LOG, ike->sa.logger,
		     "IMPAIR: cloning incoming encrypted message and scheduling its replay");
		schedule_md_event("replay encrypted message",
				  clone_raw_md(md, HERE));
	}

	/*
	 * Having read the SK header, the .cursor is pointing at the
	 * IV.  Lets corrupt it!
	 */
	size_t iv_offset = sk_pbs->cur - md->packet.ptr;
	if (impair.corrupt_encrypted && !md->fake_clone) {
		llog(RC_LOG, ike->sa.logger,
		     "IMPAIR: corrupting incoming encrypted message's SK payload's first byte");
		md->packet.ptr[iv_offset] = ~(md->packet.ptr[iv_offset]);
	}

	chunk_t message = chunk2(md->packet.ptr, sk_pbs->roof - md->packet.ptr);
	shunk_t plain = null_shunk; /*to be sure*/
	bool ok = verify_and_decrypt_v2_message(ike, message, &plain, iv_offset);
	/*
	 * Update the SK pbs so that points at the decrypted payload
	 * (skipping IV et.al.) BUT preserve .start, keeping it
	 * pointing at SK's header.
	 */
	sk_pbs->cur = plain.ptr;
	sk_pbs->roof = plain.ptr + plain.len;

	name_buf xb;
	ldbg(ike->sa.logger, PRI_SO" ikev2 %s decrypt %s",
	     pri_so(ike->sa.st_serialno),
	     str_enum_long(&ikev2_exchange_names, md->hdr.isa_xchg, &xb),
	     ok ? "success" : "failed");

	return ok;
}

/*
 * IKEv2 fragments:
 *
 *                        1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | Next Payload  |C|  RESERVED   |         Payload Length        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |        Fragment Number        |        Total Fragments        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                     Initialization Vector                     |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   ~                      Encrypted content                        ~
 *   +               +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |               |             Padding (0-255 octets)            |
 *   +-+-+-+-+-+-+-+-+                               +-+-+-+-+-+-+-+-+
 *   |                                               |  Pad Length   |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   ~                    Integrity Checksum Data                    ~
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *

 *
 */

static bool encrypt_and_record_outbound_fragment(struct logger *logger,
						 struct ike_sa *ike,
						 const struct isakmp_hdr *hdr,
						 enum next_payload_types_ikev2 skf_np,
						 struct v2_outgoing_fragments *fragments,
						 shunk_t fragment,
						 unsigned int number)
{
	struct fragment_pbs_out message_fragment;
	if (!open_fragment_pbs_out("fragment", &message_fragment, logger)) {
		return false;
	}

	/* HDR out */

	struct pbs_out body;
	if (!pbs_out_struct(&message_fragment.pbs, (*hdr), &isakmp_hdr_desc, &body))
		return false;

	/*
	 * Fake up an SK payload description sufficient to fool the
	 * encryption code.
	 *
	 * While things are close, they are not identical - an SKF
	 * payload header has extra fields and, for the first
	 * fragment, forces the Next Payload.
	 */

	struct v2SK_payload skf = {
		.ike = ike,
		.logger = logger,
		.payload = {
		    .ptr = body.cur,
		    .len = 0 /* computed at end; set here to silence GCC 4.8.5 */
		}
	};

	/*
	 * emit SKF header, save location.
	 *
	 * In the first fragment, .NP is set to the SK payload's next
	 * payload type.
	 */

	const struct ikev2_skf e = {
		.isaskf_np = skf_np, /* needed */
		.isaskf_critical = build_ikev2_critical(false, ike->sa.logger),
		.isaskf_number = number,
		.isaskf_total = fragments->len,
	};
	if (!pbs_out_struct(&body, e, &ikev2_skf_desc, &skf.pbs))
		return false;

	/*
	 * Additional Authenticated Data - AAD - is everything so far:
	 * i.e., the IKE header and payload headers.
	 *
	 * RFC5282 says: The Initialization Vector and Ciphertext
	 * fields [...] MUST NOT be included in the associated data.
	 */

	skf.aad = shunk2(skf.pbs.container->start, skf.pbs.cur - skf.pbs.container->start);

	/* emit space for IV, and save location */

	if (!pad_v2SK_iv(&skf)) {
		/* already logged */
		return false;
	}

	/* save cleartext start */

	skf.cleartext.ptr = skf.pbs.cur;

	/* output the fragment */

	if (!pbs_out_hunk(&skf.pbs, fragment, "cleartext fragment"))
		return false;

	if (!close_v2SK_payload(&skf)) {
		return false;
	}

	close_pbs_out(&body);
	close_pbs_out(&message_fragment.pbs);

	if (!encrypt_v2SK_payload(&skf)) {
		llog(RC_LOG, logger, "error encrypting fragment %u", number);
		return false;
	}

	ldbg(ike->sa.logger, "recording fragment %u", number);
	PASSERT(ike->sa.logger, number > 0);
	fragments->item[number - 1] = clone_pbs_out_all(&message_fragment.pbs, "fragment");
	return true;
}

static bool encrypt_and_record_outbound_fragments(shunk_t message,
						  struct v2SK_payload *sk,
						  struct v2_outgoing_fragments **fragments,
						  const char *story)
{
	/*
	 * fragment contents:
	 * - sometimes:	NON_ESP_MARKER (RFC3948) (NON_ESP_MARKER_SIZE) (4)
	 * - always:	isakmp header (NSIZEOF_isakmp_hdr) (28)
	 * - always:	ikev2_skf header (NSIZEOF_ikev2_skf) (8)
	 * - variable:	IV (no IV is longer than SHA2_512_DIGEST_SIZE) (64 or less)
	 * - variable:	fragment's data
	 * - variable:	padding (no padding is longer than MAX_CBC_BLOCK_SIZE) (16 or less)
	 */

	/*
	 * XXX: this math seems very contrived, can the fragment()
	 * function above be left to do the computation on-the-fly?
	 *
	 * XXX: perhaps, the problem is that the SKF header needs to
	 * know the total number of fragments.
	 *
	 * Start with the max size of the entire packet (aka
	 * .ikev2_max_fragment_size).
	 */

	unsigned int max_skf_size = endpoint_info(sk->ike->sa.st_remote_endpoint)->ikev2_max_fragment_size;

	/*
	 * Reduce MAX by non-ESP marker bytes.  When doing NAT these
	 * are added to the start of the UDP packet.
	 */
	if (sk->ike->sa.st_iface_endpoint != NULL &&
	    sk->ike->sa.st_iface_endpoint->esp_encapsulation_enabled) {
		max_skf_size -= NON_ESP_MARKER_SIZE;
	}

	/*
	 * Reduce MAX of the always present message and the SKF
	 * headers
	 */
	max_skf_size -= NSIZEOF_isakmp_hdr + NSIZEOF_ikev2_skf;

	/*
	 * Reduce MAX by the always present integrity/tag bytes.
	 */
	max_skf_size -= (encrypt_desc_is_aead(sk->ike->sa.st_oakley.ta_encrypt)
			 ? sk->ike->sa.st_oakley.ta_encrypt->aead_tag_size
			 : sk->ike->sa.st_oakley.ta_integ->integ_output_size);

	if (sk->ike->sa.st_oakley.ta_encrypt->pad_to_blocksize) {
		max_skf_size &= ~(sk->ike->sa.st_oakley.ta_encrypt->enc_blocksize - 1);
	}

	max_skf_size -= 2;	/* ??? what's this? */

	PASSERT(sk->logger, sk->cleartext.len != 0);

	/* required number of fragments; rounded */
	unsigned int nfrags = (sk->cleartext.len + max_skf_size - 1) / max_skf_size;

	if (nfrags > MAX_IKE_FRAGMENTS) {
		llog(RC_LOG, sk->logger,
			    "fragmenting this %zu byte message into %u byte chunks leads to too many frags",
			    sk->cleartext.len, max_skf_size);
		return false;
	}

	realloc_v2_outgoing_fragments(fragments, sk->logger, nfrags, story);

	/*
	 * Extract the hdr from the original unfragmented message.
	 *
	 * Tweak it ready for use as the fragment's message header
	 * (clear Next Paylod).
	 */
	struct isakmp_hdr hdr;
	{
		struct pbs_in pbs = pbs_in_from_shunk(message, "sk hdr");
		struct pbs_in ignored;
		diag_t d = pbs_in_struct(&pbs, &isakmp_hdr_desc, &hdr, sizeof(hdr), &ignored);
		if (d != NULL) {
			llog(RC_LOG, sk->logger, "%s", str_diag(d));
			pfree_diag(&d);
			return false;
		}
	}
	hdr.isa_np = ISAKMP_NEXT_v2NONE; /* clear NP */

	/*
	 * Extract the value of the SK's next payload field from the
	 * original unfragmented message.  This is used as the first
	 * fragmented payload's SKF Next Payload field, the rest have
	 * Next Payload set to NONE(0).
	 *
	 * Note: the value isn't known until the first encrypted
	 * payload is emitted.  Hence the need to fetch it from the SK
	 * payload after the message has been constructed.
	 */
	enum next_payload_types_ikev2 skf_np = fixup_value(sk->logger, &sk->sk_next_payload_field);

	unsigned int number = 1;
	shunk_t clear_cursor = HUNK_AS_SHUNK(&sk->cleartext);

	do {
		/* chop off the next fragment, advancing the cursor */
		shunk_t fragment = shunk_slice(clear_cursor, 0, PMIN(clear_cursor.len, max_skf_size));
		clear_cursor = shunk_slice(clear_cursor, fragment.len, clear_cursor.len);

		if (!encrypt_and_record_outbound_fragment(sk->logger, sk->ike,
							  &hdr, skf_np,
							  (*fragments), fragment,
							  number)) {
			return false;
		}

		number++;
		skf_np = ISAKMP_NEXT_v2NONE;
	} while (clear_cursor.len > 0);

	return true;
}

/*
 * Encrypt and Record the message ready for sending.  If needed, first
 * fragment it.
 */

static stf_status encrypt_and_record_v2SK_message(shunk_t message,
						  struct v2SK_payload *sk,
						  struct v2_outgoing_fragments **outgoing_fragments,
						  const char *story)
{
	size_t len = message.len;

	/*
	 * If we are doing NAT, so that the other end doesn't mistake
	 * this message for ESP, each message needs a non-ESP_Marker
	 * prefix.
	 */
	if (!PEXPECT(sk->logger, sk->ike->sa.st_iface_endpoint != NULL) &&
	    sk->ike->sa.st_iface_endpoint->esp_encapsulation_enabled)
		len += NON_ESP_MARKER_SIZE;

	/* IPv4 and IPv6 have different fragment sizes */
	if (sk->ike->sa.st_iface_endpoint->io->protocol == &ip_protocol_udp &&
	    sk->ike->sa.st_v2_ike_fragmentation_enabled &&
	    len >= endpoint_info(sk->ike->sa.st_remote_endpoint)->ikev2_max_fragment_size) {
		if (!encrypt_and_record_outbound_fragments(message, sk, outgoing_fragments, story)) {
			ldbg(sk->logger, "record outbound fragments failed");
			return STF_INTERNAL_ERROR;
		}
		return STF_OK;
	}

	if (!encrypt_v2SK_payload(sk)) {
		llog(RC_LOG, sk->logger, "error encrypting %s message", story);
		return STF_INTERNAL_ERROR;
	}
	ldbg(sk->logger, "recording outgoing fragment failed");
	record_v2_outgoing_message(message, outgoing_fragments, sk->logger, story);
	return STF_OK;
}

struct ikev2_id build_v2_id_payload(const struct host_end *end, shunk_t *body,
				    const char *what, struct logger *logger)
{
	struct ikev2_id id_header = {
		.isai_type = id_to_payload(&end->id, &end->addr, body),
		.isai_critical = build_ikev2_critical(false, logger),
	};
	if (impair.send_nonzero_reserved_id) {
		llog(RC_LOG, logger, "IMPAIR: setting reserved byte 3 of %s to 0x%02x",
		     what, ISAKMP_PAYLOAD_FLAG_LIBRESWAN_BOGUS);
		id_header.isai_res3 = ISAKMP_PAYLOAD_FLAG_LIBRESWAN_BOGUS;
	}
	return id_header;
}

bool open_v2_message(const char *story,
		     struct ike_sa *ike, struct logger *logger,
		     struct msg_digest *request_md,
		     enum ikev2_exchange exchange_type,
		     uint8_t *buf, size_t sizeof_buf,
		     struct v2_message *message,
		     enum payload_security security)
{
	*message = (struct v2_message) {
		.story = story,
		.logger = logger,
		.security = security,
		.outgoing_fragments = (request_md == NULL ? &ike->sa.st_v2_msgid_windows.initiator.outgoing_fragments :
				       &ike->sa.st_v2_msgid_windows.responder.outgoing_fragments),
	};

	message->message = open_pbs_out(story, buf, sizeof_buf, logger);

	if (!open_v2_message_body(&message->message, ike, request_md,
				  exchange_type, &message->body)) {
		llog(RC_LOG, message->logger,
		     "error initializing hdr for encrypted notification");
		return false;
	}

	if (!emit_v2UNKNOWN("unencrypted", exchange_type,
			    &impair.add_unknown_v2_payload_to,
			    &message->body)) {
		return false;
	}

	switch (security) {
	case ENCRYPTED_PAYLOAD:
		/*
		 * Never encrypt the initial exchange (peer requires
		 * Nr in response before it can compute keys and
		 * decrypt it).
		 */
		if (exchange_type == ISAKMP_v2_IKE_SA_INIT ||
		    exchange_type == ISAKMP_v2_IKE_SESSION_RESUME) {
			name_buf nb;
			llog_pexpect(message->logger, HERE,
				     "exchange type %s is invalid for encrypted notification",
				     str_enum_short(&ikev2_exchange_names, exchange_type, &nb));
			return false;
		}
		/*
		 * Encrypting requires an IKE SA for the keys.
		 */
		if (PBAD(message->logger, ike == NULL)) {
			return false;
		}
		/*
		 * Encryption requires the IKE SA to have keys.
		 */
		if (!PEXPECT(message->logger, ike->sa.hidden_variables.st_skeyid_calculated)) {
			return false;
		}
		if (!open_body_v2SK_payload(&message->body, ike, logger, &message->sk)) {
			return false;
		}

		/*
		 * Save the Next Payload chain's fixup which currently
		 * points into the just emitted SK header's Next
		 * Payload field.
		 *
		 * Since there is a single chain running through the
		 * message, this is stored somewhere other then the
		 * SK.PBS (probably the outermost PBS).
		 */
		struct fixup *next_payload_chain = pbs_out_next_payload_chain(&message->sk.pbs);
		message->sk.sk_next_payload_field = *next_payload_chain;

		message->pbs = &message->sk.pbs;
		if (!emit_v2UNKNOWN("encrypted", exchange_type,
				    &impair.add_unknown_v2_payload_to_sk,
				    message->pbs)) {
			return false;
		}
		break;
	case UNENCRYPTED_PAYLOAD:
		/*
		 * Only send the initial exchange unencrypted (peer
		 * needs unencrypted response containing Nr before it
		 * can generate keys; all following exchanges should
		 * be encrypted).
		 *
		 * Note that .ST_SKEYID_CALCULATED may be set. With
		 * IKE_SESSION_RESUME, it is computed before building
		 * the response (for IKE_SA_INIT it is only computed
		 * upon the arrival of the IKE_AUTH message).
		 */
		if (exchange_type != ISAKMP_v2_IKE_SA_INIT &&
		    exchange_type != ISAKMP_v2_IKE_SESSION_RESUME) {
			name_buf nb;
			llog_pexpect(message->logger, HERE,
				     "exchange type %s is invalid for encrypted notification",
				     str_enum_short(&ikev2_exchange_names, exchange_type, &nb));
			return false;
		}
		message->pbs = &message->body;
		break;
	}

	return true;
}

bool close_v2_message(struct v2_message *message)
{
	if (impair.add_v2_notification.enabled) {
		if (!emit_v2N(impair.add_v2_notification.value, message->pbs)) {
			return false;
		}
	}

	switch (message->security) {
	case ENCRYPTED_PAYLOAD:
		if (!close_v2SK_payload(&message->sk)) {
			return false;
		}
		break;
	case UNENCRYPTED_PAYLOAD:
		break;
	}
	close_pbs_out(&message->body);
	close_pbs_out(&message->message);
	return true;
}

bool record_v2_message(struct v2_message *message)
{
	switch (message->security) {
	case ENCRYPTED_PAYLOAD:
		if (encrypt_and_record_v2SK_message(pbs_out_all(&message->message),
						    &message->sk,
						    message->outgoing_fragments,
						    message->story) != STF_OK) {
			return false;
		}
		return true;
	case UNENCRYPTED_PAYLOAD:
		record_v2_outgoing_message(pbs_out_all(&message->message),
					   message->outgoing_fragments,
					   message->logger,
					   message->story);
		return true;
	}
	bad_case(message->security);
}

bool close_and_record_v2_message(struct v2_message *message)
{
	if (!close_v2_message(message)) {
		return false;
	}

	if (!record_v2_message(message)) {
		return false;
	}

	return true;
}
