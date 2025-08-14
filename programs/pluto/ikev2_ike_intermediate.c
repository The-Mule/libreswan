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

static ikev2_state_transition_fn process_v2_IKE_INTERMEDIATE_request;	/* type assertion */
static bool recalc_v2_ike_intermediate_keymat(struct ike_sa *ike, PK11SymKey *skeyseed);

/*
 * Without this the code makes little sense.
 * https://datatracker.ietf.org/doc/html/draft-ietf-ipsecme-ikev2-intermediate-08
 *
 *                       1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ^ ^ <-- MESSAGE START
 *  |                       IKE SA Initiator's SPI                  | | |
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
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ v |
 *  |                                                               |   |
 *  ~                 Unencrypted payloads (if any)                 ~   |
 *  |                                                               |   |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ ^ | <-- ENCRYPTED HEADER
 *  | Next Payload  |C|  RESERVED   |    Adjusted Payload Length    | | |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ | v
 *  |                                                               | |
 *  ~                     Initialization Vector                     ~ E
 *  |                                                               | E
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ c ^ <-- PLAIN
 *  |                                                               | r |
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
				     const uint8_t *message_start,
				     shunk_t plain,
				     chunk_t *int_auth_ir)
{
	struct logger *logger = ike->sa.logger;

	/*
	 * Define variables that match the naming scheme used by the
	 * RFC's ASCII diagram above.
	 */

	/*
	 * Extract the message header, will need to patch up the
	 * trailing length field.
	 */
	struct isakmp_hdr adjusted_message_header;
	shunk_t header = {
		.ptr = message_start,
		.len = sizeof(adjusted_message_header),
	};

	struct ikev2_generic adjusted_encrypted_payload_header;
	shunk_t unencrypted_payloads = {
		.ptr = header.ptr + header.len,
		.len = ((const uint8_t*) plain.ptr - message_start
			- ike->sa.st_oakley.ta_encrypt->wire_iv_size
			- sizeof(adjusted_encrypted_payload_header)
			- header.len),
	};

	shunk_t encrypted_payload = {
		.ptr = unencrypted_payloads.ptr + unencrypted_payloads.len,
		.len = plain.len + sizeof(adjusted_encrypted_payload_header),
	};

	/*
	 * Extract the encrypted header, will need to patch up the
	 * trailing Payload Length field.
	 */
	shunk_t encrypted_payload_header = {
		.ptr = encrypted_payload.ptr,
		.len = sizeof(adjusted_encrypted_payload_header),
	};

	/* skip the IV */
	shunk_t inner_payloads = {
		.ptr = plain.ptr,
		.len = plain.len,
	};

	/*
	 * compute the PRF over "A" + "P" as in:
	 *
	 * IntAuth_i1 = prf(SK_pi1,              IntAuth_i1A [| IntAuth_i1P])
	 * IntAuth_i2 = prf(SK_pi2, IntAuth_i1 | IntAuth_i2A [| IntAuth_i2P])
	 *
	 * IntAuth_r1 = prf(SK_pr1,              IntAuth_r1A [| IntAuth_r1P])
	 * IntAuth_r2 = prf(SK_pr2, IntAuth_r1 | IntAuth_r2A [| IntAuth_r2P])
	 */

	/* prf(SK_p[ir](N), ... */
	struct crypt_prf *prf = crypt_prf_init_symkey("prf(IntAuth_*_A [| IntAuth_*_P])",
						      ike->sa.st_oakley.ta_prf,
						      "SK_p", intermediate_key,
						      ike->sa.logger);

	/* prf(..., IntAuth_[ir](N-1) | ...) */
	if (int_auth_ir->len > 0) {
		crypt_prf_update_hunk(prf, "IntAuth_[ir](N-1)", *int_auth_ir);
	}

	/* A: prf(... | IntAuth_[ir](N)A | ...) */

	/* the message header needs its Length adjusted */
	size_t adjusted_payload_length = (header.len
				 + unencrypted_payloads.len
				 + encrypted_payload_header.len
				 + inner_payloads.len);
	ldbg(logger, "adjusted payload length: %zu", adjusted_payload_length);
	memcpy(&adjusted_message_header, header.ptr, header.len);
	hton_thing(adjusted_payload_length, adjusted_message_header.isa_length);
	crypt_prf_update_thing(prf, "Adjusted Message Header", adjusted_message_header);

	/* Unencrypted payload */
	crypt_prf_update_hunk(prf, "Unencrypted payloads (if any)", unencrypted_payloads);

	/* encrypted payload header needs its Length adjusted */
	size_t adjusted_encrypted_payload_length = encrypted_payload_header.len + inner_payloads.len;
	ldbg(logger, "adjusted encrypted payload length: %zu", adjusted_encrypted_payload_length);
	memcpy(&adjusted_encrypted_payload_header, encrypted_payload_header.ptr, encrypted_payload_header.len);
	hton_thing(adjusted_encrypted_payload_length, adjusted_encrypted_payload_header.isag_length);
	crypt_prf_update_thing(prf, "Adjusted Encrypted (SK) Header", adjusted_encrypted_payload_header);

	/* P: prf(... | IntAuth_[ir](N)P) */

	crypt_prf_update_bytes(prf, "Inner payloads (decrypted)",
			       inner_payloads.ptr, inner_payloads.len);

	/* extract the mac; replace existing value */
	struct crypt_mac mac = crypt_prf_final_mac(&prf, NULL/*no-truncation*/);
	free_chunk_content(int_auth_ir);
	*int_auth_ir = clone_hunk(mac, "IntAuth");
}

