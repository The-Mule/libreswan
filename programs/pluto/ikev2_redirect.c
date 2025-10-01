/* IKEv2 Redirect Mechanism (RFC 5685) related functions, for libreswan
 *
 * Copyright (C) 2018 - 2020 Vukasin Karadzic <vukasin.karadzic@gmail.com>
 * Copyright (C) 2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2020 Paul Wouters <pwouters@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <unistd.h>

#include "constants.h"

#include "defs.h"

#include "id.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "demux.h"
#include "ip_address.h"
#include "ipsec_doi.h"
#include "ikev2.h"
#include "ikev2_send.h"
#include "ikev2_informational.h"
#include "ikev2_states.h"
#include "ip_info.h"
#include "ikev2_redirect.h"
#include "initiate.h"
#include "log.h"
#include "pending.h"
#include "pluto_stats.h"
#include "orient.h"
#include "ikev2_message.h"
#include "ikev2_notification.h"
#include "show.h"
#include "ddos.h"

static emit_v2_INFORMATIONAL_request_payload_fn add_redirect_payload; /* type check */

static enum global_redirect global_redirect; /* see config_setup.[hc] and init_global_redirect() */

static struct redirect_dests global_dests = {0};

static const char *global_redirect_to(void)
{
	if (global_dests.whole == NULL)
		return ""; /* allows caller to strlen() */
	return global_dests.whole;
}

void free_redirect_dests(struct redirect_dests *dests)
{
	pfreeany(dests->whole);
	dests->next = 0;
	pfreeany(dests->splits);
}

void free_global_redirect_dests(void)
{
	free_redirect_dests(&global_dests);
}

bool set_redirect_dests(const char *rd_str, struct redirect_dests *dests)
{
	free_redirect_dests(dests);

	/* hope for the best */
	dests->whole = clone_str(rd_str, "redirect dests");
	dests->next = 0;
	dests->splits = ttoshunks(shunk1(dests->whole), ", ", EAT_EMPTY_SHUNKS);

	if (dests->splits->len == 0) {
		free_redirect_dests(dests);
		return false;
	}

	return true;
}

/*
 * Initialize the static struct redirect_dests variable.
 *
 * @param grd_str comma-separated string containing the destinations
 *  (a copy will be made so caller need not preserve the string).
 *  If it is not specified in conf file, gdr_str will be NULL.
 */

static bool set_global_redirect_dests(const char *grd_str)
{
	return set_redirect_dests(grd_str, &global_dests);
}

/*
 * Returns a string (shunk) destination to be shipped in REDIRECT
 * payload.
 *
 * @param rl struct containing redirect destinations
 * @return shunk_t string to be shipped.
 */

shunk_t next_redirect_dest(struct redirect_dests *rl)
{
	if (rl->next >= rl->splits->len) {
		rl->next = 0;
	}
	return rl->splits->item[rl->next++];
}

/*
 * Structure of REDIRECT Notify payload from RFC 5685.
 * The second part (Notification data) is interesting to us.
 * GW Ident Type: Type of Identity of new gateway
 * GW Ident Len:  Length of the New Responder GW Identity field
 *
 * Nonce Data is sent only if Redirect is happening during
 * IKE_SA_INIT exchange.
 *                      1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | Next Payload  |C|  RESERVED   |         Payload Length        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |Protocol ID(=0)| SPI Size (=0) |      Notify Message Type      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | GW Ident Type |  GW Ident Len |                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+                               ~
 * ~                   New Responder GW Identity                   ~
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * ~                        Nonce Data                             ~
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */

/*
 * Routines to build a notification data for REDIRECT (or REDIRECTED_FROM)
 * payload from the given string or ip.
 */

