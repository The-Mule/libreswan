/* ip cidr, for libreswan
 *
 * Copyright (C) 2019-2024 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2023 Brady Johnson <bradyjoh@redhat.com>
 * Copyright (C) 2021 Antony Antony <antony@phenome.org>
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/lgpl-2.1.txt>.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
 * License for more details.
 */

#include "passert.h"
#include "jambuf.h"
#include "lswlog.h"

#include "ip_cidr.h"
#include "ip_info.h"

const ip_cidr unset_cidr;

ip_cidr cidr_from_raw(where_t where,
		      const struct ip_info *afi,
		      const struct ip_bytes bytes,
		      unsigned prefix_len)
{

 	/* combine */
	ip_cidr cidr = {
		.ip.is_set = true,
		.ip.version = afi->ip.version,
		.bytes = bytes,
		.prefix_len = prefix_len,
	};
	pexpect_cidr(cidr, where);
	return cidr;
}

diag_t data_to_cidr(const void *data, size_t sizeof_data, unsigned prefix_len,
		       const struct ip_info *afi, ip_cidr *dst)
{
	*dst = unset_cidr;

	if (afi == NULL) {
		return diag("unknown CIDR address family");
	}

	/* accept longer! */
	if (sizeof_data < afi->ip_size) {
		return diag("minimum %s CIDR address buffer length is %zu, not %zu",
			    afi->ip_name, afi->ip_size, sizeof_data);
	}

	if (prefix_len > afi->mask_cnt) {
		return diag("maximum %s CIDR prefix length is %u, not %u",
			    afi->ip_name, afi->mask_cnt, prefix_len);
	}

	struct ip_bytes bytes = unset_ip_bytes;
	memcpy(bytes.byte, data, afi->ip_size);
	*dst = cidr_from_raw(HERE, afi, bytes, prefix_len);
	return NULL;
}

ip_cidr cidr_from_address(ip_address address)
{
	const struct ip_info *afi = address_info(address);
	if (afi == NULL) {
		return unset_cidr;
	}

	/* contains both routing-prefix and host-identifier */
	return cidr_from_raw(HERE, afi, address.bytes,
			     afi->mask_cnt/*32|128*/);

}

const struct ip_info *cidr_type(const ip_cidr *cidr)
{
	/* may return NULL */
	return ip_type(cidr);
}

const struct ip_info *cidr_info(const ip_cidr cidr)
{
	/* may return NULL */
	return ip_info(cidr);
}

ip_address cidr_address(const ip_cidr cidr)
{
	const struct ip_info *afi = cidr_info(cidr);
	if (afi == NULL) {
		return unset_address;
	}

	return address_from_raw(HERE, afi, cidr.bytes);
}

ip_address cidr_prefix(const ip_cidr cidr)
{
	const struct ip_info *afi = cidr_info(cidr);
	if (afi == NULL) {
		return unset_address;
	}
	return address_from_raw(HERE, afi,
				ip_bytes_blit(afi, cidr.bytes,
					      &keep_routing_prefix,
					      &clear_host_identifier,
					      cidr.prefix_len));
}

int cidr_prefix_len(const ip_cidr cidr)
{
	return cidr.prefix_len;
}

err_t cidr_check(const ip_cidr cidr)
{
	if (!cidr.ip.is_set) {
		return "unset";
	}

	const struct ip_info *afi = cidr_info(cidr);
	if (afi == NULL) {
		return "unknown address family";
	}

	/* https://en.wikipedia.org/wiki/IPv6_address#Special_addresses */
	/* ::/0 and/or 0.0.0.0/0 */
	if (cidr.prefix_len == 0 && thingeq(cidr.bytes, unset_ip_bytes)) {
		return "default route (no specific route)";
	}

	if (thingeq(cidr.bytes, unset_ip_bytes)) {
		return "unspecified address";
	}

	return NULL;
}

bool cidr_is_specified(const ip_cidr cidr)
{
	return cidr_check(cidr) == NULL;
}

shunk_t cidr_as_shunk(const ip_cidr *cidr)
{
	const struct ip_info *afi = cidr_type(cidr);
	if (afi == NULL) {
		return null_shunk;
	}

	return shunk2(&cidr->bytes, afi->ip_size);
}

chunk_t cidr_as_chunk(ip_cidr *cidr)
{
	const struct ip_info *afi = cidr_type(cidr);
	if (afi == NULL) {
		/* NULL+unset+unknown */
		return empty_chunk;
	}

	return chunk2(&cidr->bytes, afi->ip_size);
}

size_t jam_cidr(struct jambuf *buf, const ip_cidr *cidr)
{
	const struct ip_info *afi;
	size_t s = jam_invalid_ip(buf, "cidr", cidr, &afi);
	if (s > 0) {
		return s;
	}

	ip_address sa = cidr_address(*cidr);
	s += jam_address(buf, &sa); /* sensitive? */
	s += jam(buf, "/%u", cidr->prefix_len);
	return s;
}

const char *str_cidr(const ip_cidr *cidr, cidr_buf *out)
{
	struct jambuf buf = ARRAY_AS_JAMBUF(out->buf);
	jam_cidr(&buf, cidr);
	return out->buf;
}

void pexpect_cidr(const ip_cidr cidr, where_t where)
{
	if (cidr.ip.is_set == false ||
	    cidr.ip.version == 0) {
		llog_pexpect(&global_logger, where, "invalid "PRI_CIDR, pri_cidr(cidr));
	}
}

bool cidr_eq_cidr(const ip_cidr l, const ip_cidr r)
{
	bool l_set = cidr_is_specified(l);
	bool r_set = cidr_is_specified(r);

	if (! l_set && ! r_set) {
		/* unset/NULL addresses are equal */
		return true;
	}
	if (! l_set || ! r_set) {
		return false;
	}
	/* must compare individual fields */
	return (l.ip.version == r.ip.version &&
		l.prefix_len == r.prefix_len &&
		thingeq(l.bytes, r.bytes));
}