const struct kem_desc *next_additional_kem_desc(struct ike_sa *ike)
{
	unsigned ke_index = ike->sa.st_v2_ike_intermediate.ke_index;
	if (ke_index >= ike->sa.st_oakley.ta_addke.len) {
		return NULL;
	}

	/* none is allowed, not NULL?!? */
	const struct kem_desc *kem = ike->sa.st_oakley.ta_addke.list[ke_index].kem;
	if (PBAD(ike->sa.logger, kem == NULL)) {
		return NULL;
	}

	return kem;
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

	/* beginning of data going out */

	struct v2_message request;
	if (!open_v2_message("intermediate exchange request",
			     ike, ike->sa.logger,
			     NULL/*request*/, ISAKMP_v2_IKE_INTERMEDIATE,
			     reply_buffer, sizeof(reply_buffer), &request,
			     ENCRYPTED_PAYLOAD)) {
		return STF_INTERNAL_ERROR;
	}

	if (ike->sa.st_v2_ike_ppk == PPK_IKE_INTERMEDIATE) {
		struct connection *const c = ike->sa.st_connection;
		struct shunks *ppk_ids_shunks = c->config->ppk_ids_shunks;
		bool found_one = false;

		if (ppk_ids_shunks == NULL) {
			/* find any matching PPK and PPK_ID */
			const struct secret_ppk_stuff *ppk =
				get_connection_ppk_and_ppk_id(c);
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
	compute_intermediate_mac(ike, ike->sa.st_skey_pi_nss,
				 request.sk.pbs.container->start,
				 HUNK_AS_SHUNK(request.sk.cleartext) /* inner payloads */,
				 &ike->sa.st_v2_ike_intermediate.initiator);

	if (!encrypt_v2SK_payload(&request.sk)) {
		llog(RC_LOG, request.logger,
		     "error encrypting response");
		return STF_INTERNAL_ERROR;
	}

	record_v2_message(pbs_out_all(&request.message),
			  request.outgoing_fragments,
			  request.logger);

	return STF_OK;
}

static bool recalc_v2_ike_intermediate_ppk_keymat(struct ike_sa *ike, shunk_t ppk, where_t where)
{
	struct logger *logger = ike->sa.logger;
	const struct prf_desc *prf = ike->sa.st_oakley.ta_prf;

	ldbg(logger, "%s() calculating skeyseed using prf %s",
	     __func__, prf->common.fqn);

	/*
	 * We need old_skey_d to recalculate SKEYSEED'.
	 */

	PK11SymKey *skeyseed =
		ikev2_ike_sa_ppk_interm_skeyseed(prf,
						 /*old*/ike->sa.st_skey_d_nss,
						 ppk, logger);
	if (skeyseed == NULL) {
		llog_pexpect(logger, where, "ppk SKEYSEED failed");
		return false;
	}

	return recalc_v2_ike_intermediate_keymat(ike, skeyseed);
}

bool recalc_v2_ike_intermediate_keymat(struct ike_sa *ike, PK11SymKey *skeyseed)
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
	free_chunk_content(&ike->sa.st_skey_chunk_SK_pi);
	free_chunk_content(&ike->sa.st_skey_chunk_SK_pr);
	free_chunk_content(&ike->sa.st_skey_initiator_salt);
	free_chunk_content(&ike->sa.st_skey_responder_salt);
	cipher_context_destroy(&ike->sa.st_ike_encrypt_cipher_context, logger);
	cipher_context_destroy(&ike->sa.st_ike_decrypt_cipher_context, logger);

	/* now we have to generate the keys for everything */

	calc_v2_ike_keymat(&ike->sa, skeyseed, &ike->sa.st_ike_spis);
	symkey_delref(logger, "skeyseed", &skeyseed);
	return true;
}

stf_status process_v2_IKE_INTERMEDIATE_request(struct ike_sa *ike,
					       struct child_sa *null_child,
					       struct msg_digest *md)
{
	struct logger *logger = ike->sa.logger;

	PEXPECT(logger, null_child == NULL);

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
	if (ike->sa.st_v2_ike_intermediate.id > 2/*magic!*/) {
		llog_sa(RC_LOG, ike, "too many IKE_INTERMEDIATE exchanges");
		return STF_FATAL;
	}

	/*
	 * Now that the payload has been decrypted, perform the
	 * intermediate exchange calculation.
	 *
	 * For Intermediate Exchange, apply PRF to the peer's messages
	 * and store in state for further authentication.
	 *
	 * Hence, here the responder uses the initiator's keys.
	 */
	shunk_t plain = pbs_in_all(&md->chain[ISAKMP_NEXT_v2SK]->pbs);
	compute_intermediate_mac(ike, ike->sa.st_skey_pi_nss,
				 md->packet_pbs.start, plain,
				 &ike->sa.st_v2_ike_intermediate.initiator);

	const struct secret_ppk_stuff *ppk = NULL;
	if (ike->sa.st_v2_ike_ppk == PPK_IKE_INTERMEDIATE) {

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
				ldbg(logger, "found matching PPK, send PPK_IDENTITY back");
				ppk = ppk_candidate;
				PEXPECT(logger, hunk_eq(ppk->id, payl.ppk_id_payl.ppk_id));
				break;
			}
		}

		if (ppk == NULL) {
			if (ike->sa.st_connection->config->ppk.insist) {
				llog_sa(RC_LOG, ike, "No matching (PPK_ID, PPK) found and connection requires \
					      a valid PPK. Abort!");
				record_v2N_response(ike->sa.logger, ike, md,
						    v2N_AUTHENTICATION_FAILED, empty_shunk/*no data*/,
						    ENCRYPTED_PAYLOAD);
				return STF_FATAL;
			} else {
				llog_sa(RC_LOG, ike,
					"failed to find a matching PPK, continuing without PPK");
			}
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

	if (ppk != NULL) {
		struct pbs_out ppks;
		if (!open_v2N_output_pbs(response.pbs, v2N_PPK_IDENTITY, &ppks)) {
			return STF_INTERNAL_ERROR;
		}
		/* we have a match, send PPK_IDENTITY back */
		const struct ppk_id_payload ppk_id_p =
			ppk_id_payload(PPK_ID_FIXED, HUNK_AS_SHUNK(ppk->id), ike->sa.logger);
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
	compute_intermediate_mac(ike, ike->sa.st_skey_pr_nss,
				 response.sk.pbs.container->start,
				 HUNK_AS_SHUNK(response.sk.cleartext) /* inner payloads */,
				 &ike->sa.st_v2_ike_intermediate.responder);

	if (!encrypt_v2SK_payload(&response.sk)) {
		llog(RC_LOG, response.logger,
		     "error encrypting response");
		return STF_INTERNAL_ERROR;
	}

	record_v2_message(pbs_out_all(&response.message),
			  response.outgoing_fragments,
			  response.logger);

	if (ppk != NULL) {
		recalc_v2_ike_intermediate_ppk_keymat(ike, ppk->key, HERE);
		llog(RC_LOG, ike->sa.logger, "PPK used in IKE_INTERMEDIATE as responder");
	}

	return STF_OK;
}

static stf_status process_v2_IKE_INTERMEDIATE_response(struct ike_sa *ike,
						       struct child_sa *null_child,
						       struct msg_digest *md)
{
	struct logger *logger = ike->sa.logger;
	PEXPECT(logger, null_child == NULL);

	/*
	 * The function below always schedules a dh calculation - even
	 * when it's been performed earlier (there's something in the
	 * intermediate echange about this?).
	 *
	 * So that things don't pexpect, blow away the old shared secret.
	 */
	ldbg(logger, "HACK: blow away old shared secret as going to re-compute it");
	symkey_delref(ike->sa.logger, "st_dh_shared_secret", &ike->sa.st_dh_shared_secret);
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
	shunk_t plain = pbs_in_all(&md->chain[ISAKMP_NEXT_v2SK]->pbs);
	compute_intermediate_mac(ike, ike->sa.st_skey_pr_nss,
				 md->packet_pbs.start, plain,
				 &ike->sa.st_v2_ike_intermediate.responder);

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

	ldbg(logger, "No KE payload in INTERMEDIATE RESPONSE, not calculating keys, going to AUTH by completing state transition");

	/*
	 * Initiate the calculation of g^xy.
	 *
	 * Form and pass in the full SPI[ir] that will eventually be
	 * used by this IKE SA.  Only once DH has been computed and
	 * the SA is secure (but not authenticated) should the state's
	 * IKE SPIr be updated.
	 */

	PEXPECT(logger, !ike_spi_is_zero(&ike->sa.st_ike_spis.responder));
	ike->sa.st_ike_rekey_spis = (ike_spis_t) {
		.initiator = ike->sa.st_ike_spis.initiator,
		.responder = md->hdr.isa_ike_responder_spi,
	};

	/*
	 * XXX: does the keymat need to be re-computed here?
	 */

	if (ike->sa.st_v2_ike_ppk == PPK_IKE_INTERMEDIATE && md->pd[PD_v2N_PPK_IDENTITY] != NULL) {
		struct ppk_id_payload payl;
		if (!extract_v2N_ppk_identity(&md->pd[PD_v2N_PPK_IDENTITY]->pbs, &payl, ike)) {
			ldbg(logger, "failed to extract PPK_ID from PPK_IDENTITY payload. Abort!");
			return STF_FATAL;
		}
		const struct secret_ppk_stuff *ppk =
			get_ppk_stuff_by_id(/*ppk_id*/HUNK_AS_SHUNK(payl.ppk_id),
					    ike->sa.logger);

		recalc_v2_ike_intermediate_ppk_keymat(ike, ppk->key, HERE);
		llog(RC_LOG, ike->sa.logger, "PPK used in IKE_INTERMEDIATE as initiator");
	}
	if (md->pd[PD_v2N_PPK_IDENTITY] == NULL) {
		if (ike->sa.st_connection->config->ppk.insist) {
			llog_sa(RC_LOG, ike, "N(PPK_IDENTITY) not received and connection \
					      insists on PPK. Abort!");
			return STF_FATAL;
		} else {
			llog_sa(RC_LOG, ike,
				"N(PPK_IDENTITY) not received, continuing without PPK");
		}
	}
	/*
	 * We've done one intermediate exchange round, now proceed to
	 * IKE AUTH.
	 */
#if 0
	return next_v2_transition(ike, md, &initiate_v2_IKE_INTERMEDIATE_transition, HERE);
#else
	return next_v2_exchange(ike, md, &v2_IKE_AUTH_exchange, HERE);
#endif
}

/*
 * IKE_INTERMEDIATE exchange and transitions.
 */

static const struct v2_transition v2_IKE_INTERMEDIATE_initiate_transition = {
	.story      = "initiating IKE_INTERMEDIATE",
	.to = &state_v2_IKE_INTERMEDIATE_I,
	.exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	.processor  = initiate_v2_IKE_INTERMEDIATE_request,
	.llog_success = llog_success_ikev2_exchange_initiator,
	.timeout_event = EVENT_v2_RETRANSMIT,
};

static const struct v2_transition v2_IKE_INTERMEDIATE_responder_transition[] = {

	{ .story      = "Responder: process IKE_INTERMEDIATE request",
	  .to = &state_v2_IKE_INTERMEDIATE_R,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_REQUEST,
	  .message_payloads.required = v2P(SK),
	  .encrypted_payloads.required = LEMPTY,
	  .encrypted_payloads.optional = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_request,
	  .llog_success = llog_success_ikev2_exchange_responder,
	  .timeout_event = EVENT_v2_DISCARD, },

};

static const struct v2_transition v2_IKE_INTERMEDIATE_response_transition[] = {
	{ .story      = "processing IKE_INTERMEDIATE response",
	  .to = &state_v2_IKE_INTERMEDIATE_IR,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = v2P(SK),
	  .message_payloads.optional = LEMPTY,
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
	    ", initiating IKE_INTERMEDIATE or IKE_AUTH",
	    CAT_OPEN_IKE_SA, CAT_OPEN_IKE_SA, /*secured*/true,
	    &state_v2_IKE_SA_INIT_IR,
	    &state_v2_IKE_INTERMEDIATE_IR);