static bool emit_redirect_common(struct pbs_out *pbs,
				 enum gw_identity_type gwit,
				 shunk_t id,
				 shunk_t nonce)
{
	if (id.len > 0xFF) {
		llog(RC_LOG, pbs->logger,
		     "redirect destination longer than 255 octets; ignoring");
		return false;
	}

	struct ikev2_redirect_part gwi = {
		/* note: struct has no holes */
		.gw_identity_type = gwit,
		.gw_identity_len = id.len
	};

	if (!pbs_out_struct(pbs, gwi, &ikev2_redirect_desc, /*inner-pbs*/NULL)) {
		return false;
	}

	if (!pbs_out_hunk(pbs, id, "redirect ID")) {
		/* already logged */
		return false;
	}

	if (nonce.len > 0 &&
	    !pbs_out_hunk(pbs, nonce, "nonce in redirect notify")) {
		return false;
	}

	return true;
}

static bool emit_redirect_ip(struct pbs_out *pbs,
			     const ip_address *dest_ip,
			     shunk_t nonce)
{
	struct logger *logger = pbs->logger;
	enum gw_identity_type gwit;

	const struct ip_info *afi = address_type(dest_ip);
	PASSERT(logger, afi != NULL);
	switch (afi->af) {
	case AF_INET:
		gwit = GW_IPV4;
		break;
	case AF_INET6:
		gwit = GW_IPV6;
		break;
	default:
		bad_case(afi->af);
	}

	return emit_redirect_common(pbs, gwit, address_as_shunk(dest_ip), nonce);
}

static bool emit_redirect_destination(struct pbs_out *pbs,
				      shunk_t dest,
				      shunk_t nonce)
{
	ip_address ip_addr;
	err_t ugh = ttoaddress_num(dest, NULL/*UNSPEC*/, &ip_addr);

	if (ugh != NULL) {
		/*
		* ttoaddr_num failed: just ship dest_str as a FQDN
		* ??? it may be a bogus string
		*/
		return emit_redirect_common(pbs, GW_FQDN, dest, nonce);
	}

	return emit_redirect_ip(pbs, &ip_addr, nonce);
}

struct emit_v2_response_context {
	shunk_t Ni;
	shunk_t dest;
};

static emit_v2_response_fn emit_v2N_REDIRECT_response; /* type check*/
bool emit_v2N_REDIRECT_response(struct pbs_out *pbs,
				struct emit_v2_response_context *context)
{
	struct pbs_out redirect;
	if (!open_v2N_output_pbs(pbs, v2N_REDIRECT, &redirect)) {
		return false;
	}

	if (!emit_redirect_destination(&redirect, context->dest, context->Ni)) {
		return false;
	}

	close_pbs_out(&redirect);
	return true;
}

bool redirect_global(struct msg_digest *md)
{
	struct logger *logger = md->logger;

	/* if we don't support global redirection, no need to continue */
	if (global_redirect == GLOBAL_REDIRECT_NO ||
	    (global_redirect == GLOBAL_REDIRECT_AUTO && !require_ddos_cookies()))
		return false;

	/*
	 * From this point on we know that redirection is a must, and return
	 * value will be true. The only thing that we need to note is whether
	 * we redirect or not, and that difference will be marked with a
	 * log message.
	 */

	if (md->chain[ISAKMP_NEXT_v2Ni] == NULL) {
		/* Ni is used as cookie to protect REDIRECT in IKE_SA_INIT */
		ldbg(logger, "Ni payload required for REDIRECT is missing");
		pstats_ikev2_redirect_failed++;
		return true;
	}

	if (md->pd[PD_v2N_REDIRECTED_FROM] == NULL &&
	    md->pd[PD_v2N_REDIRECT_SUPPORTED] == NULL) {
		ldbg(logger, "peer didn't indicate support for redirection");
		pstats_ikev2_redirect_failed++;
		return true;
	}

	shunk_t Ni = pbs_in_left(&md->chain[ISAKMP_NEXT_v2Ni]->pbs);
	if (Ni.len == 0) {
		ldbg(logger, "Initiator nonce should not be zero length");
		pstats_ikev2_redirect_failed++;
		return true;
	}

	shunk_t dest = next_redirect_dest(&global_dests);
	if (dest.len == 0) {
		ldbg(logger, "no (meaningful) destination for global redirection has been specified");
		pstats_ikev2_redirect_failed++;
		return true;
	}

	struct emit_v2_response_context context = {
		.dest = dest,
		.Ni = Ni,
	};

	if (send_v2_response_from_md(md, "REDIRECT",
				     emit_v2N_REDIRECT_response,
				     &context)) {
		llog(RC_LOG, logger, "failed to send REDIRECT response");
		pstats_ikev2_redirect_failed++;
		return true;
	}

	pstats_ikev2_redirect_completed++;
	return true;
}

