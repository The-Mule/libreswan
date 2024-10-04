/* error logging functions, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2005-2007 Michael Richardson
 * Copyright (C) 2006-2010 Bart Trojanowski
 * Copyright (C) 2008-2012 Paul Wouters
 * Copyright (C) 2008-2010 David McCullough.
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013,2015 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2017-2020 Andrew Cagney <cagney@gnu.org>
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

#include <pthread.h>    /* Must be the first include file; XXX: why? */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>
#include <unistd.h>

#include "defs.h"
#include "log.h"
#include "connections.h"
#include "state.h"
#include "kernel.h"	/* for kernel_ops */
#include "timer.h"
#include "ip_endpoint.h"
#include "impair.h"
#include "demux.h"	/* for struct msg_digest */
#include "pending.h"
#include "show.h"

static struct fd *logger_fd(const struct logger *logger);
static void log_raw(int severity, const char *prefix, struct jambuf *buf);

static struct log_param log_param;	/* set during startup */

bool log_to_audit = false;

static FILE *pluto_log_file = NULL;	/* either a real file or stderr */

char *pluto_stats_binary = NULL;

/*
 * Initialization of the real logger.
 *
 * Caller is using a scratch logger and relying on stderr being open.
 */

struct logger *init_log(const char *progname)
{
	pluto_log_file = stderr;
	return string_logger(HERE, "%s", progname);
}

void switch_log(struct log_param param, struct logger **logger)
{
	/* save parameters */
	log_param = param;

	/*
	 * NOTE: Can't touch global PLUTO_LOG_FILE as it is in use.
	 * Hence save the new log file in LOG_FILE and switch at the
	 * end.
	 */
	PASSERT((*logger), pluto_log_file == stderr);
	FILE *log_file = NULL;

	if (param.log_to_file != NULL) {
		log_file = fopen(param.log_to_file, param.append ? "a" : "w");
		if (log_file == NULL) {
			llog_errno(RC_LOG, (*logger), errno,
				   "cannot open logfile '%s':", param.log_to_file);
			/* keep logging but to stdout! */
			pfree(param.log_to_file);
			param.log_to_file = NULL;
			param.log_to_stderr = true;
		} else {
			/*
			 * buffer by line: should be faster that no
			 * buffering; and yet safe since each message
			 * is written from a buffer as a line.
			 */
			setvbuf(log_file, NULL, _IOLBF, 0);
		}
	}

	if (log_file == NULL && param.log_to_stderr) {
		log_file = stderr;
		setbuf(log_file, NULL);
	}

	if (log_file == NULL) {
		openlog("pluto", LOG_CONS | LOG_NDELAY | LOG_PID, LOG_AUTHPRIV);
	}

	/* finally; shutdown stderr */
	if (log_file != stderr) {
		close(STDERR_FILENO); /*stderr*/
		/* stdin points at /dev/null from earlier */
		PASSERT((*logger), dup2(0, STDERR_FILENO) == STDERR_FILENO);
	}

	/*
	 * Finally switch.
	 *
	 * Close the tmp logger (need to fake a memory allocation so
	 * that the refcnt checker doesn't get confused) and update
	 * the log file and direct LOGGER at GLOBAL_LOGGER.
	 */
	dbg_alloc("logger", logger, HERE);
	free_logger(logger, HERE);
	*logger = &global_logger;
	pluto_log_file = log_file;
}

/*
 * Wrap up the logic to decide if a particular output should occur.
 * The compiler will likely inline these.
 */

static void jambuf_to_whack(struct jambuf *buf, const struct fd *whackfd, enum rc_type rc)
{
	/*
	 * XXX: use iovec as it's easier than trying to deal with
	 * truncation while still ensuring that the message is
	 * terminated with a '\n' (this isn't a performance thing, it
	 * just replaces local memory moves with kernel equivalent).
	 */

	/* 'NNN ' */
	char prefix[10];/*65535+200*/
	int prefix_len = snprintf(prefix, sizeof(prefix), "%03u ", rc);
	passert(prefix_len >= 0 && (unsigned) prefix_len < sizeof(prefix));

	/* message, not including trailing '\0' */
	shunk_t message = jambuf_as_shunk(buf);

	/* NL */
	char nl = '\n';

	struct iovec iov[] = {
		{ .iov_base = prefix, .iov_len = prefix_len, },
		/* need to cast away const :-( */
		{ .iov_base = (void*)message.ptr, .iov_len = message.len, },
		{ .iov_base = &nl, .iov_len = sizeof(nl), },
	};
	struct msghdr msg = {
		.msg_iov = iov,
		.msg_iovlen = elemsof(iov),
	};

	/* write to whack socket, but suppress possible SIGPIPE */
	ssize_t s = fd_sendmsg(whackfd, &msg, MSG_NOSIGNAL);
	if (s < 0) {
		/* probably the other end hit cntrl-c */
		JAMBUF(buf) {
			jam(buf, "whack error: ");
			jam_errno(buf, (-(int)s));
			/* not whack */
			log_raw(LOG_WARNING, "", buf);
		}
	}
}

