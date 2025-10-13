/* Key Exchange Method algorithms, for libreswan
 *
 * Copyright (C) 2025 Andrew Cagney
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

#include <cryptohi.h>
#include <keyhi.h>

#include "crypt_kem.h"
#include "crypt_symkey.h"

#include "lswnss.h"
#include "lswlog.h"
#include "ike_alg.h"
#include "ike_alg_kem_ops.h"
#include "passert.h"
#include "lswalloc.h"

static bool nss_ml_kem_calc_local_secret_1(const struct kem_desc *kem,
					   SECKEYPrivateKey **private_key,
					   SECKEYPublicKey **public_key,
					   struct logger *logger,
					   CK_MECHANISM_TYPE ml_kem_mechanism,
					   PK11SlotInfo *slot)
{
	LSW_CK_ML_KEM_PARAMETER_SET_TYPE generate_key_pair_parameter =
		kem->nss.ml_kem.generate_key_pair_parameter;
	(*private_key) = PK11_GenerateKeyPair(slot, ml_kem_mechanism,
					      &generate_key_pair_parameter,
					      public_key,
					      /*isPerm*/PR_FALSE,
					      /*isSensitive*/PK11_IsFIPS() ? PR_TRUE : PR_FALSE,
					      lsw_nss_get_password_context(logger));
	if (*public_key == NULL || *private_key == NULL) {
		llog_nss_error(ERROR_STREAM, logger, "PK11_GenerateKeyPair(%lx) failed",
			       ml_kem_mechanism);
		return false;
	}

	PASSERT(logger, (*private_key) != NULL);
	PASSERT(logger, (*public_key) != NULL);
	return true;
}

static bool nss_ml_kem_calc_local_secret(const struct kem_desc *kem,
					 SECKEYPrivateKey **private_key,
					 SECKEYPublicKey **public_key,
					 struct logger *logger)
{
	CK_MECHANISM_TYPE ml_kem_mechanism = LSW_CKM_ML_KEM_KEY_PAIR_GEN;
	PK11SlotInfo *slot = PK11_GetBestSlot(ml_kem_mechanism, lsw_nss_get_password_context(logger));
	if (slot == NULL) {
		llog_nss_error(ERROR_STREAM, logger, "PK11_GetBestSlot(%lx) failed",
			       ml_kem_mechanism);
		return false;
	}
	bool ok = nss_ml_kem_calc_local_secret_1(kem, private_key, public_key, logger,
						 ml_kem_mechanism, slot);
	PK11_FreeSlot(slot);
	return ok;
}

static shunk_t nss_ml_kem_local_secret_ke(const struct kem_desc *kem,
					  const SECKEYPublicKey *public_key)
{
	/* this only works on the initiator */
	passert(public_key->u.kyber.publicValue.len == kem->initiator_bytes);
	return shunk2(public_key->u.kyber.publicValue.data,
		      public_key->u.kyber.publicValue.len);
}

static diag_t nss_ml_kem_encapsulate_1(const struct kem_desc *kem,
				       shunk_t initiator_ke,
				       PK11SymKey **shared_key,
				       chunk_t *responder_ke,
				       struct logger *logger,
				       PRArenaPool *arena)
{
	void *password_context = lsw_nss_get_password_context(logger);
	SECStatus status;

	SECKEYPublicKey *initiator_pubkey = PORT_ArenaZNew(arena, SECKEYPublicKey);
	if (initiator_pubkey == NULL) {
		return diag_nss_error("allocating %s() SECKEYPublicKey", __func__);
	}

	initiator_pubkey->arena = arena;
	initiator_pubkey->keyType = kyberKey;
	initiator_pubkey->pkcs11Slot = NULL;
	initiator_pubkey->pkcs11ID = CK_INVALID_HANDLE;

	/*
	 * Now copy the kyber bits.
	 */

	SECKEYKyberPublicKey *kyber = &initiator_pubkey->u.kyber;
	kyber->params = kem->nss.ml_kem.encapsulate_parameter;

	status = SECITEM_MakeItem(arena, &kyber->publicValue,
				  initiator_ke.ptr, initiator_ke.len);
	if (status != SECSuccess) {
		return diag_nss_error("allocating %s() publicValue", __func__);
	}

	CK_OBJECT_HANDLE handle;
	{
		PK11SlotInfo *slot = PK11_GetBestSlot(LSW_CKM_ML_KEM, password_context);
		if (slot == NULL) {
			return diag_nss_error("getting %s() slot", __func__);
		}

		handle = PK11_ImportPublicKey(slot, initiator_pubkey, PR_FALSE);
		PK11_FreeSlot(slot);
		if (handle == CK_INVALID_HANDLE) {
			return diag_nss_error("importing %s() initiator pubkey", __func__);
		}
	}

	SECItem *nss_ke = NULL; /* must SECITEM_FreeItem(PR_TRUE); */
	status = PK11_Encapsulate(initiator_pubkey,
				  CKM_HKDF_DERIVE, PK11_ATTR_SESSION | PK11_ATTR_INSENSITIVE | PK11_ATTR_PUBLIC,
				  CKF_DERIVE,
				  shared_key,
				  &nss_ke);

	/* Destroy the imported public key */
	PASSERT(logger, initiator_pubkey->pkcs11Slot != NULL);
	PK11_DestroyObject(initiator_pubkey->pkcs11Slot, initiator_pubkey->pkcs11ID);
	PK11_FreeSlot(initiator_pubkey->pkcs11Slot);

	if (status != SECSuccess) {
		PEXPECT(logger, (*shared_key) == NULL);
		PEXPECT(logger, nss_ke == NULL);
		return diag_nss_error("encapsulate %s() initiator pubkey", __func__);
	}

	if (PBAD(logger, nss_ke == NULL) ||
	    PBAD(logger, (*shared_key) == NULL)) {
		return diag_nss_error("encapsulate %s() initiator pubkey", __func__);
	}

	/* let accounting know there's a new key in town */
	symkey_newref(logger, "responder-shared-key", (*shared_key));

	(*responder_ke) = clone_secitem_as_chunk((*nss_ke), "responder-ke");
	SECITEM_FreeItem(nss_ke, /*also-free-SECItem?*/PR_TRUE);

	return NULL;
}

