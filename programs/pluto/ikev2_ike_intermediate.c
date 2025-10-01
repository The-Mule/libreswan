/* IKEv2 IKE_INTERMEDIATE exchange, for Libreswan
 *
 * Copyright (C) 2020  Yulia Kuzovkova <ukuzovkova@gmail.com>
 * Copyright (C) 2021  Andrew Cagney
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

#include "ike_alg_kem.h"		/* for ike_alg_kem_none; */
#include "state.h"
#include "demux.h"
#include "keys.h"
#include "crypt_dh.h"
#include "ikev2.h"
#include "ikev2_send.h"
#include "ikev2_message.h"
#include "ikev2_ppk.h"
#include "ikev2_ike_intermediate.h"
#include "crypt_symkey.h"
#include "log.h"
#include "connections.h"
#include "unpack.h"
#include "ikev2_nat.h"
#include "ikev2_ike_auth.h"
#include "pluto_stats.h"
#include "crypt_prf.h"
#include "ikev2_states.h"
#include "ikev2_eap.h"
#include "secrets.h"
#include "crypt_cipher.h"
#include "ikev2_prf.h"
#include "ikev2_notification.h"
#include "crypt_kem.h"
#include "ikev2_parent.h"		/* for emit_v2KE() */
#include "ikev2_ke.h"
#include "ikev2_helper.h"

static ikev2_state_transition_fn process_v2_IKE_INTERMEDIATE_request;	/* type assertion */
void extract_v2_ike_intermediate_keys(struct ike_sa *ike, PK11SymKey *keymat);

static ikev2_helper_fn initiate_v2_IKE_INTERMEDIATE_request_helper;
static ikev2_helper_fn process_v2_IKE_INTERMEDIATE_request_helper;
static ikev2_helper_fn process_v2_IKE_INTERMEDIATE_response_helper;

static ikev2_resume_fn initiate_v2_IKE_INTERMEDIATE_request_continue;
static ikev2_resume_fn process_v2_IKE_INTERMEDIATE_request_continue;
static ikev2_resume_fn process_v2_IKE_INTERMEDIATE_response_continue;

static ikev2_cleanup_fn cleanup_IKE_INTERMEDIATE_task;

struct ikev2_task {
	struct ikev2_ike_intermediate_exchange exchange;
	/* for ADDKE */
	struct kem_initiator *initiator;
	struct kem_responder *responder;
	/* for SKEYSEED */
	chunk_t ni;
	chunk_t nr;
	PK11SymKey *d;
	/* for KEYMAT */
	ike_spis_t ike_spis;
	size_t nr_keymat_bytes;
	PK11SymKey *keymat;
	const struct prf_desc *prf;
};

void cleanup_IKE_INTERMEDIATE_task(struct ikev2_task **task, struct logger *logger)
{
	pfree_kem_initiator(&(*task)->initiator, logger);
	pfree_kem_responder(&(*task)->responder, logger);
	free_chunk_content(&(*task)->ni);
	free_chunk_content(&(*task)->nr);
	symkey_delref(logger, "d", &(*task)->d);
	symkey_delref(logger, "skeyseed", &(*task)->keymat);
	pfreeany(*task);
}

/*
 * Without this the code makes little sense.
 *
 * https://www.rfc-editor.org/rfc/rfc9242.html#figure-1
 *
 *                       1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ^ ^ <- MESSAGE
 *  |                       IKE SA Initiator's SPI                  | | |    HEADER
 *  |                                                               | | |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ I |
 *  |                       IKE SA Responder's SPI                  | K |
 *  |                                                               | E |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+   |
 *  |  Next Payload | MjVer | MnVer | Exchange Type |     Flags     | H |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ d |
 *  |                          Message ID                           | r A
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ | |
 *  |                       Adjusted Length                         | | |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ v | <- UNENCRYPTED
 *  |                                                               |   |    PAYLOADS
 *  ~                 Unencrypted payloads (if any)                 ~   |
 *  |                                                               |   |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ^ | <- ENCRYPTED
 *  | Next Payload  |C|  RESERVED   |    Adjusted Payload Length    | | |    PAYLOAD
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ | v    HEADER
 *  |                                                               | |
 *  ~                     Initialization Vector                     ~ E
 *  |                                                               | E
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ c ^ <- INNER
 *  |                                                               | r |    PAYLOADS
 *  ~             Inner payloads (not yet encrypted)                ~   P
 *  |                                                               | P |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ l v
 *  |              Padding (0-255 octets)           |  Pad Length   | d
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ |
 *  |                                                               | |
 *  ~                    Integrity Checksum Data                    ~ |
 *  |                                                               | |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ v
 *
 *      Figure 1: Data to Authenticate in the IKE_INTERMEDIATE Exchange
 *                                Messages
 *
 *  Figure 1 illustrates the layout of the IntAuth_[i/r]*A (denoted
 *  as A) and the IntAuth_[i/r]*P (denoted as P) chunks in case the
 *  Encrypted payload is not empty.
 */

