/*
 * Cryptographic helper function - calculate DH
 *
 * Copyright (C) 2006-2008 Michael C. Richardson <mcr@xelerance.com>
 * Copyright (C) 2007-2009 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2012-2013 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2015-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2017 Antony Antony <antony@phenome.org>
 * Copyright (C) 2017-2019 Andrew Cagney <cagney@gnu.org>
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
 *
 * This code was developed with the support of IXIA communications.
 *
 */

#include "ike_alg.h"
#include "crypt_symkey.h"

#include "defs.h"
#include "log.h"
#include "ikev2_prf.h"
#include "crypt_dh.h"
#include "state.h"
#include "crypt_cipher.h"

void calc_v2_keymat(struct state *st,
		    PK11SymKey *old_skey_d, /* SKEYSEED IKE Rekey */
		    const struct prf_desc *old_prf, /* IKE Rekey */
		    const ike_spis_t *new_ike_spis)
{
	PK11SymKey *shared = st->st_dh_shared_secret;
	const ike_spis_t *ike_spis = new_ike_spis;
	struct logger *logger = st->logger;

	ldbgf(DBG_CRYPT, logger, "NSS: Started key computation");

	const struct encrypt_desc *cipher = st->st_oakley.ta_encrypt;
	const struct prf_desc *prf = st->st_oakley.ta_prf;
	const struct integ_desc *integ = st->st_oakley.ta_integ;

	PASSERT(st->logger, cipher != NULL);
	PASSERT(st->logger, prf != NULL);

	size_t key_size = st->st_oakley.enckeylen / BITS_IN_BYTE;
	size_t salt_size = cipher->salt_size;

	ldbg(logger, "calculating skeyseed using prf=%s integ=%s cipherkey-size=%zu salt-size=%zu",
	     prf->common.fqn,
	     (integ != NULL ? integ->common.fqn : "n/a"),
	     key_size, salt_size);

	PK11SymKey *skeyseed;
	if (old_skey_d == NULL) {
		/* generate SKEYSEED from key=(Ni|Nr), hash of shared */
		skeyseed = ikev2_ike_sa_skeyseed(prf, st->st_ni, st->st_nr,
						 shared, logger);
	}  else {
		skeyseed = ikev2_ike_sa_rekey_skeyseed(old_prf, old_skey_d,
						       shared,
						       st->st_ni, st->st_nr,
						       logger);
	}

	passert(skeyseed != NULL);

	/* now we have to generate the keys for everything */

	/* need to know how many bits to generate */
	/* SK_d needs PRF hasher key bytes */
	/* SK_p needs PRF hasher*2 key bytes */
	/* SK_e needs key_size*2 key bytes */
	/* ..._salt needs salt_size*2 bytes */
	/* SK_a needs integ's key size*2 bytes */

	int skd_bytes = prf->prf_key_size;
	int skp_bytes = prf->prf_key_size;
	int integ_size = integ != NULL ? integ->integ_keymat_size : 0;
	size_t total_keysize = skd_bytes + 2*skp_bytes + 2*key_size + 2*salt_size + 2*integ_size;
	PK11SymKey *finalkey = ikev2_ike_sa_keymat(prf, skeyseed,
						   st->st_ni, st->st_nr, ike_spis,
						   total_keysize, logger);
	symkey_delref(logger, "skeyseed", &skeyseed);

	size_t next_byte = 0;

	st->st_skey_d_nss = key_from_symkey_bytes("SK_d", finalkey,
						  next_byte, skd_bytes,
						  HERE, logger);
	next_byte += skd_bytes;

	st->st_skey_ai_nss = key_from_symkey_bytes("SK_ai", finalkey,
						   next_byte, integ_size,
						   HERE, logger);
	next_byte += integ_size;

	st->st_skey_ar_nss = key_from_symkey_bytes("SK_ar", finalkey,
						   next_byte, integ_size,
						   HERE, logger);
	next_byte += integ_size;

	/*
	 * The initiator encryption key and salt are extracted
	 * together.
	 */

	st->st_skey_ei_nss = encrypt_key_from_symkey_bytes("SK_ei",
							   cipher,
							   next_byte, key_size,
							   finalkey,
							   HERE, logger);
	next_byte += key_size;

	st->st_skey_initiator_salt = chunk_from_symkey_bytes("initiator salt",
							     finalkey,
							     next_byte, salt_size,
							     logger, HERE);
	next_byte += salt_size;

	/*
	 * The responder encryption key and salt are extracted
	 * together.
	 */

	st->st_skey_er_nss = encrypt_key_from_symkey_bytes("SK_er_k",
						   cipher,
						   next_byte, key_size,
						   finalkey,
						   HERE, logger);
	next_byte += key_size;

	st->st_skey_responder_salt = chunk_from_symkey_bytes("responder salt",
							     finalkey, next_byte,
							     salt_size,
							     logger, HERE);
	next_byte += salt_size;

	/*
	 * PPK
	 */

	st->st_skey_pi_nss = key_from_symkey_bytes("SK_pi", finalkey,
						   next_byte, skp_bytes,
						   HERE, logger);
	/* store copy of SK_pi_k for later use in authnull */
	st->st_skey_chunk_SK_pi = chunk_from_symkey("chunk_SK_pi", st->st_skey_pi_nss, logger);
	next_byte += skp_bytes;

	st->st_skey_pr_nss = key_from_symkey_bytes("SK_pr", finalkey,
						   next_byte, skp_bytes,
						   HERE, logger);
	/* store copy of SK_pr_k for later use in authnull */
	st->st_skey_chunk_SK_pr = chunk_from_symkey("chunk_SK_pr", st->st_skey_pr_nss, logger);

	ldbgf(DBG_CRYPT, logger, "NSS ikev2: finished computing individual keys for IKEv2 SA");
	symkey_delref(logger, "finalkey", &finalkey);

	switch (st->st_sa_role) {
	case SA_INITIATOR:
		/* encrypt outbound uses I */
		st->st_ike_encrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      ENCRYPT, FILL_WIRE_IV,
					      st->st_skey_ei_nss,
					      HUNK_AS_SHUNK(st->st_skey_initiator_salt),
					      st->logger);
		/* decrypt inbound uses R */
		st->st_ike_decrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      DECRYPT, USE_WIRE_IV,
					      st->st_skey_er_nss,
					      HUNK_AS_SHUNK(st->st_skey_responder_salt),
					      st->logger);
		break;
	case SA_RESPONDER:
		/* encrypt outbound uses R */
		st->st_ike_encrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      ENCRYPT, FILL_WIRE_IV,
					      st->st_skey_er_nss,
					      HUNK_AS_SHUNK(st->st_skey_responder_salt),
					      st->logger);
		/* decrypt inbound uses I */
		st->st_ike_decrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      DECRYPT, USE_WIRE_IV,
					      st->st_skey_ei_nss,
					      HUNK_AS_SHUNK(st->st_skey_initiator_salt),
					      st->logger);
		break;
	default:
		bad_case(st->st_sa_role);
	}

	st->hidden_variables.st_skeyid_calculated = true;
}