bool emit_v2N_REDIRECT(const char *destination, struct pbs_out *outs)
{
	struct pbs_out redirect;
	if (!open_v2N_output_pbs(outs, v2N_REDIRECT, &redirect)) {
		return false;
	}

	if (!emit_redirect_destination(&redirect, shunk1(destination),
				       /*nonce*/empty_shunk)) {
		return false;
	}

	close_pbs_out(&redirect);
	return true;
}

bool emit_v2N_REDIRECTED_FROM(const ip_address *old_gateway, struct pbs_out *outs)
{
	struct pbs_out redirected_from;
	if (!open_v2N_output_pbs(outs, v2N_REDIRECTED_FROM, &redirected_from)) {
		return false;
	}

	if (!emit_redirect_ip(&redirected_from, old_gateway, empty_shunk)) {
		return false;
	}

	close_pbs_out(&redirected_from);
	return true;
}

/*
 * Iterate through the allowed_targets_list, and if none of the
 * specified addresses matches the one from REDIRECT
 * payload, return FALSE
 */
static bool allow_to_be_redirected(const char *allowed_targets_list,
				   ip_address dest_ip,
				   struct logger *logger)
{
	if (allowed_targets_list == NULL || streq(allowed_targets_list, "%any"))
		return true;

	for (const char *t = allowed_targets_list;; ) {
		t += strspn(t, ", ");	/* skip leading separator */
		int len = (int) strcspn(t, ", ");	/* length of name */
		if (len == 0)
			break;	/* no more */

		ip_address ip_addr;
		err_t ugh = ttoaddress_num(shunk2(t, len), NULL/*UNSPEC*/, &ip_addr);

		if (ugh != NULL) {
			ldbg(logger, "address %.*s isn't a valid address", len, t);
		} else if (address_eq_address(dest_ip, ip_addr)) {
			ldbg(logger, "address %.*s is a match to received GW identity", len, t);
			return true;
		} else {
			ldbg(logger, "address %.*s is not a match to received GW identity", len, t);
		}
		t += len;	/* skip name */
	}
	ldbg(logger, "we did not find suitable address in the list specified by accept-redirect-to option");
	return false;
}

/*
 * Extract needed information from IKEv2 Notify Redirect
 * notification.
 *
 * @param data that was transferred in v2_REDIRECT Notify
 * @param char* list of addresses we accept being redirected
 * 	  to, specified with conn option accept-redirect-to
 * @param nonce that was send in IKE_SA_INIT request,
 * 	  we need to compare it with nonce data sent
 * 	  in Notify data. We do all that only if
 * 	  nonce isn't NULL.
 * @param redirect_ip ip address we need to redirect to
 * @return err_t NULL if everything went right,
 * 		 otherwise (non-NULL) what went wrong
 *
 * XXX: this logs and returns err_t; sometimes.
 */

static err_t parse_redirect_payload(const struct pbs_in *notify_pbs,
				    const char *allowed_targets_list,
				    const chunk_t *nonce,
				    ip_address *redirect_ip /* result */,
				    struct logger *logger)
{
	struct pbs_in input_pbs = *notify_pbs;
	struct ikev2_redirect_part gw_info;

	diag_t d = pbs_in_struct(&input_pbs, &ikev2_redirect_desc,
				 &gw_info, sizeof(gw_info), NULL);
	if (d != NULL) {
		llog(RC_LOG, logger, "%s", str_diag(d));
		pfree_diag(&d);
		return "received malformed REDIRECT payload";
	}