/*
 * Interactive input from the whack user, using current whack_fd
 */
bool whack_prompt_for(struct state *st,
		      const char *prompt,
		      bool echo, char *ansbuf, size_t ansbuf_len)
{
	/* find an fd */
	struct fd *whack_fd = logger_fd(st->logger);
	if (whack_fd == NULL) {
		log_state(RC_LOG, st,
			  "XAUTH password requested, but no file descriptor available for prompt");
		return false;
	}

	ldbg(st->logger, "prompting whack for %s using "PRI_FD,
	     prompt, pri_fd(whack_fd));

	JAMBUF(buf) {
		jam_logger_prefix(buf, st->logger);
		/* the real message */
		jam(buf, "prompt for %s:", prompt);
		jambuf_to_whack(buf, whack_fd,
				echo ? RC_USERPROMPT : RC_ENTERSECRET);
	}

	ssize_t n = fd_read(whack_fd, ansbuf, ansbuf_len);
	if (n < 0) {
		llog_errno(RC_LOG, st->logger, (-(int)n),
			   "read(whackfd) failed: ");
		return false;
	}

	if (n == 0) {
		log_state(RC_LOG, st, "no %s entered, aborted", prompt);
		return false;
	}

	ansbuf[ansbuf_len - 1] = '\0'; /* ensure buffer is NULL terminated */
	return true;
}

static void log_raw(int severity, const char *prefix, struct jambuf *buf)
{
	/* assume there's a logging prefix; normally there is */
	struct realtm t = local_realtime(realnow());
	if (pluto_log_file != NULL) {
		if (log_param.log_with_timestamp) {
			char now[34] = "";
			strftime(now, sizeof(now), "%b %e %T", &t.tm);
			fprintf(pluto_log_file, "%s.%06ld: %s%s\n",
				now, t.microsec, prefix, buf->array);
		} else {
			fprintf(pluto_log_file, "%s%s\n", prefix, buf->array);
		}
	} else {
		syslog(severity, "%s%s", prefix, buf->array);
	}
	/* not whack */
}

void free_log(void)
{
	pfreeany(log_param.log_to_file);
}

void close_log(void)
{
	/*
	 * XXX: can't trust log_param.log_to_file as may have already
	 * been freed.
	 */
	if (pluto_log_file != NULL) {
		if (pluto_log_file != stderr) {
			fclose(pluto_log_file);
		}
		pluto_log_file = NULL;
	} else {
		closelog();
	}
}

void show_log(struct show *s)
{
	SHOW_JAMBUF(s, buf) {
		jam(buf, "logfile='%s'", (log_param.log_to_file ? log_param.log_to_file :
					  log_param.log_to_stderr ? "<stderr>" :
					  "<syslog>"));
		jam_string(buf, ", ");
		jam(buf, "logappend=%s", bool_str(log_param.append));
		jam_string(buf, ", ");
		jam(buf, "logip=%s", bool_str(log_ip));
		jam_string(buf, ", ");
		jam(buf, "audit-log=%s", bool_str(log_to_audit));
	}
}


void set_debugging(lset_t deb)
{
	cur_debugging = deb;
}

static void log_whacks(enum rc_type rc, const struct logger *logger, struct jambuf *buf)
{
	for (unsigned i = 0; i < elemsof(logger->whackfd); i++) {
		if (logger->whackfd[i] == NULL) {
			continue;
		}
		jambuf_to_whack(buf, logger->whackfd[i], rc);
	}
}