static void compute_intermediate_mac(struct ike_sa *ike,
				     PK11SymKey *intermediate_key,
				     shunk_t message_header,
				     shunk_t unencrypted_payloads,
				     shunk_t encrypted_payload_header,
				     shunk_t inner_payloads,
				     chunk_t *int_auth_ir)
{
	struct logger *logger = ike->sa.logger;

	/*
	 * compute the PRF over "A" + "P" as in:
	 *
	 * IntAuth_i1 = prf(SK_pi1,              IntAuth_i1A [| IntAuth_i1P])
	 * IntAuth_i2 = prf(SK_pi2, IntAuth_i1 | IntAuth_i2A [| IntAuth_i2P])
	 *
	 * IntAuth_r1 = prf(SK_pr1,              IntAuth_r1A [| IntAuth_r1P])
	 * IntAuth_r2 = prf(SK_pr2, IntAuth_r1 | IntAuth_r2A [| IntAuth_r2P])
	 */

	/*
	 * IntAuth_[ir](N) = prf(SK_p[ir](N), ...)
	 */

	struct crypt_prf *prf = crypt_prf_init_symkey("IKE INTERMEDIATE",
						      ike->sa.st_oakley.ta_prf,
						      "SK_p", intermediate_key,
						      ike->sa.logger);

	/*
	 * IntAuth_[ir](N) = prf(..., IntAuth_[ir](N-1) | ...)
	 */

	if (int_auth_ir->len > 0) {
		crypt_prf_update_hunk(prf, "IntAuth_[ir](N-1)", *int_auth_ir);
	}

	/*
	 * IntAuth_[ir](N) = prf(... | IntAuth_[ir](N)A | ...)
	 *
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ^ ^ <- MESSAGE
	 *  |                       IKE SA Initiator's SPI                  | | |    HEADER
	 *  |                                                               | | |
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ I |
	 *  |                       IKE SA Responder's SPI                  | K |
	 *  |                                                               | E |
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+   |
	 *  |  Next Payload | MjVer | MnVer | Exchange Type |     Flags     | H |
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ d |
	 *  |                          Message ID                           | r A
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ | |
	 *  |                       Adjusted Length                         | | |
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ v | <- UNENCRYPTED
	 *  |                                                               |   |    PAYLOADS
	 *  ~                 Unencrypted payloads (if any)                 ~   |
	 *  |                                                               |   |
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ^ | <- ENCRYPTED
	 *  | Next Payload  |C|  RESERVED   |    Adjusted Payload Length    | | |    PAYLOAD
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ | v    HEADER
	 *
	 * The IntAuth_[i/r]*A chunk consists of the sequence of
	 * octets from the first octet of the IKE Header (not
	 * including the prepended four octets of zeros, if UDP
	 * encapsulation or TCP encapsulation of ESP packets is used)
	 * to the last octet of the generic header of the Encrypted
	 * payload.  The scope of IntAuth_[i/r]*A is identical to the
	 * scope of Associated Data defined for the use of AEAD
	 * algorithms in IKEv2 (see Section 5.1 of [RFC5282]), which
	 * is stressed by using the "A" suffix in its name.  Note that
	 * calculation of IntAuth_[i/r]*A doesn't depend on whether an
	 * AEAD algorithm or a plain cipher is used in IKE SA.
	 *
	 * For the purpose of prf calculation, the Length field in the
	 * IKE Header and the Payload Length field in the Encrypted
	 * payload header are adjusted so that they don't count the
	 * lengths of Initialization Vector, Integrity Checksum Data,
	 * Padding, and Pad Length fields. In other words, the Length
	 * field in the IKE Header (denoted as Adjusted Length in
	 * Figure 1) is set to the sum of the lengths of
	 * IntAuth_[i/r]*A and IntAuth_[i/r]*P, and the Payload Length
	 * field in the Encrypted payload header (denoted as Adjusted
	 * Payload Length in Figure 1) is set to the length of
	 * IntAuth_[i/r]*P plus the size of the Encrypted payload
	 * header (four octets).
	 *
	 * Define variables that match the naming scheme used by the
	 * RFC's ASCII diagram above.
	 */

	/*
	 * The message header needs its Length adjusted.
	 *
	 * What isn't mentioned is that, when things are fragmented,
	 * the Next Payload field also needs to be changed from SKF to
	 * SK (but only when there's no unencrypted payloads).
	 *
	 * When there's unencrypted payloads, its the last of those
	 * that needs adjusting, and pluto doesn't do that (caller
	 * will have rejected it).
	 */
	size_t adjusted_payload_length = (message_header.len
					  + unencrypted_payloads.len
					  + encrypted_payload_header.len
					  + inner_payloads.len);
	ldbg(logger, "adjusted payload length: %zu", adjusted_payload_length);
	struct isakmp_hdr adjusted_message_header;
	memcpy(&adjusted_message_header, message_header.ptr, message_header.len);
	hton_thing(adjusted_payload_length, adjusted_message_header.isa_length);
	if (adjusted_message_header.isa_np == ISAKMP_NEXT_v2SKF) {
		ldbg(logger, "adjusted fragmented Next Payload to SK");
		adjusted_message_header.isa_np = ISAKMP_NEXT_v2SK;
	}
	crypt_prf_update_thing(prf, "IntAuth_[ir](N)A: adjusted message header",
			       adjusted_message_header);

	/*
	 * Unencrypted payload (hopefully empty)
	 *
	 * When there's fragmentation, this needs to be adjusted so
	 * that the last unencrypted payload's Next Payload field is
	 * SK, and not SKF.  Not happening.
	 */
	crypt_prf_update_hunk(prf, "IntAuth_[ir](N)A: unencrypted payloads",
			      unencrypted_payloads);

	/*
	 * Encrypted payload header needs its Length adjusted.
	 */
	size_t adjusted_encrypted_payload_length = encrypted_payload_header.len + inner_payloads.len;
	ldbg(logger, "adjusted encrypted payload length: %zu", adjusted_encrypted_payload_length);
	struct ikev2_generic adjusted_encrypted_payload_header;
	memcpy(&adjusted_encrypted_payload_header, encrypted_payload_header.ptr, encrypted_payload_header.len);
	hton_thing(adjusted_encrypted_payload_length, adjusted_encrypted_payload_header.isag_length);
	crypt_prf_update_thing(prf, "IntAuth_[ir](N)A: adjusted encrypted (SK) header",
			       adjusted_encrypted_payload_header);

	/*
	 * IntAuth_[ir](N) = prf(... | IntAuth_[ir](N)P)
	 *
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ c ^ <- INNER
	 *  |                                                               | r |    PAYLOADS
	 *  ~             Inner payloads (not yet encrypted)                ~   P
	 *  |                                                               | P |
	 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ l v
	 *
	 * The IntAuth_[i/r]*P chunk is present if the Encrypted
	 * payload is not empty. It consists of the content of the
	 * Encrypted payload that is fully formed but not yet
	 * encrypted.  The Initialization Vector, Padding, Pad Length,
	 * and Integrity Checksum Data fields (see Section 3.14 of
	 * [RFC7296]) are not included into the calculation.  In other
	 * words, the IntAuth_[i/r]*P chunk is the inner payloads of
	 * the Encrypted payload in plaintext form, which is stressed
	 * by using the "P" suffix in its name.
	 */

	crypt_prf_update_hunk(prf, "IntAuth_[ir](N)P", inner_payloads);

	/* extract the mac; replace existing value */
	struct crypt_mac mac = crypt_prf_final_mac(&prf, NULL/*no-truncation*/);
	free_chunk_content(int_auth_ir);
	*int_auth_ir = clone_hunk_as_chunk(mac, "IntAuth");
}

