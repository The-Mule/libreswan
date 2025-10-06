/* mechanisms for preshared keys (public, private, and preshared secrets)
 * definitions: lib/libswan/secrets.c
 *
 * Copyright (C) 1998-2002,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2003-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2009 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2016 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Vukasin Karadzic <vukasin.karadzic@gmail.com>
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
#ifndef _SECRETS_H
#define _SECRETS_H

#include <pk11pub.h>

#include "refcnt.h"
#include "lswcdefs.h"
#include "x509.h"
#include "id.h"
#include "err.h"
#include "realtime.h"
#include "ckaid.h"
#include "diag.h"
#include "keyid.h"
#include "refcnt.h"
#include "crypt_mac.h"
#include "ike_alg.h"		/* for HASH_ALGORITHM_IDENTIFIER */

struct logger;
struct state;	/* forward declaration */
struct secret;	/* opaque definition, private to secrets.c */
struct pubkey;		/* forward */
struct pubkey_content;	/* forward */
struct pubkey_type;	/* forward */
struct hash_desc;
struct hash_hunk;
struct hash_hunks;
struct cert;

/*
 * The raw public key.
 *
 * While this is abstracted as a SECKEYPublicKey, it can be thought of
 * as the Subject Public Key Info.
 *
 * Danger: this is often stolen.
 */

struct pubkey_content {
	const struct pubkey_type *type;
	keyid_t keyid;	/* see ipsec_keyblobtoid(3) */
	ckaid_t ckaid;
	SECKEYPublicKey *public_key;
};

void free_pubkey_content(struct pubkey_content *pubkey_content,
			 const struct logger *logger);

/*
 * private key types
 */
enum secret_kind {
	/* start at one so accidental 0 will not match */
	SECRET_PSK = 1,
	/*
	 * XXX: need three PUBKEY types?
	 *
	 * Most code doesn't care.  However the code searching for the
	 * private key uses ID+KIND where KIND is explicitly specified
	 * as RSA, ECDSA, et.al.  Changing that to the generic
	 * SECRET_PUBKEY may result in the wrong key being found?
	 *
	 * NSS, for instance, with certutils, lets the key kind be
	 * specified as a filter.
	 */
	SECRET_RSA,
	SECRET_ECDSA, /* should not be needed */
	SECRET_EDDSA, /* should not be needed */

	SECRET_XAUTH,
	SECRET_PPK,
	SECRET_NULL,
	SECRET_INVALID,
};

/*
 * Pre-shared Secrets, and XAUTH stuff.
 */

struct secret_preshared_stuff {
	size_t len;
	uint8_t ptr[];
};

const struct secret_preshared_stuff *secret_preshared_stuff(const struct secret *);

/*
 * PKI or raw public/private keys.
 */

struct secret_pubkey_stuff {
	struct refcnt refcnt;
	SECKEYPrivateKey *private_key;
	struct pubkey_content content;
};

struct secret_pubkey_stuff *secret_pubkey_stuff(const struct secret *s);

struct secret_pubkey_stuff *secret_pubkey_stuff_addref(struct secret_pubkey_stuff *, where_t where);
void secret_pubkey_stuff_delref(struct secret_pubkey_stuff **, where_t where);

/*
 * PPK
 */

struct secret_ppk_stuff {
	shunk_t key;
	chunk_t id;
	uint8_t data[];
};

const struct secret_ppk_stuff *secret_ppk_stuff(const struct secret *s);
const struct secret_ppk_stuff *secret_ppk_stuff_by_id(const struct secret *secrets, shunk_t ppk_id);

diag_t secret_pubkey_stuff_to_pubkey_der(struct secret_pubkey_stuff *pks, chunk_t *der);
diag_t pubkey_der_to_pubkey_content(shunk_t pubkey_der, struct pubkey_content *pkc);

extern struct id_list *lsw_get_idlist(const struct secret *s);

/*
 * return 1 to continue to next,
 * return 0 to return current secret
 * return -1 to return NULL
 */

struct secret_context;
typedef int (*secret_eval)(struct secret *secret,
			   enum secret_kind kind,
			   unsigned line,
			   struct secret_context *context);

struct secret *foreach_secret(struct secret *secrets,
			      secret_eval func,
			      struct secret_context *context);