void jambuf_to_logger(struct jambuf *buf, const struct logger *logger, lset_t rc_flags)
{
	enum rc_type rc = (rc_flags & RC_MASK);
	enum stream stream = (rc_flags & STREAM_MASK);
	switch (stream) {
	case DEBUG_STREAM:
		log_raw(LOG_DEBUG, "", buf);
		return;
	case ALL_STREAMS:
		log_raw(LOG_WARNING, "", buf);
		log_whacks(rc, logger, buf);
		return;
	case LOG_STREAM:
		log_raw(LOG_WARNING, "", buf);
		return;
	case WHACK_STREAM:
		if (DBGP(DBG_BASE)) {
			log_raw(LOG_DEBUG, "|] ", buf);
		}
		log_whacks(rc, logger, buf);
		return;
	case ERROR_STREAM:
	case PEXPECT_STREAM:
	case FATAL_STREAM:
		log_raw(LOG_ERR, "", buf);
		log_whacks(rc, logger, buf);
		return;
	case PASSERT_STREAM:
		log_raw(LOG_ERR, "", buf);
		log_whacks(rc, logger, buf);
		return; /*abort();*/
	case NO_STREAM:
		/*
		 * XXX: Like writing to /dev/null - go through the
		 * motions but with no result.  Code really really
		 * should not call this function with this flag.
		 */
		return;
	}
	abort(); /* not bad_case(stream) as recursive */
}

const struct logger_object_vec logger_global_vec = {
	.name = "global",
	.jam_object_prefix = jam_object_prefix_none,
	.free_object = false,
};

struct logger logger_from(struct logger *global, const ip_endpoint *from)
{
	struct logger logger = {
		.where = HERE,
		.object = from,
		.object_vec = &logger_from_vec,
	};
	struct fd **fd = logger.whackfd;
	FOR_EACH_ELEMENT(gfd, global->whackfd) {
		if (*gfd != NULL) {
			*fd++ = *gfd;
		}
	}
	return logger;
}

static size_t jam_from_prefix(struct jambuf *buf, const void *object)
{
	size_t s = 0;
	if (!in_main_thread()) {
		s += jam(buf, PEXPECT_PREFIX"%s in main thread", __func__);
	} else if (object == NULL) {
		s += jam(buf, PEXPECT_PREFIX"%s NULL", __func__);
	} else {
		const ip_endpoint *from = object;
		/* peer's IP address */
		if (endpoint_protocol(*from) == &ip_protocol_tcp) {
			s += jam(buf, "connection from ");
		} else {
			s += jam(buf, "packet from ");
		}
		s += jam_endpoint_sensitive(buf, from);
	}
	return s;
}

const struct logger_object_vec logger_from_vec = {
	.name = "from",
	.jam_object_prefix = jam_from_prefix,
	.free_object = false,
};

static size_t jam_message_prefix(struct jambuf *buf, const void *object)
{
	size_t s = 0;
	if (!in_main_thread()) {
		s += jam(buf, PEXPECT_PREFIX"%s in main thread", __func__);
	} else if (object == NULL) {
		s += jam(buf, PEXPECT_PREFIX"%s NULL", __func__);
	} else {
		const struct msg_digest *md = object;
		s += jam_from_prefix(buf, &md->sender);
	}
	return s;
}

const struct logger_object_vec logger_message_vec = {
	.name = "message",
	.jam_object_prefix = jam_message_prefix,
	.free_object = false,
};

static size_t jam_connection_prefix(struct jambuf *buf, const void *object)
{
	size_t s = 0;
	if (!in_main_thread()) {
		s += jam(buf, PEXPECT_PREFIX"%s in main thread",
			 __func__);
	} else if (object == NULL) {
		s += jam(buf, PEXPECT_PREFIX"%s NULL", __func__);
	} else {
		const struct connection *c = object;
		s += jam_connection(buf, c);
	}
	return s;
}

const struct logger_object_vec logger_connection_vec = {
	.name = "connection",
	.jam_object_prefix = jam_connection_prefix,
	.free_object = false,
};

size_t jam_state(struct jambuf *buf, const struct state *st)
{
	size_t s = 0;
	/*
	 * XXX: When delete state() triggers a delete
	 * connection, this can be NULL.
	 */
	if (st->st_connection != NULL) {
		s += jam_connection(buf, st->st_connection);
	}
	/* state number */
	s += jam(buf, " "PRI_SO, pri_so(st->st_serialno));
	return s;
}