void recalc_v2_ppk_interm_keymat(struct state *st,
				 PK11SymKey *old_skey_d, /* SKEYSEED IKE Rekey */
		    		 shunk_t *ppk,
				 const ike_spis_t *new_ike_spis)
{
	PASSERT(st->logger, ppk != NULL);

	const ike_spis_t *ike_spis = new_ike_spis;
	struct logger *logger = st->logger;

	ldbgf(DBG_CRYPT, logger, "NSS: Started key computation");

	const struct encrypt_desc *cipher = st->st_oakley.ta_encrypt;
	const struct prf_desc *prf = st->st_oakley.ta_prf;
	const struct integ_desc *integ = st->st_oakley.ta_integ;

	PASSERT(st->logger, cipher != NULL);
	PASSERT(st->logger, prf != NULL);

	size_t key_size = st->st_oakley.enckeylen / BITS_IN_BYTE;
	size_t salt_size = cipher->salt_size;

	ldbg(logger, "calculating skeyseed' (ppk interm) using prf=%s integ=%s cipherkey-size=%zu salt-size=%zu",
	     prf->common.fqn,
	     (integ != NULL ? integ->common.fqn : "n/a"),
	     key_size, salt_size);

	/*
	 * old_skey_d and &st->st_skey_d_nss point to the same key,
	 * so we need to decouple them since we need old_skey_d to
	 * recalculate SKEYSEED' and then we need to populate it
	 * (&st->st_skey_d_nss)
	 */
	old_skey_d = key_from_symkey_bytes("'old' SK_d", st->st_skey_d_nss,
				  	   0, prf->prf_key_size,
					   HERE, logger);
	/* release old keys (and salts) */
	symkey_delref(logger, "SK_d", &st->st_skey_d_nss);
	symkey_delref(logger, "SK_ai", &st->st_skey_ai_nss);
	symkey_delref(logger, "SK_ar", &st->st_skey_ar_nss);
	symkey_delref(logger, "SK_ei", &st->st_skey_ei_nss);
	symkey_delref(logger, "SK_er", &st->st_skey_er_nss);
	symkey_delref(logger, "SK_pi", &st->st_skey_pi_nss);
	symkey_delref(logger, "SK_pr", &st->st_skey_pr_nss);
	free_chunk_content(&st->st_skey_chunk_SK_pi);
	free_chunk_content(&st->st_skey_chunk_SK_pr);
	free_chunk_content(&st->st_skey_initiator_salt);
	free_chunk_content(&st->st_skey_responder_salt);

	PK11SymKey *skeyseed;
	PK11SymKey *ppk_key = symkey_from_hunk("PPK Keying material", *ppk, logger);

	skeyseed = ikev2_ike_sa_ppk_interm_skeyseed(prf, old_skey_d, ppk_key, logger);
	symkey_delref(logger, "PPK key", &ppk_key);
	
	passert(skeyseed != NULL);

	/* now we have to generate the keys for everything */

	/* need to know how many bits to generate */
	/* SK_d needs PRF hasher key bytes */
	/* SK_p needs PRF hasher*2 key bytes */
	/* SK_e needs key_size*2 key bytes */
	/* ..._salt needs salt_size*2 bytes */
	/* SK_a needs integ's key size*2 bytes */

	int skd_bytes = prf->prf_key_size;
	int skp_bytes = prf->prf_key_size;
	int integ_size = integ != NULL ? integ->integ_keymat_size : 0;
	size_t total_keysize = skd_bytes + 2*skp_bytes + 2*key_size + 2*salt_size + 2*integ_size;
	PK11SymKey *finalkey = ikev2_ike_sa_keymat(prf, skeyseed,
						   st->st_ni, st->st_nr, ike_spis,
						   total_keysize, logger);
	symkey_delref(logger, "skeyseed", &skeyseed);

	size_t next_byte = 0;

	st->st_skey_d_nss = key_from_symkey_bytes("SK_d", finalkey,
						  next_byte, skd_bytes,
						  HERE, logger);
	next_byte += skd_bytes;

	st->st_skey_ai_nss = key_from_symkey_bytes("SK_ai", finalkey,
						   next_byte, integ_size,
						   HERE, logger);
	next_byte += integ_size;

	st->st_skey_ar_nss = key_from_symkey_bytes("SK_ar", finalkey,
						   next_byte, integ_size,
						   HERE, logger);
	next_byte += integ_size;

	/*
	 * The initiator encryption key and salt are extracted
	 * together.
	 */

	st->st_skey_ei_nss = encrypt_key_from_symkey_bytes("SK_ei",
							   cipher,
							   next_byte, key_size,
							   finalkey,
							   HERE, logger);
	next_byte += key_size;

	st->st_skey_initiator_salt = chunk_from_symkey_bytes("initiator salt",
							     finalkey,
							     next_byte, salt_size,
							     logger, HERE);
	next_byte += salt_size;

	/*
	 * The responder encryption key and salt are extracted
	 * together.
	 */

	st->st_skey_er_nss = encrypt_key_from_symkey_bytes("SK_er_k",
						   cipher,
						   next_byte, key_size,
						   finalkey,
						   HERE, logger);
	next_byte += key_size;

	st->st_skey_responder_salt = chunk_from_symkey_bytes("responder salt",
							     finalkey, next_byte,
							     salt_size,
							     logger, HERE);
	next_byte += salt_size;

	st->st_skey_pi_nss = key_from_symkey_bytes("SK_pi", finalkey,
						   next_byte, skp_bytes,
						   HERE, logger);
	/* store copy of SK_pi_k for later use in authnull */
	st->st_skey_chunk_SK_pi = chunk_from_symkey("chunk_SK_pi", st->st_skey_pi_nss, logger);
	next_byte += skp_bytes;

	st->st_skey_pr_nss = key_from_symkey_bytes("SK_pr", finalkey,
						   next_byte, skp_bytes,
						   HERE, logger);
	/* store copy of SK_pr_k for later use in authnull */
	st->st_skey_chunk_SK_pr = chunk_from_symkey("chunk_SK_pr", st->st_skey_pr_nss, logger);

	ldbgf(DBG_CRYPT, logger, "NSS ikev2: finished computing individual keys for IKEv2 SA (ppk intermediate)");
	symkey_delref(logger, "finalkey", &finalkey);

	switch (st->st_sa_role) {
	case SA_INITIATOR:
		/* encrypt outbound uses I */
		st->st_ike_encrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      ENCRYPT, FILL_WIRE_IV,
					      st->st_skey_ei_nss,
					      HUNK_AS_SHUNK(st->st_skey_initiator_salt),
					      st->logger);
		/* decrypt inbound uses R */
		st->st_ike_decrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      DECRYPT, USE_WIRE_IV,
					      st->st_skey_er_nss,
					      HUNK_AS_SHUNK(st->st_skey_responder_salt),
					      st->logger);
		break;
	case SA_RESPONDER:
		/* encrypt outbound uses R */
		st->st_ike_encrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      ENCRYPT, FILL_WIRE_IV,
					      st->st_skey_er_nss,
					      HUNK_AS_SHUNK(st->st_skey_responder_salt),
					      st->logger);
		/* decrypt inbound uses I */
		st->st_ike_decrypt_cipher_context =
			cipher_context_create(st->st_oakley.ta_encrypt,
					      DECRYPT, USE_WIRE_IV,
					      st->st_skey_ei_nss,
					      HUNK_AS_SHUNK(st->st_skey_initiator_salt),
					      st->logger);
		break;
	default:
		bad_case(st->st_sa_role);
	}

	st->hidden_variables.st_skeyid_calculated = true;
}