static void compute_intermediate_outbound_mac(struct ike_sa *ike,
					      PK11SymKey *intermediate_key,
					      struct v2_message *message,
					      chunk_t *intermediate_auth_ir)
{
	shunk_t message_header = {
		.ptr = message->sk.pbs.container->start,
		.len = sizeof(struct isakmp_hdr),
	};
	shunk_t unencrypted_payloads = {
		.ptr = (message_header.ptr + message_header.len),
		.len = ((const uint8_t *)message->sk.header.ptr
			- (const uint8_t *)message_header.ptr
			- message_header.len),
	};
	shunk_t encrypted_payload_header = {
		.ptr = unencrypted_payloads.ptr + unencrypted_payloads.len,
		.len = sizeof(struct ikev2_generic)/*encrypted header*/,
	};
	shunk_t inner_payloads = HUNK_AS_SHUNK(&message->sk.cleartext);
	compute_intermediate_mac(ike, intermediate_key,
				 message_header,
				 unencrypted_payloads,
				 encrypted_payload_header,
				 inner_payloads,
				 intermediate_auth_ir);
}

static bool compute_intermediate_inbound_mac(struct ike_sa *ike,
					     PK11SymKey *intermediate_key,
					     struct msg_digest *md,
					     chunk_t *intermediate_auth_ir)
{
	const struct payload_digest *sk = md->chain[ISAKMP_NEXT_v2SK];

	shunk_t message_header = {
		.ptr = md->packet_pbs.start,
		.len = sizeof(struct isakmp_hdr),
	};
	const uint8_t *message_header_end = (message_header.ptr + message_header.len);
	/*
	 * Several problems that stem from how pluto re-constructs the
	 * fragments into an unfragmented payload:
	 *
	 *
	 */
	shunk_t unencrypted_payloads;
	if (md->hdr.isa_np == ISAKMP_NEXT_v2SK ||
	    md->hdr.isa_np == ISAKMP_NEXT_v2SKF) {
		/*
		 * It's empty.
		 *
		 * For SKF, the hash code will need to change Next
		 * Payload back to SK.
		 */
		unencrypted_payloads = (shunk_t) {
			.ptr = message_header_end,
			.len = 0,
		};
	} else if (md->v2_frags_total == 0) {
		/*
		 * DANGER:
		 *
		 * When re-constructing a fragmented message, Pluto
		 * stores the accumulated fragments in a separate
		 * buffer (md->raw_packet) and then points sk->pbs at
		 * it.  This means that, for a fragmented payload,
		 * sk->pbs.start DOES NOT point into md->packet_pbs.
		 * Hence, the calculation:
		 *
		 *    sk->pbs.start - md->packet_pbs.start
		 *
		 * can only be used when the message is unfragmented.
		 *
		 * sk->pbs.start points at the contents of the SK
		 * packet, not the header, hence the math to back up.
		 */
		unencrypted_payloads = (shunk_t) {
			.ptr = message_header_end,
			.len = ((sk->pbs.start
				 - ike->sa.st_oakley.ta_encrypt->wire_iv_size
				 - sizeof(struct ikev2_generic))
				- message_header_end),
		};
	} else {
		/*
		 * NOTE:
		 *
		 * When computing the HASH of a fragmented message, in
		 * addition to adjusting the header's Length field,
		 * the headers Next Payload needs to be changed from
		 * SKF to SK.
		 *
		 * However, when the fragmented message also contains
		 * unencrypted payloads, its the Next Payload field of
		 * the last unencrypted payload that contains SKF and
		 * needs to be changed.
		 *
		 * Fortunatly, fragmented messages with unencrypted
		 * payloads aren't really a thing.  So not handling
		 * them is mostly hardmless.
		 */
		llog(WARNING_STREAM, ike->sa.logger,
		     "fragmented IKE_INTERMEDIATE messages with unencrypted payloads is not supported");
		return false;
	}
	/*
	 * Note.  For fragmented packets, this is the encrypted header
	 * from the first fragment.
	 */
	shunk_t encrypted_payload_header = {
		.ptr = unencrypted_payloads.ptr + unencrypted_payloads.len,
		.len = sizeof(struct ikev2_generic)/*encrypted header*/,
	};
	shunk_t inner_payloads = pbs_in_all(&sk->pbs);
	compute_intermediate_mac(ike, intermediate_key,
				 message_header,
				 unencrypted_payloads,
				 encrypted_payload_header,
				 inner_payloads,
				 intermediate_auth_ir);
	return true;
}

/*
 * Return the IKE_INTERMEDIATE exchange being worked on.
 */

bool next_is_ikev2_ike_intermediate_exchange(struct ike_sa *ike)
{
	if (!ike->sa.st_v2_ike_intermediate.enabled) {
		ldbg(ike->sa.logger, "IKE_INTERMEDIATE? no; not negotiated");
		return false;
	}

	unsigned next_exchange = ike->sa.st_v2_ike_intermediate.next_exchange;
	unsigned nr_exchanges = ike->sa.st_oakley.ta_addke.len;
	if (nr_exchanges == 0 && ike->sa.st_v2_ike_ppk == PPK_IKE_INTERMEDIATE) {
		nr_exchanges++;
	}

	return (next_exchange < nr_exchanges);

}

bool next_ikev2_ike_intermediate_exchange(struct ike_sa *ike)
{
	if (!PEXPECT(ike->sa.logger, next_is_ikev2_ike_intermediate_exchange(ike))) {
		return false;
	}
	ike->sa.st_v2_ike_intermediate.next_exchange++;
	return true;
}

struct ikev2_ike_intermediate_exchange current_ikev2_ike_intermediate_exchange(struct ike_sa *ike)
{
	struct ikev2_ike_intermediate_exchange exchange = {0};

	unsigned next_exchange = ike->sa.st_v2_ike_intermediate.next_exchange;
	if (PBAD(ike->sa.logger, next_exchange == 0)) {
		return exchange;
	}