	const struct ip_info *af;

	switch (gw_info.gw_identity_type) {
	case GW_IPV4:
		af = &ipv4_info;
		break;
	case GW_IPV6:
		af = &ipv6_info;
		break;
	case GW_FQDN:
		af = NULL;
		break;
	default:
		return "bad GW Ident Type";
	}

	/* extract actual GW Identity */
	if (af == NULL) {
		/*
		 * The FQDN string isn't NUL-terminated.
		 *
		 * The length is stored in a byte so it cannot be
		 * larger than 0xFF.
		 * Some helpful compilers moan about this test being always true
		 * so I eliminated it:
		 *	PASSERT(gw_info.gw_identity_len <= 0xFF);
		 */
		shunk_t gw_str;
		diag_t d = pbs_in_shunk(&input_pbs, gw_info.gw_identity_len, &gw_str, "GW Identity");
		if (d != NULL) {
			llog(RC_LOG, logger, "%s", str_diag(d));
			pfree_diag(&d);
			return "error while extracting GW Identity from variable part of IKEv2_REDIRECT Notify payload";
		}

		err_t ugh = ttoaddress_dns(gw_str, NULL/*UNSPEC*/, redirect_ip);
		if (ugh != NULL)
			return ugh;
	} else {
		if (gw_info.gw_identity_len < af->ip_size) {
			return "transferred GW Identity Length is too small for an IP address";
		}
		diag_t d = pbs_in_address(&input_pbs, redirect_ip, af, "REDIRECT address");
		if (d != NULL) {
			llog(RC_LOG, logger, "%s", str_diag(d));
			pfree_diag(&d);
			return "variable part of payload does not match transferred GW Identity Length";
		}
		address_buf b;
		ldbg(logger, "   GW Identity IP: %s", str_address(redirect_ip, &b));
	}

	/*
	 * now check the list of allowed targets to
	 * see if parsed address matches any in the list
	 */
	if (!allow_to_be_redirected(allowed_targets_list, (*redirect_ip), logger))
		return "received GW Identity is not listed in accept-redirect-to conn option";

	size_t len = pbs_left(&input_pbs);

	if (nonce == NULL) {
		if (len > 0)
			return "unexpected extra bytes in Notify data after GW data - nonce should have been omitted";
	} else if (nonce->len != len || !memeq(nonce->ptr, input_pbs.cur, len)) {
		if (LDBGP(DBG_BASE, logger)) {
			LDBG_log(logger, "expected nonce");
			LDBG_hunk(logger, nonce);
			LDBG_log(logger, "received nonce");
			LDBG_dump(logger, input_pbs.cur, len);
		}
		return "received nonce does not match our expected nonce Ni (spoofed packet?)";
	}

	return NULL;
}

static void save_redirect(struct ike_sa *ike, struct msg_digest *md, ip_address to)
{
	struct logger *logger = ike->sa.logger;
	struct connection *c = ike->sa.st_connection;
	name_buf xchg;
	enum_short(&ikev2_exchange_names, md->hdr.isa_xchg, &xchg);

	ike->sa.st_viable_parent = false; /* just to be sure */

	c->redirect.attempt++;
	if (c->redirect.attempt > MAX_REDIRECTS) {
		llog(RC_LOG, logger, "%s redirect exceeds limit; assuming redirect loop",
		     xchg.buf);
		/*
		 * Clear redirect.counter, revival code will see this
		 * and, instead, schedule a revival.
		 *
		 * Per RFC force the revival delay to 5 minutes!!!.
		 */
		c->redirect.attempt = 0;
		c->revival.delay = deltatime_min(REVIVE_CONN_DELAY_MAX,
						 deltatime(REDIRECT_LOOP_DETECT_PERIOD));
		return;
	}

	/* will use this when initiating in a callback */
	c->redirect.ip = to;
	c->redirect.old_gw_address = c->remote->host.addr;
	c->remote->host.addr = to;
	ike->sa.st_skip_revival_as_redirecting = true;

	address_buf b;
	llog(RC_LOG, logger, "%s response redirects to new gateway %s",
	     xchg.buf, str_address_sensitive(&to, &b));
}