static size_t jam_state_prefix(struct jambuf *buf, const void *object)
{
	size_t s = 0;
	if (!in_main_thread()) {
		s += jam(buf, PEXPECT_PREFIX"%s in main thread", __func__);
	} else if (object == NULL) {
		s += jam(buf, PEXPECT_PREFIX"%s NULL", __func__);
	} else {
		const struct state *st = object;
		s += jam_state(buf, st);
		/* state name */
		if (DBGP(DBG_ADD_PREFIX)) {
			s += jam(buf, " ");
			s += jam_string(buf, st->st_state->short_name);
		}
	}
	return s;
}

const struct logger_object_vec logger_state_vec = {
	.name = "state",
	.jam_object_prefix = jam_state_prefix,
	.free_object = false,
};

static size_t jam_string_prefix(struct jambuf *buf, const void *object)
{
	const char *string = object;
	return jam_string(buf, string);
}

static const struct logger_object_vec logger_string_vec = {
	.name = "string(never-suppress)",
	.jam_object_prefix = jam_string_prefix,
	.free_object = true,
};

struct logger *alloc_logger(void *object, const struct logger_object_vec *vec,
			    lset_t debugging, where_t where)
{
	struct logger logger = {
		.object = object,
		.object_vec = vec,
		.where = where,
		.debugging = debugging,
	};
	struct logger *l = clone_thing(logger, "logger");
	dbg_alloc("alloc logger", l, where);
	return l;
}

struct logger *clone_logger(const struct logger *stack, where_t where)
{
	/*
	 * Convert the dynamicically generated OBJECT prefix into an
	 * unchanging string.  This way the prefix can be safely
	 * accessed on a helper thread.
	 *
	 * Use str_prefix() so that the prefix doesn't include
	 * ":_" as added by jam_logger_prefix().
	 */
	prefix_buf pb;
	const char *prefix = str_prefix(stack, &pb);
	/* construct the clone */
	struct logger heap = {
		.where = stack->where,
		.object_vec = &logger_string_vec,
		.object = clone_str(prefix, "heap logger prefix"),
		.debugging = stack->debugging,
	};
	/* and clone it */
	struct logger *l = clone_thing(heap, "heap logger");
	dbg_alloc("clone logger", l, where);
	/* copy over whacks */
	unsigned h = 0;
	FOR_EACH_ELEMENT(sfd, stack->whackfd) {
		if (*sfd != NULL) {
			pdbg(l, "attach whack "PRI_FD" to logger %p slot %u "PRI_WHERE,
			     pri_fd(*sfd), l, h, pri_where(where));
			l->whackfd[h++] = fd_addref_where(*sfd, where);
		}
	}
	return l;
}

struct logger *string_logger(where_t where, const char *fmt, ...)
{
	/*
	 * Convert the dynamicically generated OBJECT prefix into an
	 * unchanging string.  This way the prefix can be safely
	 * accessed on a helper thread.
	 */
	char prefix[LOG_WIDTH];
	{
		struct jambuf prefix_buf = ARRAY_AS_JAMBUF(prefix);
		va_list ap;
		va_start(ap, fmt);
		jam_va_list(&prefix_buf, fmt, ap);
		va_end(ap);
	}
	/* construct the clone */
	struct logger logger = {
		.where = where,
		.object_vec = &logger_string_vec,
		.object = clone_str(prefix, "string logger prefix"),
	};
	/* and clone it */
	struct logger *l = clone_thing(logger, "string logger");
	dbg_alloc("string logger", l, where);
	return l;
}

void release_whack(struct logger *logger, where_t where)
{
	bool whacked = false;
	for (unsigned i = 0; i < elemsof(logger->whackfd); i++) {
		if (logger->whackfd[i] != NULL) {
			whacked = true;
			pdbg(logger, "detach whack "PRI_FD" from logger %p slot %u "PRI_WHERE,
			     pri_fd(logger->whackfd[i]), logger, i, pri_where(where));
			fd_delref_where(&logger->whackfd[i], where);
		}
	}
	if (!whacked) {
		pdbg(logger, "releasing whack (but there are none) "PRI_WHERE,
		     pri_where(where));
	}
}

void free_logger(struct logger **logp, where_t where)
{
	release_whack(*logp, where);
	/*
	 * For instance the string allocated by clone_logger().  More
	 * complex objects are freed by other means.
	 */
	dbg_free("logger", *logp, where);
	if ((*logp)->object_vec->free_object) {
		pfree((void*) (*logp)->object);
	}
	/* done */
	pfree(*logp);
	*logp = NULL;
}