	unsigned current_exchange = next_exchange - 1;
	if (current_exchange < ike->sa.st_oakley.ta_addke.len) {
		exchange.addke.type = ike->sa.st_oakley.ta_addke.list[current_exchange].type;
		exchange.addke.kem = ike->sa.st_oakley.ta_addke.list[current_exchange].kem;
		/* NONE is allowed, not NULL?!? */
		PASSERT(ike->sa.logger, exchange.addke.kem != NULL);
	}

	if ((ike->sa.st_oakley.ta_addke.len == 0 /*none*/||
	     current_exchange + 1 == ike->sa.st_oakley.ta_addke.len /*last*/) &&
	    ike->sa.st_v2_ike_ppk == PPK_IKE_INTERMEDIATE) {
		exchange.ppk = true;
	}

	name_buf tn;
	ldbg(ike->sa.logger, "IKE_INTERMEDIATE index %d len %d; %s=%s, ppk=%s",
	     current_exchange, ike->sa.st_oakley.ta_addke.len,
	     (exchange.addke.type == 0 ? "addke" :
	      str_enum_short(&ikev2_trans_type_names, exchange.addke.type, &tn)),
	     (exchange.addke.kem == NULL ? "no" : exchange.addke.kem->common.fqn),
	     bool_str(exchange.ppk));

	return exchange;
}

static bool extract_ike_intermediate_v2KE(const struct kem_desc *kem,
					  struct msg_digest *md,
					  shunk_t *ke,
					  struct logger *logger)
{
	const struct payload_digest *v2ke = md->chain[ISAKMP_NEXT_v2KE];

	if (v2ke == NULL) {
		name_buf rb;
		llog(RC_LOG, logger,
		     "%s KE missing from IKE_INTERMEDIATE %s",
		     kem->common.fqn,
		     str_enum_short(&message_role_names, v2_msg_role(md), &rb));
		return false;
	}

	if (v2ke->payload.v2ke.isak_kem != kem->ikev2_alg_id) {
		name_buf rb;
		name_buf gb;
		llog(RC_LOG, logger,
		     "expecting KE for %s in IKE_INTERMEDIATE %s received KE for %s",
		     kem->common.fqn,
		     str_enum_short(&message_role_names, v2_msg_role(md), &rb),
		     str_enum_short(&ikev2_trans_type_kem_names, v2ke->payload.v2ke.isak_kem, &gb));
		return false;
	}

	*ke = pbs_in_left(&v2ke->pbs);

	size_t ke_bytes = (v2_msg_role(md) == MESSAGE_REQUEST ? kem->initiator_bytes :
			   kem->responder_bytes);
	if (ke->len != ke_bytes) {
		name_buf rb;
		llog(RC_LOG, logger,
		     "%s KE in IKE_INTERMEDIATE %s is %zu bytes long but should be %zu bytes",
		     kem->common.fqn,
		     str_enum_short(&message_role_names, v2_msg_role(md), &rb),
		     ke->len, ke_bytes);
		return false;
	}

	return true;
}

static stf_status initiate_v2_IKE_INTERMEDIATE_request(struct ike_sa *ike,
						       struct child_sa *null_child,
						       struct msg_digest *null_md)
{
	PEXPECT(ike->sa.logger, null_child == NULL);
	PEXPECT(ike->sa.logger, null_md == NULL);
	PEXPECT(ike->sa.logger, ike->sa.st_sa_role == SA_INITIATOR);

	ldbg(ike->sa.logger, "%s() for "PRI_SO" %s: g^{xy} calculated, sending INTERMEDIATE",
	     __func__, pri_so(ike->sa.st_serialno), ike->sa.st_state->name);

	/* advance to the next ike intermediate exchange */
	if (!next_ikev2_ike_intermediate_exchange(ike)) {
		return STF_INTERNAL_ERROR;
	}

	struct ikev2_task task = {
		.exchange = current_ikev2_ike_intermediate_exchange(ike),
	};

	submit_ikev2_task(ike, null_md,
			  clone_thing(task, "initiator task"),
			  initiate_v2_IKE_INTERMEDIATE_request_helper,
			  initiate_v2_IKE_INTERMEDIATE_request_continue,
			  cleanup_IKE_INTERMEDIATE_task,
			  HERE);

	return STF_SUSPEND;
}

stf_status initiate_v2_IKE_INTERMEDIATE_request_helper(struct ikev2_task *task,
						       struct msg_digest *null_md,
						       struct logger *logger)
{
	PEXPECT(logger, null_md == NULL);

	if (task->exchange.addke.kem != NULL &&
	    task->exchange.addke.kem != &ike_alg_kem_none) {
		diag_t d = kem_initiator_key_gen(task->exchange.addke.kem,
						 &task->initiator, logger);
		if (d != NULL) {
			llog(RC_LOG, logger, "IKE_INTERMEDIATE key generation failed: %s", str_diag(d));
			pfree_diag(&d);
			return STF_FATAL;
		}
		if (LDBGP(DBG_BASE, logger)) {
			shunk_t ke = kem_initiator_ke(task->initiator);
			LDBG_log(logger, "initiator ADDKE:");
			LDBG_hunk(logger, ke);
		}
	}

	return STF_OK;
}

stf_status initiate_v2_IKE_INTERMEDIATE_request_continue(struct ike_sa *ike,
							 struct msg_digest *null_md,
							 struct ikev2_task *task)
{
	PEXPECT(ike->sa.logger, null_md == NULL);

	/* beginning of data going out */

	struct v2_message request;
	if (!open_v2_message("intermediate exchange request",
			     ike, ike->sa.logger,
			     NULL/*request*/, ISAKMP_v2_IKE_INTERMEDIATE,
			     reply_buffer, sizeof(reply_buffer), &request,
			     ENCRYPTED_PAYLOAD)) {
		return STF_INTERNAL_ERROR;
	}

	if (task->initiator != NULL) {
		if (!emit_v2KE(kem_initiator_ke(task->initiator),
			       task->exchange.addke.kem,
			       request.pbs)) {
			return STF_INTERNAL_ERROR;
		}
	}