struct hash_signature {
	size_t len;
	/*
	 * For ECDSA, see https://tools.ietf.org/html/rfc4754#section-7
	 * for where 1056 is coming from (it is the largest of the
	 * signature lengths amongst ECDSA 256, 384, and 521).
	 *
	 * For RSA this needs to be big enough to fit the modulus.
	 * Because the modulus in the SECItem is signed (but the raw
	 * value is unsigned), the modulus may have been prepended
	 * with an additional zero byte.  Hence the +1 to accommodate
	 * fuzzy checks against modulus.len.
	 *
	 * New code should just ask NSS for the signature length.
	 */
	uint8_t ptr[PMAX(BYTES_FOR_BITS(8192)+1/*RSA*/, BYTES_FOR_BITS(1056)/*ECDSA*/)];
};

struct pubkey_type {
	const char *name;
	enum secret_kind private_key_kind;
	enum ipseckey_algorithm_type ipseckey_algorithm;
	void (*free_pubkey_content)(struct pubkey_content *pkc,
				    const struct logger *logger);
	/* to/from the blob in DNS's IPSECKEY's Public Key field */
	diag_t (*ipseckey_rdata_to_pubkey_content)(shunk_t ipseckey_pubkey,
						   struct pubkey_content *pkc,
						   const struct logger *logger);
	err_t (*pubkey_content_to_ipseckey_rdata)(const struct pubkey_content *pkc,
						  chunk_t *ipseckey_pubkey,
						  enum ipseckey_algorithm_type *ipseckey_algorithm);
	/* nss */
	err_t (*extract_pubkey_content)(struct pubkey_content *pkc,
					SECKEYPublicKey *pubkey_nss,
					SECItem *ckaid_nss,
					const struct logger *logger);
	bool (*pubkey_same)(const struct pubkey_content *lhs,
			    const struct pubkey_content *rhs,
			    const struct logger *logger);
#define pubkey_strength_in_bits(PUBKEY) ((PUBKEY)->content.type->strength_in_bits(PUBKEY))
	size_t (*strength_in_bits)(const struct pubkey *pubkey);
};

struct pubkey_signer {
	const char *name;
	enum digital_signature_blob digital_signature_blob;
	const struct pubkey_type *type;
	struct hash_signature (*sign)(const struct pubkey_signer *signer,
				      const struct hash_desc *hasher,
				      const struct secret_pubkey_stuff *pks,
				      const struct hash_hunks *hunks,
				      struct logger *logger);
	struct hash_signature (*sign_hash)(const struct secret_pubkey_stuff *pks,
					   const uint8_t *hash_octets, size_t hash_len,
					   const struct hash_desc *hash_algo,
					   struct logger *logger);
	/*
	 * Danger! This function returns three results
	 *
	 * true;FATAL_DIAG=NULL: pubkey verified
	 * false;FATAL_DIAG=NULL: pubkey did not verify
	 * false;FATAL_DIAG!=NULL: operation should be aborted
	 */
	bool (*authenticate_hash_signature)(const struct pubkey_signer *signer,
					    const struct crypt_mac *hash,
					    shunk_t signature,
					    struct pubkey *kr,
					    const struct hash_desc *hash_algo,
					    diag_t *fatal_diag,
					    struct logger *logger);
	bool (*authenticate_message_signature)(const struct pubkey_signer *signer,
					       const struct hash_hunks *hunks,
					       shunk_t signature,
					       struct pubkey *kr,
					       const struct hash_desc *hash_algo,
					       diag_t *fatal_diag,
					       struct logger *logger);
	size_t (*jam_auth_method)(struct jambuf *,
				  const struct pubkey_signer *,
				  const struct pubkey *,
				  const struct hash_desc *);
};

extern const struct pubkey_type *pubkey_types[]; /* NULL terminated */

extern const struct pubkey_type pubkey_type_rsa;
extern const struct pubkey_type pubkey_type_ecdsa;

extern const struct pubkey_signer pubkey_signer_raw_rsa;		/* IKEv1 */
extern const struct pubkey_signer pubkey_signer_raw_pkcs1_1_5_rsa;	/* rfc7296 */
extern const struct pubkey_signer pubkey_signer_raw_ecdsa;		/* rfc4754 */

extern const struct pubkey_signer pubkey_signer_digsig_pkcs1_1_5_rsa;	/* rfc7427 */
extern const struct pubkey_signer pubkey_signer_digsig_rsassa_pss;	/* rfc7427 */
extern const struct pubkey_signer pubkey_signer_digsig_ecdsa;		/* rfc7427 */