static diag_t nss_ml_kem_encapsulate(const struct kem_desc *kem,
				     shunk_t initiator_ke,
				     PK11SymKey **shared_key,
				     chunk_t *responder_ke,
				     struct logger *logger)
{
	/*
	 * Allocate the public key, giving it its own arena.
	 *
	 * Since the arena contains everything allocated to the
	 * seckey, error recovery just requires freeing that.
	 */

	PRArenaPool *arena = PORT_NewArena(DER_DEFAULT_CHUNKSIZE);
	if (arena == NULL) {
		return diag_nss_error("allocating %s() arena", __func__);
	}

	diag_t d = nss_ml_kem_encapsulate_1(kem, initiator_ke,
					    shared_key,
					    responder_ke,
					    logger, arena);
	PORT_FreeArena(arena, /*zero*/PR_TRUE);
	return d;
}

static diag_t nss_ml_kem_decapsulate(const struct kem_desc *kem UNUSED,
				     SECKEYPrivateKey *initiator_private_key,
				     shunk_t responder_ke,
				     PK11SymKey**shared_key,
				     struct logger *logger UNUSED)
{
	SECItem nss_ke = same_shunk_as_secitem(responder_ke, siBuffer);
	SECStatus status = PK11_Decapsulate(initiator_private_key, &nss_ke,
					    CKM_HKDF_DERIVE,
					    PK11_ATTR_SESSION | PK11_ATTR_INSENSITIVE,
					    CKF_DERIVE,
					    shared_key);
	if (status != SECSuccess) {
		return diag_nss_error("decapsulating %s() responder KE", __func__);
	}

	/* tell accounting that there's a new key on the scene */
	symkey_newref(logger, "initiator-shared-key", (*shared_key));
	return NULL;
}

static void nss_ml_kem_check(const struct kem_desc *kem, struct logger *logger)
{
	const struct ike_alg *alg = &kem->common;
	pexpect_ike_alg(logger, alg, kem->ikev1_oakley_id < 0);
	pexpect_ike_alg(logger, alg, kem->ikev1_ipsec_id < 0);
	pexpect_ike_alg(logger, alg, kem->bytes == 0);
	pexpect_ike_alg(logger, alg, kem->initiator_bytes > 0);
	pexpect_ike_alg(logger, alg, kem->responder_bytes > 0);
	pexpect_ike_alg(logger, alg, kem->nss.ml_kem.generate_key_pair_parameter > 0);
	pexpect_ike_alg(logger, alg, kem->nss.ml_kem.encapsulate_parameter > 0);
}


const struct kem_ops ike_alg_kem_ml_kem_nss_ops = {
	.backend = "NSS(MLKEM)",
	.check = nss_ml_kem_check,
	.calc_local_secret = nss_ml_kem_calc_local_secret,
	.local_secret_ke = nss_ml_kem_local_secret_ke,
	.kem_encapsulate = nss_ml_kem_encapsulate,
	.kem_decapsulate = nss_ml_kem_decapsulate,
};