	if (task->exchange.ppk) {
		struct connection *const c = ike->sa.st_connection;
		struct shunks *ppk_ids_shunks = c->config->ppk_ids_shunks;
		bool found_one = false;

		if (ppk_ids_shunks == NULL) {
			/* find any matching PPK and PPK_ID */
			const struct secret_ppk_stuff *ppk =
				get_connection_ppk_stuff(c);
			if (ppk != NULL) {
				found_one = true;
				if (!emit_v2N_PPK_IDENTITY_KEY(request.pbs, ike, ppk)) {
					return STF_INTERNAL_ERROR;
				}
			}
		} else {
			ITEMS_FOR_EACH(ppk_id, ppk_ids_shunks) {
				const struct secret_ppk_stuff *ppk =
					get_ppk_stuff_by_id(*ppk_id, ike->sa.logger);
				if (ppk != NULL) {
					found_one = true;
					if (!emit_v2N_PPK_IDENTITY_KEY(request.pbs, ike, ppk)) {
						return STF_INTERNAL_ERROR;
					}
				}
			}
		}

		if (!found_one) {
			if (c->config->ppk.insist) {
				llog_sa(RC_LOG, ike,
					"connection requires PPK, but we didn't find one");
				return STF_FATAL;
			} else {
				llog_sa(RC_LOG, ike,
					"failed to find PPK and PPK_ID, continuing without PPK");
			}
		}
	}

	if (!close_v2_message(&request)) {
		return STF_INTERNAL_ERROR;
	}

	/*
	 * For Intermediate Exchange, apply PRF to the peer's messages
	 * and store in state for further authentication.
	 */

	compute_intermediate_outbound_mac(ike, ike->sa.st_skey_pi_nss, &request,
					  &ike->sa.st_v2_ike_intermediate.initiator);

	if (!record_v2_message(&request)) {
		return STF_INTERNAL_ERROR;
	}

	/* save initiator for response processor */
	ike->sa.st_kem.initiator = task->initiator;
	task->initiator = NULL;

	return STF_OK;
}

static bool recalc_v2_ike_intermediate_ppk_keymat(struct ike_sa *ike,
						  const struct secret_ppk_stuff *ppk,
						  where_t where)
{
	struct logger *logger = ike->sa.logger;
	const struct prf_desc *prf = ike->sa.st_oakley.ta_prf;

	ldbg(logger, "%s() calculating skeyseed using prf %s",
	     __func__, prf->common.fqn);

	/*
	 * We need old_skey_d to recalculate SKEYSEED'.
	 */

	PK11SymKey *skeyseed =
		ikev2_IKE_INTERMEDIATE_ppk_skeyseed(prf, ppk->key,
						    /*old*/ike->sa.st_skey_d_nss,
						    logger);
	if (skeyseed == NULL) {
		llog_pexpect(logger, where, "ppk SKEYSEED failed");
		return false;
	}

	size_t nr_keymat_bytes = nr_ikev2_ike_keymat_bytes(&ike->sa);
	PK11SymKey *keymat = ikev2_ike_sa_keymat(prf, skeyseed,
						 ike->sa.st_ni, ike->sa.st_nr,
						 &ike->sa.st_ike_spis,
						 nr_keymat_bytes,
						 logger);

	extract_v2_ike_intermediate_keys(ike, keymat);

	symkey_delref(logger, "skeyseed", &keymat);
	symkey_delref(logger, "skeyseed", &skeyseed);

	LLOG_JAMBUF(RC_LOG, ike->sa.logger, buf) {
		jam_string(buf, "PPK '");
		jam_sanitized_hunk(buf, ppk->id);
		jam_string(buf, "' used in IKE_INTERMEDIATE by ");
		jam_enum_human(buf, &sa_role_names, ike->sa.st_sa_role);
	}
	return true;
}

void extract_v2_ike_intermediate_keys(struct ike_sa *ike, PK11SymKey *keymat)
{
	struct logger *logger = ike->sa.logger;

	/* release old keys, salts and cipher contexts */

	symkey_delref(logger, "SK_d", &ike->sa.st_skey_d_nss);
	symkey_delref(logger, "SK_ai", &ike->sa.st_skey_ai_nss);
	symkey_delref(logger, "SK_ar", &ike->sa.st_skey_ar_nss);
	symkey_delref(logger, "SK_ei", &ike->sa.st_skey_ei_nss);
	symkey_delref(logger, "SK_er", &ike->sa.st_skey_er_nss);
	symkey_delref(logger, "SK_pi", &ike->sa.st_skey_pi_nss);
	symkey_delref(logger, "SK_pr", &ike->sa.st_skey_pr_nss);
	free_chunk_content(&ike->sa.st_skey_initiator_salt);
	free_chunk_content(&ike->sa.st_skey_responder_salt);
	cipher_context_destroy(&ike->sa.st_ike_encrypt_cipher_context, logger);
	cipher_context_destroy(&ike->sa.st_ike_decrypt_cipher_context, logger);

	/* now we have to generate the keys for everything */

	extract_ikev2_ike_keys(&ike->sa, keymat);
}

stf_status process_v2_IKE_INTERMEDIATE_request(struct ike_sa *ike,
					       struct child_sa *null_child,
					       struct msg_digest *md)
{
	struct logger *logger = ike->sa.logger;
	PEXPECT(logger, null_child == NULL);

	if (!PEXPECT(logger, next_ikev2_ike_intermediate_exchange(ike))) {
		return STF_INTERNAL_ERROR;
	}

	natify_ikev2_ike_responder_endpoints(ike, md);

	/*
	 * All systems are go.
	 *
	 * Since DH succeeded, a secure (but unauthenticated) SA
	 * (channel) is available.  From this point on, should things
	 * go south, the state needs to be abandoned (but it shouldn't
	 * happen).
	 */

	/* save the most recent ID */
	ike->sa.st_v2_ike_intermediate.id = md->hdr.isa_msgid;

	/*
	 * Now that the payload has been decrypted, perform the
	 * intermediate exchange calculation.
	 *
	 * For Intermediate Exchange, apply PRF to the peer's messages
	 * and store in state for further authentication.
	 *
	 * Hence, here the responder uses the initiator's keys.
	 */
	if (!compute_intermediate_inbound_mac(ike, ike->sa.st_skey_pi_nss, md,
					      &ike->sa.st_v2_ike_intermediate.initiator)) {
		/* already logged; send back something */
		record_v2N_response(ike->sa.logger, ike, md,
				    v2N_INVALID_SYNTAX, empty_shunk,
				    ENCRYPTED_PAYLOAD);
		return STF_FATAL;
	}