bool redirect_ike_auth(struct ike_sa *ike, struct msg_digest *md, stf_status *redirect_status)
{
	struct logger *logger = ike->sa.logger;

	if (md->pd[PD_v2N_REDIRECT] == NULL) {
		ldbg(logger, "redirect: no redirect payload in IKE_AUTH reply");
		return false;
	}

	ldbg(logger, "redirect: received v2N_REDIRECT in authenticated IKE_AUTH reply");
	if (!ike->sa.st_connection->config->redirect.accept) {
		ldbg(logger, "ignoring v2N_REDIRECT, we don't accept being redirected");
		return false;
	}

	ip_address redirect_ip;
	err_t err = parse_redirect_payload(&md->pd[PD_v2N_REDIRECT]->pbs,
					   ike->sa.st_connection->config->redirect.accept_to,
					   NULL,
					   &redirect_ip,
					   ike->sa.logger);
	if (err != NULL) {
		ldbg(logger, "redirect: warning: parsing of v2N_REDIRECT payload failed: %s", err);
		return false;
	}

	save_redirect(ike, md, redirect_ip);
	*redirect_status = STF_OK_INITIATOR_DELETE_IKE;
	return true;
}

/* helper function for send_v2_informational_request() */

static bool add_redirect_payload(struct ike_sa *ike, struct child_sa *null_child, struct pbs_out *pbs)
{
	PASSERT(ike->sa.logger, null_child == NULL);
	return emit_v2N_REDIRECT(ike->sa.st_active_redirect_gw, pbs);
}

static stf_status send_v2_INFORMATIONAL_v2N_REDIRECT_request(struct ike_sa *ike,
							     struct child_sa *null_child,
							     struct msg_digest *null_md)
{
	PASSERT(ike->sa.logger, null_child == NULL);
	PASSERT(ike->sa.logger, null_md == NULL);

	if (!record_v2_INFORMATIONAL_request("active REDIRECT informational request",
					     ike->sa.logger, ike, /*child*/NULL,
					     add_redirect_payload)) {
		return STF_INTERNAL_ERROR;
	}

	return STF_OK;
}

static stf_status process_v2_INFORMATIONAL_v2N_REDIRECT_request(struct ike_sa *ike,
								struct child_sa *null_child,
								struct msg_digest *md)
{
	struct logger *logger = ike->sa.logger;

	PASSERT(logger, v2_msg_role(md) == MESSAGE_REQUEST);
	PEXPECT(logger, null_child == NULL);

	/*
	 * This happens when we are original initiator, and we
	 * received REDIRECT payload during the active session.
	 *
	 * It trumps everything else.  Should delete also be ignored?
	 */
	if (PBAD(ike->sa.logger, md->pd[PD_v2N_REDIRECT] == NULL)) {
		return STF_INTERNAL_ERROR;
	}

	struct pbs_in redirect_pbs = md->pd[PD_v2N_REDIRECT]->pbs;
	ip_address redirect_to;
	err_t e = parse_redirect_payload(&redirect_pbs,
					 ike->sa.st_connection->config->redirect.accept_to,
					 NULL, &redirect_to, ike->sa.logger);
	if (e != NULL) {
		/* XXX: parse_redirect_payload() also often logs! */
		llog(WARNING_STREAM, logger, "parsing of v2N_REDIRECT payload failed: %s", e);
#if 0
		record_v2N_response(ike->sa.logger, ike, md,

				    v2N_INVALID_SYNTAX, NULL/*no-data*/,
				    ENCRYPTED_PAYLOAD);
		return STF_FATAL;
#else
		/*
		 * Act like nothing went wrong happened; it isn't
		 * clear when parse_redirect_payload() fails due to a
		 * syntax error or just something else.
		 */
		if (!record_v2_INFORMATIONAL_response("redirect response", ike->sa.logger,
						      ike, null_child, md,
						      /*emit-function*/NULL)) {
			return STF_INTERNAL_ERROR;
		}
		return STF_OK;
#endif
	}