void log_state(lset_t rc_flags, const struct state *st,
	       const char *msg, ...)
{
	va_list ap;
	va_start(ap, msg);
	if (pexpect((st) != NULL) &&
	    pexpect(in_main_thread())) {
		llog_va_list(rc_flags, st->logger, msg, ap);
	} else {
		/* still get the message out */
		llog_va_list(rc_flags, &global_logger, msg, ap);

	}
	va_end(ap);
}

static struct fd *logger_fd(const struct logger *logger)
{
	/* find a whack */
	FOR_EACH_ELEMENT(fdp, logger->whackfd) {
		if (*fdp != NULL) {
			return *fdp;
		}
	}
	return NULL;
}

bool whack_attached(const struct logger *logger)
{
	return logger_fd(logger) != NULL;
}

bool same_whack(const struct logger *lhs, const struct logger *rhs)
{
	FOR_EACH_ELEMENT(lfd, lhs->whackfd) {
		if (*lfd == NULL) {
			continue;
		}
		FOR_EACH_ELEMENT(rfd, rhs->whackfd) {
			if (*lfd == *rfd) {
				return true;
			}
		}
	}
	return false;
}

static void attach_fd_where(struct logger *dst, struct fd *src_fd, where_t where)
{
	/* do no harm? */
	if (src_fd == NULL) {
		pdbg(dst, "no whack to attach");
		return;
	}

	/* already attached? */
	for (unsigned i = 0; i < elemsof(dst->whackfd); i++) {
		if (dst->whackfd[i] == src_fd) {
			/* already attached */
			pdbg(dst, "whack "PRI_FD" already attached to logger %p slot %u",
			     pri_fd(src_fd), dst, i);
			return;
		}
	}

	/* attach to spare slot */
	for (unsigned i = 0; i < elemsof(dst->whackfd); i++) {
		if (dst->whackfd[i] == NULL) {
			dst->whackfd[i] = fd_addref_where(src_fd, where);
			pdbg(dst, "attach whack "PRI_FD" to empty logger %p slot %u",
			     pri_fd(src_fd), dst, i);
			return;
		}
	}

	/* replace first aka global */
	pdbg(dst, "attach whack "PRI_FD" to logger %p slot 0 (global)",
	     pri_fd(src_fd), dst);
	fd_delref_where(dst->whackfd, where);
	dst->whackfd[0] = fd_addref_where(src_fd, where);
}

void whack_attach_where(struct logger *dst, const struct logger *src, where_t where)
{
	if (src == dst) {
		return;
	}
	attach_fd_where(dst, logger_fd(src), where);
}

void md_attach_where(struct msg_digest *md, const struct logger *src, where_t where)
{
	whack_attach_where(md->logger, src, where);
}

void connection_attach_where(struct connection *c, const struct logger *src, where_t where)
{
	whack_attach_where(c->logger, src, where);
}

void state_attach_where(struct state *st, const struct logger *src, where_t where)
{
	whack_attach_where(st->logger, src, where);
}

void whack_detach_where(struct logger *dst, const struct logger *src, where_t where)
{
	if (src == dst) {
		pdbg(dst, "don't detach our own logger "PRI_WHERE, pri_where(where));
		return;
	}

	/* find a whack to detach */
	struct fd *src_fd = logger_fd(src);
	if (src_fd == NULL) {
		pdbg(dst, "no whack to detach "PRI_WHERE, pri_where(where));
		return;
	}

	/* find where it is attached */
	for (unsigned i = 0; i < elemsof(dst->whackfd); i++) {
		if (dst->whackfd[i] == src_fd) {
			pdbg(dst, "detach whack "PRI_FD" from logger %p slot %u "PRI_WHERE,
			     pri_fd(src_fd), dst, i, pri_where(where));
			fd_delref_where(&dst->whackfd[i], where);
			return;
		}
	}
}

void md_detach_where(struct msg_digest *md, const struct logger *src, where_t where)
{
	if (md == NULL) {
		return;
	}
	whack_detach_where(md->logger, src, where);
}

void connection_detach_where(struct connection *c, const struct logger *src, where_t where)
{
	if (c == NULL) {
		return;
	}
	whack_detach_where(c->logger, src, where);
}

void state_detach_where(struct state *st, const struct logger *src, where_t where)
{
	if (st == NULL) {
		return;
	}
	whack_detach_where(st->logger, src, where);
}