	struct ikev2_task task = {
		.exchange = current_ikev2_ike_intermediate_exchange(ike),
		/* for SKEYSEED */
		.ni = clone_hunk_as_chunk(ike->sa.st_ni, "Ni"),
		.nr = clone_hunk_as_chunk(ike->sa.st_nr, "Nr"),
		.d = symkey_addref(logger, "d", ike->sa.st_skey_d_nss),
		.prf = ike->sa.st_oakley.ta_prf,
		/* for KEYMAT */
		.nr_keymat_bytes = nr_ikev2_ike_keymat_bytes(&ike->sa),
		.ike_spis = ike->sa.st_ike_spis,
	};

	submit_ikev2_task(ike, md,
			  clone_thing(task, "initiator task"),
			  process_v2_IKE_INTERMEDIATE_request_helper,
			  process_v2_IKE_INTERMEDIATE_request_continue,
			  cleanup_IKE_INTERMEDIATE_task,
			  HERE);

	return STF_SUSPEND;
}

stf_status process_v2_IKE_INTERMEDIATE_request_helper(struct ikev2_task *task,
						      struct msg_digest *md,
						      struct logger *logger)
{
	if (task->exchange.addke.kem != NULL &&
	    task->exchange.addke.kem != &ike_alg_kem_none) {
		shunk_t initiator_ke;
		if (!extract_ike_intermediate_v2KE(task->exchange.addke.kem, md,
						   &initiator_ke, logger)) {
			return STF_FATAL;
		}
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_log(logger, "ADDKE: responder encapsulating using initiator KE:");
			LDBG_hunk(logger, initiator_ke);
		}

		diag_t d = kem_responder_encapsulate(task->exchange.addke.kem, initiator_ke,
						     &task->responder, logger);
		if (d != NULL) {
			llog(RC_LOG, logger, "IKE_INTERMEDIATE encapsulate failed: %s", str_diag(d));
			pfree_diag(&d);
			return STF_FATAL;
		}
		if (LDBGP(DBG_BASE, logger)) {
			shunk_t ke = kem_responder_ke(task->responder);
			LDBG_log(logger, "ADDKE: responder KE:");
			LDBG_hunk(logger, ke);
		}

		ldbg(logger, "ADDKE: responder calculating skeyseed using prf %s",
		     task->prf->common.fqn);
		PK11SymKey *skeyseed =
			ikev2_IKE_INTERMEDIATE_kem_skeyseed(task->prf,
							    /*old*/task->d,
							    kem_responder_shared_key(task->responder),
							    task->ni, task->nr,
							    logger);
		ldbg(logger, "ADDKE: responder calculating KEYMAT using prf %s",
		     task->prf->common.fqn);
		task->keymat = ikev2_ike_sa_keymat(task->prf, skeyseed,
						   task->ni, task->nr,
						   &task->ike_spis,
						   task->nr_keymat_bytes,
						   logger);
		symkey_delref(logger, "skeyseed", &skeyseed);
	}

	return STF_OK;
}

stf_status process_v2_IKE_INTERMEDIATE_request_continue(struct ike_sa *ike,
							struct msg_digest *md,
							struct ikev2_task *task)
{
	struct logger *logger = ike->sa.logger;
	PEXPECT(ike->sa.logger, md != NULL);

	const struct secret_ppk_stuff *ppk = NULL;
	if (task->exchange.ppk) {

		for (const struct payload_digest *ppk_id_key_payls = md->pd[PD_v2N_PPK_IDENTITY_KEY];
		     ppk_id_key_payls != NULL; ppk_id_key_payls = ppk_id_key_payls->next) {

			ldbg(logger, "received PPK_IDENTITY_KEY");
			struct ppk_id_key_payload payl;
			if (!extract_v2N_ppk_id_key(&ppk_id_key_payls->pbs, &payl, ike)) {
				ldbg(logger, "failed to extract PPK_ID from PPK_IDENTITY payload. Abort!");
				return STF_FATAL;
			}

			const struct secret_ppk_stuff *ppk_candidate =
				get_ppk_stuff_by_id(payl.ppk_id_payl.ppk_id,
						    ike->sa.logger);
			if (ppk_candidate == NULL) {
				continue;
			}

			/* must free_chunk_content() */
			struct ppk_confirmation ppk_confirmation = calc_PPK_IDENTITY_KEY_confirmation(ike->sa.st_oakley.ta_prf,
												      ppk_candidate,
												      ike->sa.st_ni,
												      ike->sa.st_nr,
												      &ike->sa.st_ike_spis,
												      ike->sa.logger);
			if (hunk_eq(ppk_confirmation, payl.confirmation)) {
				ldbg(logger, "found matching PPK, will send PPK_IDENTITY back");
				ppk = ppk_candidate;
				PEXPECT(logger, hunk_eq(ppk->id, payl.ppk_id_payl.ppk_id));
				break;
			}
		}

		if (ppk == NULL) {
			if (ike->sa.st_connection->config->ppk.insist) {
				llog_sa(RC_LOG, ike, "no matching (PPK_ID, PPK) found and connection requires a valid PPK, terminating connection");
				record_v2N_response(ike->sa.logger, ike, md,
						    v2N_AUTHENTICATION_FAILED, empty_shunk/*no data*/,
						    ENCRYPTED_PAYLOAD);
				return STF_FATAL;
			}
			llog_sa(RC_LOG, ike,
				"failed to find a matching PPK, continuing without PPK");
		}
	}

	/* send Intermediate Exchange response packet */

	/* beginning of data going out */

	struct v2_message response;
	if (!open_v2_message("intermediate exchange response",
			     ike, ike->sa.logger,
			     md/*response*/, ISAKMP_v2_IKE_INTERMEDIATE,
			     reply_buffer, sizeof(reply_buffer), &response,
			     ENCRYPTED_PAYLOAD)) {
		return STF_INTERNAL_ERROR;
	}

	if (task->responder != NULL) {
		if (!emit_v2KE(kem_responder_ke(task->responder),
			       task->exchange.addke.kem,
			       response.pbs)) {
			return STF_INTERNAL_ERROR;
		}
	}