	/*
	 * MAGIC: the initiate_redirect() callback initiates a new SA
	 * with the new IP, and then deletes the old IKE SA.
	 */
	save_redirect(ike, md, redirect_to);

	/*
	 * Schedule event to wipe this SA family and do it first.
	 * Remember, it won't run until after this function returns,
	 * however, it will run before any connections have had a
	 * chance to initiate.
	 *
	 * XXX: should this initiate a delete?  EXPIRE doesn't send delete requests.
	 *
	 * Should this force a delete send?
	 */
	event_force(EVENT_v2_EXPIRE, &ike->sa);

	/*
	 * The response is always empty.
	 */
	if (!record_v2_INFORMATIONAL_response("redirect response", ike->sa.logger,
					      ike, null_child, md,
					      /*emit-function*/NULL)) {
		return STF_INTERNAL_ERROR;
	}

	return STF_OK;
}

static stf_status process_v2_INFORMATIONAL_v2N_REDIRECT_response(struct ike_sa *ike,
							     struct child_sa *null_child,
							     struct msg_digest *md)
{
	PEXPECT(ike->sa.logger, md != NULL);
	PEXPECT(ike->sa.logger, null_child == NULL);
	return STF_OK;
}

static const struct v2_transition v2_INFORMATIONAL_v2N_REDIRECT_initiate_transition = {
	.story = "redirect IKE SA",
	.to = &state_v2_ESTABLISHED_IKE_SA,
	.exchange = ISAKMP_v2_INFORMATIONAL,
	.processor = send_v2_INFORMATIONAL_v2N_REDIRECT_request,
	.llog_success = ldbg_success_ikev2,
	.timeout_event =  EVENT_RETAIN,
};

static const struct v2_transition v2_INFORMATIONAL_v2N_REDIRECT_responder_transition[] = {
	{ .story      = "Informational Request",
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_REQUEST,
	  .message_payloads.required = v2P(SK),
	  .encrypted_payloads.required = v2P(N),
	  .encrypted_payloads.notification = v2N_REDIRECT,
	  .processor = process_v2_INFORMATIONAL_v2N_REDIRECT_request,
	  .llog_success = ldbg_success_ikev2,
	  .timeout_event = EVENT_RETAIN, },
};

static const struct v2_transition v2_INFORMATIONAL_v2N_REDIRECT_response_transition[] = {
	{ .story      = "Informational Response",
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = v2P(SK),
	  .encrypted_payloads.optional = v2P(N),
	  .processor  = process_v2_INFORMATIONAL_v2N_REDIRECT_response,
	  .llog_success = ldbg_success_ikev2,
	  .timeout_event = EVENT_RETAIN, },
};

const struct v2_exchange v2_INFORMATIONAL_v2N_REDIRECT_exchange = {
	.type = ISAKMP_v2_INFORMATIONAL,
	.exchange_subplot = " (redirect IKE SA)",
	.secured = true,
	.initiate.from = { &state_v2_ESTABLISHED_IKE_SA, },
	.initiate.transition = &v2_INFORMATIONAL_v2N_REDIRECT_initiate_transition,
	.transitions.responder = {
		ARRAY_REF(v2_INFORMATIONAL_v2N_REDIRECT_responder_transition),
	},
	.transitions.response = {
		ARRAY_REF(v2_INFORMATIONAL_v2N_REDIRECT_response_transition),
	},
};