const struct pubkey_type *pubkey_type_from_ipseckey_algorithm(enum ipseckey_algorithm_type alg);
const struct pubkey_type *pubkey_type_from_SECKEYPublicKey(SECKEYPublicKey *public_key);

struct hash_signature pubkey_hash_then_sign(const struct pubkey_signer *signer,
					    const struct hash_desc *hasher,
					    const struct secret_pubkey_stuff *pks,
					    const struct hash_hunks *hunks,
					    struct logger *logger);

/*
 * Public Key Machinery.
 *
 * This is a mashup of fields taken both from the certificate and the
 * subject public key info.
 */
struct pubkey {
	refcnt_t refcnt;	/* reference counted! */
	struct id id;
	enum dns_auth_level dns_auth_level;
	realtime_t installed_time;
	realtime_t until_time;
	uint32_t dns_ttl; /* from wire. until_time is derived using this */
	asn1_t issuer;
	struct pubkey_content content;
	/* for overalloc of issuer */
	uint8_t end[];
};

/*
 * XXX: While these fields seem to really belong in 'struct pubkey',
 * moving them isn't so easy - code assumes the fields are also found
 * in {RSA,ECDSA}_private_key's .pub.  Perhaps that structure have its
 * own copy.
 *
 * All pointers are references into the underlying PK structure.
 */

const ckaid_t *pubkey_ckaid(const struct pubkey *pk);
const keyid_t *pubkey_keyid(const struct pubkey *pk);

const ckaid_t *secret_ckaid(const struct secret *);
const keyid_t *secret_keyid(const struct secret *);

struct pubkey_list {
	struct pubkey *key;
	struct pubkey_list *next;
};

extern struct pubkey_list *pubkeys;	/* keys from ipsec.conf */

extern struct pubkey_list *free_public_keyentry(struct pubkey_list *p);
extern void free_public_keys(struct pubkey_list **keys);

diag_t unpack_dns_pubkey_content(enum ipseckey_algorithm_type algorithm_type,
				 shunk_t dnssec_pubkey,
				 struct pubkey_content *pkc,
				 const struct logger *logger);

diag_t unpack_dns_pubkey(const struct id *id, /* ASKK */
			 enum dns_auth_level dns_auth_level,
			 enum ipseckey_algorithm_type algorithm_type,
			 realtime_t install_time, realtime_t until_time,
			 uint32_t ttl,
			 shunk_t dnssec_pubkey,
			 struct pubkey **pubkey,
			 const struct logger *logger);

void add_pubkey(struct pubkey *pubkey, struct pubkey_list **pubkey_db);
/*add+delete-using-content*/
void replace_pubkey(struct pubkey *pubkey, struct pubkey_list **pubkey_db);

void delete_public_keys(struct pubkey_list **head,
			const struct id *id,
			const struct pubkey_type *type);
extern void form_keyid(chunk_t e, chunk_t n, keyid_t *keyid, size_t *keysize); /*XXX: make static? */

struct pubkey *pubkey_addref_where(struct pubkey *pk, where_t where);
#define pubkey_addref(PK) pubkey_addref_where(PK, HERE)
extern void pubkey_delref_where(struct pubkey **pkp, where_t where);
#define pubkey_delref(PKP) pubkey_delref_where(PKP, HERE)

bool secret_pubkey_same(const struct secret *lhs,
			const struct secret *rhs,
			const struct logger *logger);

extern void lsw_load_preshared_secrets(struct secret **psecrets, const char *secrets_file,
				       struct logger *logger);
extern void lsw_free_preshared_secrets(struct secret **psecrets, struct logger *logger);

extern struct secret *lsw_find_secret_by_id(struct secret *secrets,
					    enum secret_kind kind,
					    const struct id *my_id,
					    const struct id *his_id,
					    bool asym);

/* err_t!=NULL -> neither found nor loaded; loaded->just pulled in */
err_t find_or_load_private_key_by_cert(struct secret **secrets, const struct cert *cert,
				       struct secret_pubkey_stuff **pks, bool *load_needed,
				       const struct logger *logger);
err_t find_or_load_private_key_by_ckaid(struct secret **secrets, const ckaid_t *ckaid,
					struct secret_pubkey_stuff **pks, bool *load_needed,
					const struct logger *logger);

diag_t create_pubkey_from_cert(const struct id *id,
			       CERTCertificate *cert, struct pubkey **pk,
			       const struct logger *logger) MUST_USE_RESULT;

#endif /* _SECRETS_H */