	if (ppk != NULL) {
		struct pbs_out ppks;
		if (!open_v2N_output_pbs(response.pbs, v2N_PPK_IDENTITY, &ppks)) {
			return STF_INTERNAL_ERROR;
		}
		/* we have a match, send PPK_IDENTITY back */
		const struct ppk_id_payload ppk_id_p =
			ppk_id_payload(PPK_ID_FIXED, HUNK_AS_SHUNK(&ppk->id), ike->sa.logger);
		if (!emit_unified_ppk_id(&ppk_id_p, &ppks)) {
			return STF_INTERNAL_ERROR;
		}
		close_pbs_out(&ppks);
	}

	if (!close_v2_message(&response)) {
		return STF_INTERNAL_ERROR;
	}

	/*
	 * For Intermediate Exchange, apply PRF to the peer's messages
	 * and store in state for further authentication.
	 */
	compute_intermediate_outbound_mac(ike, ike->sa.st_skey_pr_nss, &response,
					  &ike->sa.st_v2_ike_intermediate.responder);

	if (!record_v2_message(&response)) {
		return STF_INTERNAL_ERROR;
	}

	if (task->keymat != NULL) {
		extract_v2_ike_intermediate_keys(ike, task->keymat);
	}

	if (ppk != NULL) {
		recalc_v2_ike_intermediate_ppk_keymat(ike, ppk, HERE);
	}

	return STF_OK;
}

static stf_status process_v2_IKE_INTERMEDIATE_response(struct ike_sa *ike,
						       struct child_sa *null_child,
						       struct msg_digest *md)
{
	struct logger *logger = ike->sa.logger;
	PEXPECT(logger, null_child == NULL);
	struct connection *c = ike->sa.st_connection;

	/* save the most recent ID */
	ike->sa.st_v2_ike_intermediate.id = md->hdr.isa_msgid;

	/*
	 * Now that the payload has been decrypted, perform the
	 * intermediate exchange calculation.
	 *
	 * For Intermediate Exchange, apply PRF to the peer's messages
	 * and store in state for further authentication.
	 *
	 * Hence, here the initiator uses the responder's keys.
	 */
	if (!compute_intermediate_inbound_mac(ike, ike->sa.st_skey_pr_nss, md,
					      &ike->sa.st_v2_ike_intermediate.responder)) {
		/* already logged */
		return STF_FATAL;
	}

	/*
	 * if this connection has a newer Child SA than this state
	 * this negotiation is not relevant any more.  would this
	 * cover if there are multiple CREATE_CHILD_SA pending on this
	 * IKE negotiation ???
	 *
	 * XXX: this is testing for an IKE SA that's been superseded by
	 * a newer IKE SA (not child).  Suspect this is to handle a
	 * race where the other end brings up the IKE SA first?  For
	 * that case, shouldn't this state have been deleted?
	 *
	 * NOTE: a larger serialno does not mean superseded. crossed
	 * streams could mean the lower serial established later and is
	 * the "newest". Should > be replaced with !=   ?
	 */
	if (c->established_child_sa > ike->sa.st_serialno) {
		llog_sa(RC_LOG, ike,
			"state superseded by "PRI_SO", drop this negotiation",
			pri_so(c->established_child_sa));
		return STF_FATAL;
	}

	/* transfer ownership to task */
	struct ikev2_task task = {
		.exchange = current_ikev2_ike_intermediate_exchange(ike),
		/* for ADDKE decapsulate() */
		.initiator = ike->sa.st_kem.initiator,
		/* for skeyseed */
		.ni = clone_hunk_as_chunk(ike->sa.st_ni, "Ni"),
		.nr = clone_hunk_as_chunk(ike->sa.st_nr, "Nr"),
		.d = symkey_addref(logger, "d", ike->sa.st_skey_d_nss),
		.prf = ike->sa.st_oakley.ta_prf,
		/* for KEYMAT */
		.nr_keymat_bytes = nr_ikev2_ike_keymat_bytes(&ike->sa),
		.ike_spis = ike->sa.st_ike_spis,
	};
	ike->sa.st_kem.initiator = NULL;

	submit_ikev2_task(ike, md,
			  clone_thing(task, "initiator task"),
			  process_v2_IKE_INTERMEDIATE_response_helper,
			  process_v2_IKE_INTERMEDIATE_response_continue,
			  cleanup_IKE_INTERMEDIATE_task,
			  HERE);

	return STF_SUSPEND;
}

stf_status process_v2_IKE_INTERMEDIATE_response_helper(struct ikev2_task *task,
						       struct msg_digest *md,
						       struct logger *logger)
{
	if (task->initiator != NULL) {
		shunk_t responder_ke = null_shunk;
		if (!extract_ike_intermediate_v2KE(task->exchange.addke.kem, md,
						   &responder_ke, logger)) {
			/* already logged */
			return STF_FATAL;
		}
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_log(logger, "ADDKE: decapsulating using responder KE:");
			LDBG_hunk(logger, responder_ke);
		}
		diag_t d = kem_initiator_decapsulate(task->initiator, responder_ke, logger);
		if (d != NULL) {
			llog(RC_LOG, logger, "IKE_INTERMEDIATE decapsulate failed: %s", str_diag(d));
			pfree_diag(&d);
			return STF_FATAL;
		}

		ldbg(logger, "ADDKE: initiator calculating skeyseed using prf %s",
		     task->prf->common.fqn);
		PK11SymKey *skeyseed =
			ikev2_IKE_INTERMEDIATE_kem_skeyseed(task->prf,
							    /*old*/task->d,
							    kem_initiator_shared_key(task->initiator),
							    task->ni, task->nr,
							    logger);
		ldbg(logger, "ADDKE: initiator calculating KEYMAT using prf %s",
		     task->prf->common.fqn);
		task->keymat = ikev2_ike_sa_keymat(task->prf, skeyseed,
						   task->ni, task->nr,
						   &task->ike_spis,
						   task->nr_keymat_bytes,
						   logger);
		symkey_delref(logger, "skeyseed", &skeyseed);
	}

	return STF_OK;
}

stf_status process_v2_IKE_INTERMEDIATE_response_continue(struct ike_sa *ike,
							 struct msg_digest *md,
							 struct ikev2_task *task)
{
	struct logger *logger = ike->sa.logger;
	PEXPECT(ike->sa.logger, md != NULL);