stf_status process_v2_IKE_SA_INIT_response_v2N_REDIRECT(struct ike_sa *ike,
							struct child_sa *child,
							struct msg_digest *md)
{
	struct logger *logger = ike->sa.logger;
	struct connection *c = ike->sa.st_connection;
	PEXPECT(logger, child == NULL);
	if (!PEXPECT(logger, md->pd[PD_v2N_REDIRECT] != NULL)) {
		return STF_INTERNAL_ERROR;
	}
	struct pbs_in redirect_pbs = md->pd[PD_v2N_REDIRECT]->pbs;
	if (!ike->sa.st_connection->config->redirect.accept) {
		llog(RC_LOG, logger,
		     "ignoring v2N_REDIRECT, we don't accept being redirected");
		return STF_IGNORE;
	}

	ip_address redirect_ip;
	err_t err = parse_redirect_payload(&redirect_pbs,
					   c->config->redirect.accept_to,
					   &ike->sa.st_ni,
					   &redirect_ip,
					   ike->sa.logger);
	if (err != NULL) {
		llog(WARNING_STREAM, logger,
		     "parsing of v2N_REDIRECT payload failed: %s", err);
		return STF_IGNORE;
	}

	save_redirect(ike, md, redirect_ip);
	return STF_OK_INITIATOR_DELETE_IKE;
}

void show_global_redirect(struct show *s)
{
	SHOW_JAMBUF(s, buf) {
		jam_string(buf, "global-redirect=");
		jam_sparse_long(buf, &global_redirect_names, global_redirect);
		jam_string(buf, ", ");
		jam_string(buf, "global-redirect-to=");
		jam_string(buf, (strlen(global_redirect_to()) > 0 ? global_redirect_to() :
				 "<unset>"));
	}
}

void set_global_redirect(enum global_redirect redirect, const char *dests,
			 struct logger *logger)
{
	if (dests != NULL) {
		if (set_global_redirect_dests(dests)) {
			llog(RC_LOG, logger, "set global redirect target to %s", dests);
		} else {
			global_redirect = GLOBAL_REDIRECT_NO;
			llog(RC_LOG, logger,
			     "cleared global redirect targets and disabled global redirects");
		}
	}

	switch (redirect) {
	case GLOBAL_REDIRECT_NO:
		global_redirect = GLOBAL_REDIRECT_NO;
		llog(RC_LOG, logger, "set global redirect to 'no'");
		break;
	case GLOBAL_REDIRECT_YES:
	case GLOBAL_REDIRECT_AUTO:
		if (strlen(global_redirect_to()) == 0) {
			llog(RC_LOG, logger,
			     "ipsec whack: --global-redirect set to no as there are no active redirect targets");
			global_redirect = GLOBAL_REDIRECT_NO;
		} else {
			global_redirect = redirect;
			name_buf rn;
			llog(RC_LOG, logger,
			     "set global redirect to %s",
			     str_sparse_long(&global_redirect_names, global_redirect, &rn));
		}
		break;
	}
}

/*
 * Hopefully better than old code in plutomain.c.
 */

void init_global_redirect(enum global_redirect redirect, const char *redirect_to, struct logger *logger)
{
	global_redirect = redirect;
	switch (redirect) {
	case GLOBAL_REDIRECT_NO:
		if (redirect_to != NULL) {
			llog(WARNING_STREAM, logger,
			     "ignoring global-redirect-to='%s' as global-redirect=no",
			     redirect_to);
		}
		return;
	case GLOBAL_REDIRECT_YES:
	case GLOBAL_REDIRECT_AUTO:
		/* redirect_to could be NULL */
		if (redirect_to == NULL ||
		    !set_global_redirect_dests(redirect_to)) {
			name_buf rb;
			llog(WARNING_STREAM, logger,
			     "ignoring global-redirect=%s as global-redirect-to= is empty",
			     str_sparse_short(&global_redirect_names, redirect, &rb));
			global_redirect = GLOBAL_REDIRECT_NO;
			return;
		}
		llog(RC_LOG, logger,
		     "all IKE_SA_INIT requests will from now on be redirected to: %s",
		     redirect_to);
		global_redirect = redirect;
		return;
	}
	bad_case(redirect);
}