	if (task->keymat != NULL) {
		extract_v2_ike_intermediate_keys(ike, task->keymat);
	}

	/*
	 * When there's PPK it must be performed last in the last
	 * IKE_INTERMEDIATE exchange.
	 */

	if (task->exchange.ppk) {
		if (md->pd[PD_v2N_PPK_IDENTITY] == NULL) {
			if (ike->sa.st_connection->config->ppk.insist) {
				llog_sa(RC_LOG, ike, "N(PPK_IDENTITY) not received and connection \
					      insists on PPK. Abort!");
				return STF_FATAL;
			}
			llog_sa(RC_LOG, ike, "N(PPK_IDENTITY) not received, continuing without PPK");
		} else {
			struct ppk_id_payload payl;
			if (!extract_v2N_ppk_identity(&md->pd[PD_v2N_PPK_IDENTITY]->pbs, &payl, ike)) {
				ldbg(logger, "failed to extract PPK_ID from PPK_IDENTITY payload. Abort!");
				return STF_FATAL;
			}
			const struct secret_ppk_stuff *ppk =
				get_ppk_stuff_by_id(/*ppk_id*/HUNK_AS_SHUNK(&payl.ppk_id),
						    ike->sa.logger);

			recalc_v2_ike_intermediate_ppk_keymat(ike, ppk, HERE);
		}
	}

	/*
	 * We've done an intermediate exchange round, if required
	 * perform another.
	 */

	const struct v2_exchange *next_exchange =
		(next_is_ikev2_ike_intermediate_exchange(ike) ? &v2_IKE_INTERMEDIATE_exchange
		 : &v2_IKE_AUTH_exchange);
	return next_v2_exchange(ike, md, next_exchange, HERE);
}

/*
 * IKE_INTERMEDIATE exchange and transitions.
 */

static void jam_ike_intermediate_details(struct jambuf *buf,
					 struct ike_sa *ike)
{
	jam_string(buf, " {");
	struct ikev2_ike_intermediate_exchange exchange = current_ikev2_ike_intermediate_exchange(ike);
	const char *sep = "";
	if (exchange.addke.kem != NULL) {
		jam_string(buf, sep); sep = ", ";
		jam_enum_human(buf, &ikev2_trans_type_names, exchange.addke.type);
		jam_string(buf, "=");
		jam_string(buf, exchange.addke.kem->common.fqn);
	}
	if (exchange.ppk) {
		jam_string(buf, sep); sep = ", ";
		jam_string(buf, "PPK");
	}
	jam_string(buf, "}");
}

static void llog_success_initiate_v2_IKE_INTERMEDIATE_request(struct ike_sa *ike,
							      const struct msg_digest *md)
{
	PEXPECT(ike->sa.logger, v2_msg_role(md) == NO_MESSAGE);
	LLOG_JAMBUF(RC_LOG, ike->sa.logger, buf) {
		jam_string(buf, "sent ");
		jam_enum_short(buf, &ikev2_exchange_names, ike->sa.st_v2_transition->exchange);
		jam_string(buf, " request to ");
		jam_endpoint_address_protocol_port_sensitive(buf, &ike->sa.st_remote_endpoint);
		jam_ike_intermediate_details(buf, ike);
	}
}

static void llog_success_process_v2_IKE_INTERMEDIATE_request(struct ike_sa *ike,
							     const struct msg_digest *md)
{
	PEXPECT(ike->sa.logger, v2_msg_role(md) == MESSAGE_REQUEST);
	LLOG_JAMBUF(RC_LOG, ike->sa.logger, buf) {
		jam_string(buf, "responder processed ");
		jam_enum_short(buf, &ikev2_exchange_names, ike->sa.st_v2_transition->exchange);
		jam_ike_intermediate_details(buf, ike);
		jam_string(buf, ", expecting ");
		jam_v2_exchanges(buf, &ike->sa.st_state->v2.ike_responder_exchanges);
		jam_string(buf, " request");
	}
}

static const struct v2_transition v2_IKE_INTERMEDIATE_initiate_transition = {
	.story      = "initiating IKE_INTERMEDIATE",
	.to = &state_v2_IKE_INTERMEDIATE_I,
	.exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	.processor  = initiate_v2_IKE_INTERMEDIATE_request,
	.llog_success = llog_success_initiate_v2_IKE_INTERMEDIATE_request,
	.timeout_event = EVENT_v2_RETRANSMIT,
};

static const struct v2_transition v2_IKE_INTERMEDIATE_responder_transition[] = {

	{ .story      = "Responder: process IKE_INTERMEDIATE request",
	  .to = &state_v2_IKE_INTERMEDIATE_R,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_REQUEST,
	  .message_payloads.required = v2P(SK),
	  .encrypted_payloads.optional = v2P(KE)|v2P(N/*PPK_IDENTITY*/),
	  .processor  = process_v2_IKE_INTERMEDIATE_request,
	  .llog_success = llog_success_process_v2_IKE_INTERMEDIATE_request,
	  .timeout_event = EVENT_v2_DISCARD, },

};

static const struct v2_transition v2_IKE_INTERMEDIATE_response_transition[] = {
	{ .story      = "processing IKE_INTERMEDIATE response",
	  .to = &state_v2_IKE_INTERMEDIATE_IR,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = v2P(SK),
	  .encrypted_payloads.optional = v2P(KE)|v2P(N/*PPK_IDENTITY*/),
	  .processor  = process_v2_IKE_INTERMEDIATE_response,
	  .llog_success = llog_success_ikev2_exchange_response,
	  .timeout_event = EVENT_v2_DISCARD, },
};

V2_STATE(IKE_INTERMEDIATE_R, "sent IKE_INTERMEDIATE response",
	 CAT_OPEN_IKE_SA, /*secured*/true,
	 &v2_IKE_INTERMEDIATE_exchange,
	 &v2_IKE_AUTH_exchange,
	 &v2_IKE_AUTH_EAP_exchange);

V2_EXCHANGE(IKE_INTERMEDIATE, "",
	    CAT_OPEN_IKE_SA, CAT_OPEN_IKE_SA, /*secured*/true,
	    &state_v2_IKE_SA_INIT_IR,
	    &state_v2_IKE_INTERMEDIATE_IR);
