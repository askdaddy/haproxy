/*
 * Stick table synchro management.
 *
 * Copyright 2010 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/time.h>

#include <types/global.h>
#include <types/listener.h>
#include <types/obj_type.h>
#include <types/peers.h>

#include <proto/acl.h>
#include <proto/channel.h>
#include <proto/fd.h>
#include <proto/frontend.h>
#include <proto/log.h>
#include <proto/hdr_idx.h>
#include <proto/proto_tcp.h>
#include <proto/proto_http.h>
#include <proto/proxy.h>
#include <proto/session.h>
#include <proto/stream.h>
#include <proto/signal.h>
#include <proto/stick_table.h>
#include <proto/stream_interface.h>
#include <proto/task.h>


/*******************************/
/* Current peer learning state */
/*******************************/

/******************************/
/* Current table resync state */
/******************************/
#define	SHTABLE_F_RESYNC_LOCAL		0x00000001 /* Learn from local finished or no more needed */
#define	SHTABLE_F_RESYNC_REMOTE		0x00000002 /* Learn from remote finished or no more needed */
#define	SHTABLE_F_RESYNC_ASSIGN		0x00000004 /* A peer was assigned to learn our lesson */
#define	SHTABLE_F_RESYNC_PROCESS	0x00000008 /* The assigned peer was requested for resync */
#define	SHTABLE_F_DONOTSTOP		0x00010000 /* Main table sync task block process during soft stop
						      to push data to new process */

#define	SHTABLE_RESYNC_STATEMASK	(SHTABLE_F_RESYNC_LOCAL|SHTABLE_F_RESYNC_REMOTE)
#define	SHTABLE_RESYNC_FROMLOCAL	0x00000000
#define	SHTABLE_RESYNC_FROMREMOTE	SHTABLE_F_RESYNC_LOCAL
#define	SHTABLE_RESYNC_FINISHED		(SHTABLE_F_RESYNC_LOCAL|SHTABLE_F_RESYNC_REMOTE)

/******************************/
/* Remote peer teaching state */
/******************************/
#define	PEER_F_TEACH_PROCESS		0x00000001 /* Teach a lesson to current peer */
#define	PEER_F_TEACH_STAGE1		0x00000002 /* Teach state 1 complete */
#define	PEER_F_TEACH_STAGE2		0x00000004 /* Teach stage 2 complete */
#define	PEER_F_TEACH_FINISHED		0x00000008 /* Teach conclude, (wait for confirm) */
#define	PEER_F_TEACH_COMPLETE		0x00000010 /* All that we know already taught to current peer, used only for a local peer */
#define	PEER_F_LEARN_ASSIGN		0x00000100 /* Current peer was assigned for a lesson */
#define	PEER_F_LEARN_NOTUP2DATE		0x00000200 /* Learn from peer finished but peer is not up to date */

#define	PEER_TEACH_RESET		~(PEER_F_TEACH_PROCESS|PEER_F_TEACH_STAGE1|PEER_F_TEACH_STAGE2|PEER_F_TEACH_FINISHED) /* PEER_F_TEACH_COMPLETE should never be reset */
#define	PEER_LEARN_RESET		~(PEER_F_LEARN_ASSIGN|PEER_F_LEARN_NOTUP2DATE)


/**********************************/
/* Peer Session IO handler states */
/**********************************/

enum {
	PEER_SESS_ST_ACCEPT = 0,     /* Initial state for session create by an accept, must be zero! */
	PEER_SESS_ST_GETVERSION,     /* Validate supported protocol version */
	PEER_SESS_ST_GETHOST,        /* Validate host ID correspond to local host id */
	PEER_SESS_ST_GETPEER,        /* Validate peer ID correspond to a known remote peer id */
	PEER_SESS_ST_GETTABLE,       /* Search into registered table for a table with same id and validate type and size */
	/* after this point, data were possibly exchanged */
	PEER_SESS_ST_SENDSUCCESS,    /* Send ret code 200 (success) and wait for message */
	PEER_SESS_ST_CONNECT,        /* Initial state for session create on a connect, push presentation into buffer */
	PEER_SESS_ST_GETSTATUS,      /* Wait for the welcome message */
	PEER_SESS_ST_WAITMSG,        /* Wait for data messages */
	PEER_SESS_ST_EXIT,           /* Exit with status code */
	PEER_SESS_ST_END,            /* Killed session */
};

/***************************************************/
/* Peer Session status code - part of the protocol */
/***************************************************/

#define	PEER_SESS_SC_CONNECTCODE	100 /* connect in progress */
#define	PEER_SESS_SC_CONNECTEDCODE	110 /* tcp connect success */

#define	PEER_SESS_SC_SUCCESSCODE	200 /* accept or connect successful */

#define	PEER_SESS_SC_TRYAGAIN		300 /* try again later */

#define	PEER_SESS_SC_ERRPROTO		501 /* error protocol */
#define	PEER_SESS_SC_ERRVERSION		502 /* unknown protocol version */
#define	PEER_SESS_SC_ERRHOST		503 /* bad host name */
#define	PEER_SESS_SC_ERRPEER		504 /* unknown peer */
#define	PEER_SESS_SC_ERRTYPE		505 /* table key type mismatch */
#define	PEER_SESS_SC_ERRSIZE		506 /* table key size mismatch */
#define	PEER_SESS_SC_ERRTABLE		507 /* unknown table */

#define PEER_SESSION_PROTO_NAME         "HAProxyS"

struct peers *peers = NULL;
static void peer_session_forceshutdown(struct stream * stream);


/*
 * This prepare the data update message of the stick session <ts>, <ps> is the the peer session
 * where the data going to be pushed, <msg> is a buffer of <size> to recieve data message content
 */
static int peer_prepare_datamsg(struct stksess *ts, struct peer_session *ps, char *msg, size_t size)
{
	uint32_t netinteger;
	int len;
	/* construct message */
	if (ps->lastpush && ts->upd.key > ps->lastpush && (ts->upd.key - ps->lastpush) <= 127) {
		msg[0] = 0x80 + ts->upd.key - ps->lastpush;
		len = sizeof(char);
	}
	else {
		msg[0] = 'D';
		netinteger = htonl(ts->upd.key);
		memcpy(&msg[sizeof(char)], &netinteger, sizeof(netinteger));
		len = sizeof(char) + sizeof(netinteger);
	}

	if (ps->table->table->type == STKTABLE_TYPE_STRING) {
		int stlen = strlen((char *)ts->key.key);

		netinteger = htonl(strlen((char *)ts->key.key));
		memcpy(&msg[len], &netinteger, sizeof(netinteger));
		memcpy(&msg[len+sizeof(netinteger)], ts->key.key, stlen);
		len += sizeof(netinteger) + stlen;

	}
	else if (ps->table->table->type == STKTABLE_TYPE_INTEGER) {
		netinteger = htonl(*((uint32_t *)ts->key.key));
		memcpy(&msg[len], &netinteger, sizeof(netinteger));
		len += sizeof(netinteger);
	}
	else {
		memcpy(&msg[len], ts->key.key, ps->table->table->key_size);
		len += ps->table->table->key_size;
	}

	if (stktable_data_ptr(ps->table->table, ts, STKTABLE_DT_SERVER_ID))
		netinteger = htonl(stktable_data_cast(stktable_data_ptr(ps->table->table, ts, STKTABLE_DT_SERVER_ID), server_id));
	else
		netinteger = 0;

	memcpy(&msg[len], &netinteger , sizeof(netinteger));
	len += sizeof(netinteger);

	return len;
}


/*
 * Callback to release a session with a peer
 */
static void peer_session_release(struct stream_interface *si)
{
	struct stream *s = si_strm(si);
	struct appctx *appctx = objt_appctx(si->end);
	struct peer_session *ps = (struct peer_session *)appctx->ctx.peers.ptr;

	/* appctx->ctx.peers.ptr is not a peer session */
	if (appctx->st0 < PEER_SESS_ST_SENDSUCCESS)
		return;

	/* peer session identified */
	if (ps) {
		if (ps->stream == s) {
			ps->stream = NULL;
			if (ps->flags & PEER_F_LEARN_ASSIGN) {
				/* unassign current peer for learning */
				ps->flags &= ~(PEER_F_LEARN_ASSIGN);
				ps->table->flags &= ~(SHTABLE_F_RESYNC_ASSIGN|SHTABLE_F_RESYNC_PROCESS);

				/* reschedule a resync */
				ps->table->resync_timeout = tick_add(now_ms, MS_TO_TICKS(5000));
			}
			/* reset teaching and learning flags to 0 */
			ps->flags &= PEER_TEACH_RESET;
			ps->flags &= PEER_LEARN_RESET;
		}
		task_wakeup(ps->table->sync_task, TASK_WOKEN_MSG);
	}
}


/*
 * IO Handler to handle message exchance with a peer
 */
static void peer_io_handler(struct stream_interface *si)
{
	struct stream *s = si_strm(si);
	struct peers *curpeers = (struct peers *)strm_fe(s)->parent;
	struct appctx *appctx = objt_appctx(si->end);
	int reql = 0;
	int repl = 0;

	while (1) {
switchstate:
		switch(appctx->st0) {
			case PEER_SESS_ST_ACCEPT:
				appctx->ctx.peers.ptr = NULL;
				appctx->st0 = PEER_SESS_ST_GETVERSION;
				/* fall through */
			case PEER_SESS_ST_GETVERSION:
				reql = bo_getline(si_oc(si), trash.str, trash.size);
				if (reql <= 0) { /* closed or EOL not found */
					if (reql == 0)
						goto out;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				if (trash.str[reql-1] != '\n') {
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				else if (reql > 1 && (trash.str[reql-2] == '\r'))
					trash.str[reql-2] = 0;
				else
					trash.str[reql-1] = 0;

				bo_skip(si_oc(si), reql);

				/* test version */
				if (strcmp(PEER_SESSION_PROTO_NAME " 1.0", trash.str) != 0) {
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRVERSION;
					/* test protocol */
					if (strncmp(PEER_SESSION_PROTO_NAME " ", trash.str, strlen(PEER_SESSION_PROTO_NAME)+1) != 0)
						appctx->st1 = PEER_SESS_SC_ERRPROTO;
					goto switchstate;
				}

				appctx->st0 = PEER_SESS_ST_GETHOST;
				/* fall through */
			case PEER_SESS_ST_GETHOST:
				reql = bo_getline(si_oc(si), trash.str, trash.size);
				if (reql <= 0) { /* closed or EOL not found */
					if (reql == 0)
						goto out;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				if (trash.str[reql-1] != '\n') {
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				else if (reql > 1 && (trash.str[reql-2] == '\r'))
					trash.str[reql-2] = 0;
				else
					trash.str[reql-1] = 0;

				bo_skip(si_oc(si), reql);

				/* test hostname match */
				if (strcmp(localpeer, trash.str) != 0) {
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRHOST;
					goto switchstate;
				}

				appctx->st0 = PEER_SESS_ST_GETPEER;
				/* fall through */
			case PEER_SESS_ST_GETPEER: {
				struct peer *curpeer;
				char *p;
				reql = bo_getline(si_oc(si), trash.str, trash.size);
				if (reql <= 0) { /* closed or EOL not found */
					if (reql == 0)
						goto out;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				if (trash.str[reql-1] != '\n') {
					/* Incomplete line, we quit */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				else if (reql > 1 && (trash.str[reql-2] == '\r'))
					trash.str[reql-2] = 0;
				else
					trash.str[reql-1] = 0;

				bo_skip(si_oc(si), reql);

				/* parse line "<peer name> <pid>" */
				p = strchr(trash.str, ' ');
				if (!p) {
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRPROTO;
					goto switchstate;
				}
				*p = 0;

				/* lookup known peer */
				for (curpeer = curpeers->remote; curpeer; curpeer = curpeer->next) {
					if (strcmp(curpeer->id, trash.str) == 0)
						break;
				}

				/* if unknown peer */
				if (!curpeer) {
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRPEER;
					goto switchstate;
				}

				appctx->ctx.peers.ptr = curpeer;
				appctx->st0 = PEER_SESS_ST_GETTABLE;
				/* fall through */
			}
			case PEER_SESS_ST_GETTABLE: {
				struct peer *curpeer = (struct peer *)appctx->ctx.peers.ptr;
				struct shared_table *st;
				struct peer_session *ps = NULL;
				unsigned long key_type;
				size_t key_size;
				char *p;

				reql = bo_getline(si_oc(si), trash.str, trash.size);
				if (reql <= 0) { /* closed or EOL not found */
					if (reql == 0)
						goto out;
					appctx->ctx.peers.ptr = NULL;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				/* Re init appctx->ctx.peers.ptr to null, to handle correctly a release case */
				appctx->ctx.peers.ptr = NULL;

				if (trash.str[reql-1] != '\n') {
					/* Incomplete line, we quit */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				else if (reql > 1 && (trash.str[reql-2] == '\r'))
					trash.str[reql-2] = 0;
				else
					trash.str[reql-1] = 0;

				bo_skip(si_oc(si), reql);

				/* Parse line "<table name> <type> <size>" */
				p = strchr(trash.str, ' ');
				if (!p) {
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRPROTO;
					goto switchstate;
				}
				*p = 0;
				key_type = (unsigned long)atol(p+1);

				p = strchr(p+1, ' ');
				if (!p) {
					appctx->ctx.peers.ptr = NULL;
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRPROTO;
					goto switchstate;
				}

				key_size = (size_t)atoi(p);
				for (st = curpeers->tables; st; st = st->next) {
					/* If table name matches */
					if (strcmp(st->table->id, trash.str) == 0) {
						/* Check key size mismatches, except for strings
						 * which may be truncated as long as they fit in
						 * a buffer.
						 */
						if (key_size != st->table->key_size &&
						    (key_type != STKTABLE_TYPE_STRING ||
						     1 + 4 + 4 + key_size - 1 >= trash.size)) {
							appctx->st0 = PEER_SESS_ST_EXIT;
							appctx->st1 = PEER_SESS_SC_ERRSIZE;
							goto switchstate;
						}

						/* If key type mismatches */
						if (key_type != st->table->type) {
							appctx->st0 = PEER_SESS_ST_EXIT;
							appctx->st1 = PEER_SESS_SC_ERRTYPE;
							goto switchstate;
						}

						/* lookup peer stream of current peer */
						for (ps = st->sessions; ps; ps = ps->next) {
							if (ps->peer == curpeer) {
								/* If stream already active, replaced by new one */
								if (ps->stream && ps->stream != s) {
									if (ps->peer->local) {
										/* Local connection, reply a retry */
										appctx->st0 = PEER_SESS_ST_EXIT;
										appctx->st1 = PEER_SESS_SC_TRYAGAIN;
										goto switchstate;
									}
									peer_session_forceshutdown(ps->stream);
								}
								ps->stream = s;
								break;
							}
						}
						break;
					}
				}

				/* If table not found */
				if (!st){
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRTABLE;
					goto switchstate;
				}

				/* If no peer session for current peer */
				if (!ps) {
					appctx->st0 = PEER_SESS_ST_EXIT;
					appctx->st1 = PEER_SESS_SC_ERRPEER;
					goto switchstate;
				}

				appctx->ctx.peers.ptr = ps;
				appctx->st0 = PEER_SESS_ST_SENDSUCCESS;
				/* fall through */
			}
			case PEER_SESS_ST_SENDSUCCESS: {
				struct peer_session *ps = (struct peer_session *)appctx->ctx.peers.ptr;

				repl = snprintf(trash.str, trash.size, "%d\n", PEER_SESS_SC_SUCCESSCODE);
				repl = bi_putblk(si_ic(si), trash.str, repl);
				if (repl <= 0) {
					if (repl == -1)
						goto full;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}

				/* Register status code */
				ps->statuscode = PEER_SESS_SC_SUCCESSCODE;

				/* Awake main task */
				task_wakeup(ps->table->sync_task, TASK_WOKEN_MSG);

				/* Init cursors */
				ps->teaching_origin =ps->lastpush = ps->lastack = ps->pushack = 0;
				ps->pushed = ps->update;

				/* Init confirm counter */
				ps->confirm = 0;

				/* reset teaching and learning flags to 0 */
				ps->flags &= PEER_TEACH_RESET;
				ps->flags &= PEER_LEARN_RESET;

				/* if current peer is local */
				if (ps->peer->local) {
					/* if table need resyncfrom local and no process assined  */
					if ((ps->table->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FROMLOCAL &&
					    !(ps->table->flags & SHTABLE_F_RESYNC_ASSIGN)) {
						/* assign local peer for a lesson, consider lesson already requested */
						ps->flags |= PEER_F_LEARN_ASSIGN;
						ps->table->flags |= (SHTABLE_F_RESYNC_ASSIGN|SHTABLE_F_RESYNC_PROCESS);
					}

				}
				else if ((ps->table->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FROMREMOTE &&
					 !(ps->table->flags & SHTABLE_F_RESYNC_ASSIGN)) {
					/* assign peer for a lesson  */
					ps->flags |= PEER_F_LEARN_ASSIGN;
					ps->table->flags |= SHTABLE_F_RESYNC_ASSIGN;
				}
				/* switch to waiting message state */
				appctx->st0 = PEER_SESS_ST_WAITMSG;
				goto switchstate;
			}
			case PEER_SESS_ST_CONNECT: {
				struct peer_session *ps = (struct peer_session *)appctx->ctx.peers.ptr;

				/* Send headers */
				repl = snprintf(trash.str, trash.size,
				                PEER_SESSION_PROTO_NAME " 1.0\n%s\n%s %d\n%s %lu %d\n",
				                ps->peer->id,
				                localpeer,
				                (int)getpid(),
				                ps->table->table->id,
				                ps->table->table->type,
				                (int)ps->table->table->key_size);

				if (repl >= trash.size) {
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}

				repl = bi_putblk(si_ic(si), trash.str, repl);
				if (repl <= 0) {
					if (repl == -1)
						goto full;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}

				/* switch to the waiting statuscode state */
				appctx->st0 = PEER_SESS_ST_GETSTATUS;
				/* fall through */
			}
			case PEER_SESS_ST_GETSTATUS: {
				struct peer_session *ps = (struct peer_session *)appctx->ctx.peers.ptr;

				if (si_ic(si)->flags & CF_WRITE_PARTIAL)
					ps->statuscode = PEER_SESS_SC_CONNECTEDCODE;

				reql = bo_getline(si_oc(si), trash.str, trash.size);
				if (reql <= 0) { /* closed or EOL not found */
					if (reql == 0)
						goto out;
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				if (trash.str[reql-1] != '\n') {
					/* Incomplete line, we quit */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				else if (reql > 1 && (trash.str[reql-2] == '\r'))
					trash.str[reql-2] = 0;
				else
					trash.str[reql-1] = 0;

				bo_skip(si_oc(si), reql);

				/* Register status code */
				ps->statuscode = atoi(trash.str);

				/* Awake main task */
				task_wakeup(ps->table->sync_task, TASK_WOKEN_MSG);

				/* If status code is success */
				if (ps->statuscode == PEER_SESS_SC_SUCCESSCODE) {
					/* Init cursors */
					ps->teaching_origin = ps->lastpush = ps->lastack = ps->pushack = 0;
					ps->pushed = ps->update;

					/* Init confirm counter */
					ps->confirm = 0;

					/* reset teaching and learning flags to 0 */
					ps->flags &= PEER_TEACH_RESET;
					ps->flags &= PEER_LEARN_RESET;

					/* If current peer is local */
					if (ps->peer->local) {
						/* Init cursors to push a resync */
						ps->teaching_origin = ps->pushed = ps->table->table->update;
						/* flag to start to teach lesson */
						ps->flags |= PEER_F_TEACH_PROCESS;

					}
					else if ((ps->table->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FROMREMOTE &&
					            !(ps->table->flags & SHTABLE_F_RESYNC_ASSIGN)) {
						/* If peer is remote and resync from remote is needed,
						   and no peer currently assigned */

						/* assign peer for a lesson */
						ps->flags |= PEER_F_LEARN_ASSIGN;
						ps->table->flags |= SHTABLE_F_RESYNC_ASSIGN;
					}

				}
				else {
					/* Status code is not success, abort */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}
				appctx->st0 = PEER_SESS_ST_WAITMSG;
				/* fall through */
			}
			case PEER_SESS_ST_WAITMSG: {
				struct peer_session *ps = (struct peer_session *)appctx->ctx.peers.ptr;
				struct stksess *ts, *newts = NULL;
				char c;
				int totl = 0;

				reql = bo_getblk(si_oc(si), (char *)&c, sizeof(c), totl);
				if (reql <= 0) /* closed or EOL not found */
					goto incomplete;

				totl += reql;

				if ((c & 0x80) || (c == 'D')) {
					/* Here we have data message */
					unsigned int pushack;
					int srvid;
					uint32_t netinteger;

					/* Compute update remote version */
					if (c & 0x80) {
						pushack = ps->pushack + (unsigned int)(c & 0x7F);
					}
					else {
						reql = bo_getblk(si_oc(si), (char *)&netinteger, sizeof(netinteger), totl);
						if (reql <= 0) /* closed or EOL not found */
							goto incomplete;

						totl += reql;
						pushack = ntohl(netinteger);
					}

					/* Read key. The string keys are read in two steps, the first step
					 * consists in reading whatever fits into the table directly into
					 * the pre-allocated key. The second step consists in simply
					 * draining all exceeding data. This can happen for example after a
					 * config reload with a smaller key size for the stick table than
					 * what was previously set, or when facing the impossibility to
					 * allocate a new stksess (for example when the table is full with
					 * "nopurge").
					 */
					if (ps->table->table->type == STKTABLE_TYPE_STRING) {
						unsigned int to_read, to_store;

						/* read size first */
						reql = bo_getblk(si_oc(si), (char *)&netinteger, sizeof(netinteger), totl);
						if (reql <= 0) /* closed or EOL not found */
							goto incomplete;

						totl += reql;

						to_store = 0;
						to_read = ntohl(netinteger);

						if (to_read + totl > si_ob(si)->size) {
							/* impossible to read a key this large, abort */
							reql = -1;
							goto incomplete;
						}

						newts = stksess_new(ps->table->table, NULL);
						if (newts)
							to_store = MIN(to_read, ps->table->table->key_size - 1);

						/* we read up to two blocks, the first one goes into the key,
						 * the rest is drained into the trash.
						 */
						if (to_store) {
							reql = bo_getblk(si_oc(si), (char *)newts->key.key, to_store, totl);
							if (reql <= 0) /* closed or incomplete */
								goto incomplete;
							newts->key.key[reql] = 0;
							totl += reql;
							to_read -= reql;
						}
						if (to_read) {
							reql = bo_getblk(si_oc(si), trash.str, to_read, totl);
							if (reql <= 0) /* closed or incomplete */
								goto incomplete;
							totl += reql;
						}
					}
					else if (ps->table->table->type == STKTABLE_TYPE_INTEGER) {
						reql = bo_getblk(si_oc(si), (char *)&netinteger, sizeof(netinteger), totl);
						if (reql <= 0) /* closed or EOL not found */
							goto incomplete;
						newts = stksess_new(ps->table->table, NULL);
						if (newts) {
							netinteger = ntohl(netinteger);
							memcpy(newts->key.key, &netinteger, sizeof(netinteger));
						}
						totl += reql;
					}
					else {
						/* type ip or binary */
						newts = stksess_new(ps->table->table, NULL);
						reql = bo_getblk(si_oc(si), newts ? (char *)newts->key.key : trash.str, ps->table->table->key_size, totl);
						if (reql <= 0) /* closed or EOL not found */
							goto incomplete;
						totl += reql;
					}

					/* read server id */
					reql = bo_getblk(si_oc(si), (char *)&netinteger, sizeof(netinteger), totl);
					if (reql <= 0) /* closed or EOL not found */
						goto incomplete;

					totl += reql;
					srvid = ntohl(netinteger);

					/* update entry */
					if (newts) {
						/* lookup for existing entry */
						ts = stktable_lookup(ps->table->table, newts);
						if (ts) {
							 /* the entry already exist, we can free ours */
							stktable_touch(ps->table->table, ts, 0);
							stksess_free(ps->table->table, newts);
							newts = NULL;
						}
						else {
							struct eb32_node *eb;

							/* create new entry */
							ts = stktable_store(ps->table->table, newts, 0);
							newts = NULL; /* don't reuse it */

							ts->upd.key= (++ps->table->table->update)+(2^31);
							eb = eb32_insert(&ps->table->table->updates, &ts->upd);
							if (eb != &ts->upd) {
								eb32_delete(eb);
								eb32_insert(&ps->table->table->updates, &ts->upd);
							}
						}

						/* update entry */
						if (srvid && stktable_data_ptr(ps->table->table, ts, STKTABLE_DT_SERVER_ID))
							stktable_data_cast(stktable_data_ptr(ps->table->table, ts, STKTABLE_DT_SERVER_ID), server_id) = srvid;
						ps->pushack = pushack;
					}

				}
				else if (c == 'R') {
					/* Reset message: remote need resync */

					/* reinit counters for a resync */
					ps->lastpush = 0;
					ps->teaching_origin = ps->pushed = ps->table->table->update;

					/* reset teaching flags to 0 */
					ps->flags &= PEER_TEACH_RESET;

					/* flag to start to teach lesson */
					ps->flags |= PEER_F_TEACH_PROCESS;
				}
				else if (c == 'F') {
					/* Finish message, all known updates have been pushed by remote */
					/* and remote is up to date */

					/* If resync is in progress with remote peer */
					if (ps->flags & PEER_F_LEARN_ASSIGN) {

						/* unassign current peer for learning  */
						ps->flags &= ~PEER_F_LEARN_ASSIGN;
						ps->table->flags &= ~(SHTABLE_F_RESYNC_ASSIGN|SHTABLE_F_RESYNC_PROCESS);

						/* Consider table is now up2date, resync resync no more needed from local neither remote */
						ps->table->flags |= (SHTABLE_F_RESYNC_LOCAL|SHTABLE_F_RESYNC_REMOTE);
					}
					/* Increase confirm counter to launch a confirm message */
					ps->confirm++;
				}
				else if (c == 'c') {
					/* confirm message, remote peer is now up to date with us */

					/* If stopping state */
					if (stopping) {
						/* Close session, push resync no more needed */
						ps->flags |= PEER_F_TEACH_COMPLETE;
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}

					/* reset teaching flags to 0 */
					ps->flags &= PEER_TEACH_RESET;
				}
				else if (c == 'C') {
					/* Continue message, all known updates have been pushed by remote */
					/* but remote is not up to date */

					/* If resync is in progress with current peer */
					if (ps->flags & PEER_F_LEARN_ASSIGN) {

						/* unassign current peer   */
						ps->flags &= ~PEER_F_LEARN_ASSIGN;
						ps->table->flags &= ~(SHTABLE_F_RESYNC_ASSIGN|SHTABLE_F_RESYNC_PROCESS);

						/* flag current peer is not up 2 date to try from an other */
						ps->flags |= PEER_F_LEARN_NOTUP2DATE;

						/* reschedule a resync */
						ps->table->resync_timeout = tick_add(now_ms, MS_TO_TICKS(5000));
						task_wakeup(ps->table->sync_task, TASK_WOKEN_MSG);
					}
					ps->confirm++;
				}
				else if (c == 'A') {
					/* ack message */
					uint32_t netinteger;

					reql = bo_getblk(si_oc(si), (char *)&netinteger, sizeof(netinteger), totl);
					if (reql <= 0) /* closed or EOL not found */
						goto incomplete;

					totl += reql;

					/* Consider remote is up to date with "acked" version */
					ps->update = ntohl(netinteger);
				}
				else {
					/* Unknown message */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}

				/* skip consumed message */
				bo_skip(si_oc(si), totl);

				/* loop on that state to peek next message */
				goto switchstate;

incomplete:
				/* we get here when a bo_getblk() returns <= 0 in reql */

				/* first, we may have to release newts */
				if (newts) {
					stksess_free(ps->table->table, newts);
					newts = NULL;
				}

				if (reql < 0) {
					/* there was an error */
					appctx->st0 = PEER_SESS_ST_END;
					goto switchstate;
				}

				/* Nothing to read, now we start to write */

				/* Confirm finished or partial messages */
				while (ps->confirm) {
					/* There is a confirm messages to send */
					repl = bi_putchr(si_ic(si), 'c');
					if (repl <= 0) {
						/* no more write possible */
						if (repl == -1)
							goto full;
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
					ps->confirm--;
				}

				/* Need to request a resync */
				if ((ps->flags & PEER_F_LEARN_ASSIGN) &&
					(ps->table->flags & SHTABLE_F_RESYNC_ASSIGN) &&
					!(ps->table->flags & SHTABLE_F_RESYNC_PROCESS)) {
					/* Current peer was elected to request a resync */

					repl = bi_putchr(si_ic(si), 'R');
					if (repl <= 0) {
						/* no more write possible */
						if (repl == -1)
							goto full;
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
					ps->table->flags |= SHTABLE_F_RESYNC_PROCESS;
				}

				/* It remains some updates to ack */
				if (ps->pushack != ps->lastack) {
					uint32_t netinteger;

					trash.str[0] = 'A';
					netinteger = htonl(ps->pushack);
					memcpy(&trash.str[1], &netinteger, sizeof(netinteger));

					repl = bi_putblk(si_ic(si), trash.str, 1+sizeof(netinteger));
					if (repl <= 0) {
						/* no more write possible */
						if (repl == -1)
							goto full;
						appctx->st0 = PEER_SESS_ST_END;
						goto switchstate;
					}
					ps->lastack = ps->pushack;
				}

				if (ps->flags & PEER_F_TEACH_PROCESS) {
					/* current peer was requested for a lesson */

					if (!(ps->flags & PEER_F_TEACH_STAGE1)) {
						/* lesson stage 1 not complete */
						struct eb32_node *eb;

						eb = eb32_lookup_ge(&ps->table->table->updates, ps->pushed+1);
						while (1) {
							int msglen;
							struct stksess *ts;

							if (!eb) {
								/* flag lesson stage1 complete */
								ps->flags |= PEER_F_TEACH_STAGE1;
								eb = eb32_first(&ps->table->table->updates);
								if (eb)
									ps->pushed = eb->key - 1;
								break;
							}

							ts = eb32_entry(eb, struct stksess, upd);
							msglen = peer_prepare_datamsg(ts, ps, trash.str, trash.size);
							if (msglen) {
								/* message to buffer */
								repl = bi_putblk(si_ic(si), trash.str, msglen);
								if (repl <= 0) {
									/* no more write possible */
									if (repl == -1)
										goto full;
									appctx->st0 = PEER_SESS_ST_END;
									goto switchstate;
								}
								ps->lastpush = ps->pushed = ts->upd.key;
							}
							eb = eb32_next(eb);
						}
					} /* !TEACH_STAGE1 */

					if (!(ps->flags & PEER_F_TEACH_STAGE2)) {
						/* lesson stage 2 not complete */
						struct eb32_node *eb;

						eb = eb32_lookup_ge(&ps->table->table->updates, ps->pushed+1);
						while (1) {
							int msglen;
							struct stksess *ts;

							if (!eb || eb->key > ps->teaching_origin) {
								/* flag lesson stage1 complete */
								ps->flags |= PEER_F_TEACH_STAGE2;
								ps->pushed = ps->teaching_origin;
								break;
							}

							ts = eb32_entry(eb, struct stksess, upd);
							msglen = peer_prepare_datamsg(ts, ps, trash.str, trash.size);
							if (msglen) {
								/* message to buffer */
								repl = bi_putblk(si_ic(si), trash.str, msglen);
								if (repl <= 0) {
									/* no more write possible */
									if (repl == -1)
										goto full;
									appctx->st0 = PEER_SESS_ST_END;
									goto switchstate;
								}
								ps->lastpush = ps->pushed = ts->upd.key;
							}
							eb = eb32_next(eb);
						}
					} /* !TEACH_STAGE2 */

					if (!(ps->flags & PEER_F_TEACH_FINISHED)) {
						/* process final lesson message */
						repl = bi_putchr(si_ic(si), ((ps->table->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FINISHED) ? 'F' : 'C');
						if (repl <= 0) {
							/* no more write possible */
							if (repl == -1)
								goto full;
							appctx->st0 = PEER_SESS_ST_END;
							goto switchstate;
						}

						/* flag finished message sent */
						ps->flags |= PEER_F_TEACH_FINISHED;
					} /* !TEACH_FINISHED */
				} /* TEACH_PROCESS */

				if (!(ps->flags & PEER_F_LEARN_ASSIGN) &&
				     (int)(ps->pushed - ps->table->table->localupdate) < 0) {
					/* Push local updates, only if no learning in progress (to avoid ping-pong effects) */
					struct eb32_node *eb;

					eb = eb32_lookup_ge(&ps->table->table->updates, ps->pushed+1);
					while (1) {
						int msglen;
						struct stksess *ts;

						/* push local updates */
						if (!eb) {
							eb = eb32_first(&ps->table->table->updates);
							if (!eb || ((int)(eb->key - ps->pushed) <= 0)) {
								ps->pushed = ps->table->table->localupdate;
								break;
							}
						}

						if ((int)(eb->key - ps->table->table->localupdate) > 0) {
							ps->pushed = ps->table->table->localupdate;
							break;
						}

						ts = eb32_entry(eb, struct stksess, upd);
						msglen = peer_prepare_datamsg(ts, ps, trash.str, trash.size);
						if (msglen) {
							/* message to buffer */
							repl = bi_putblk(si_ic(si), trash.str, msglen);
							if (repl <= 0) {
								/* no more write possible */
								if (repl == -1)
									goto full;
								appctx->st0 = PEER_SESS_ST_END;
								goto switchstate;
							}
							ps->lastpush = ps->pushed = ts->upd.key;
						}
						eb = eb32_next(eb);
					}
				} /* ! LEARN_ASSIGN */
				/* noting more to do */
				goto out;
			}
			case PEER_SESS_ST_EXIT:
				repl = snprintf(trash.str, trash.size, "%d\n", appctx->st1);

				if (bi_putblk(si_ic(si), trash.str, repl) == -1)
					goto full;
				appctx->st0 = PEER_SESS_ST_END;
				/* fall through */
			case PEER_SESS_ST_END: {
				si_shutw(si);
				si_shutr(si);
				si_ic(si)->flags |= CF_READ_NULL;
				goto quit;
			}
		}
	}
out:
	si_update(si);
	si_oc(si)->flags |= CF_READ_DONTWAIT;
	/* we don't want to expire timeouts while we're processing requests */
	si_ic(si)->rex = TICK_ETERNITY;
	si_oc(si)->wex = TICK_ETERNITY;
quit:
	return;
full:
	si->flags |= SI_FL_WAIT_ROOM;
	goto out;
}

static struct si_applet peer_applet = {
	.obj_type = OBJ_TYPE_APPLET,
	.name = "<PEER>", /* used for logging */
	.fct = peer_io_handler,
	.release = peer_session_release,
};

/*
 * Use this function to force a close of a peer session
 */
static void peer_session_forceshutdown(struct stream * stream)
{
	struct stream_interface *oldsi = NULL;
	struct appctx *appctx = NULL;
	int i;

	for (i = 0; i <= 1; i++) {
		appctx = objt_appctx(stream->si[i].end);
		if (!appctx)
			continue;
		if (appctx->applet != &peer_applet)
			continue;

		oldsi = &stream->si[i];
		break;
	}

	if (!appctx)
		return;

	/* call release to reinit resync states if needed */
	peer_session_release(oldsi);
	appctx->st0 = PEER_SESS_ST_END;
	appctx->ctx.peers.ptr = NULL;
	task_wakeup(stream->task, TASK_WOKEN_MSG);
}

/* Pre-configures a peers frontend to accept incoming connections */
void peers_setup_frontend(struct proxy *fe)
{
	fe->last_change = now.tv_sec;
	fe->cap = PR_CAP_FE;
	fe->maxconn = 0;
	fe->conn_retries = CONN_RETRIES;
	fe->timeout.client = MS_TO_TICKS(5000);
	fe->accept = frontend_accept;
	fe->default_target = &peer_applet.obj_type;
	fe->options2 |= PR_O2_INDEPSTR | PR_O2_SMARTCON | PR_O2_SMARTACC;
}

/*
 * Create a new peer session in assigned state (connect will start automatically)
 */
static struct stream *peer_session_create(struct peer *peer, struct peer_session *ps)
{
	struct listener *l = LIST_NEXT(&peer->peers->peers_fe->conf.listeners, struct listener *, by_fe);
	struct proxy *p = (struct proxy *)l->frontend; /* attached frontend */
	struct appctx *appctx;
	struct session *sess;
	struct stream *s;
	struct task *t;
	struct connection *conn;

	ps->reconnect = tick_add(now_ms, MS_TO_TICKS(5000));
	ps->statuscode = PEER_SESS_SC_CONNECTCODE;
	s = NULL;

	appctx = appctx_new(&peer_applet);
	if (!appctx)
		goto out_close;

	appctx->st0 = PEER_SESS_ST_CONNECT;
	appctx->ctx.peers.ptr = (void *)ps;

	sess = session_new(p, l, &appctx->obj_type);
	if (!sess) {
		Alert("out of memory in peer_session_create().\n");
		goto out_free_appctx;
	}

	if ((t = task_new()) == NULL) {
		Alert("out of memory in peer_session_create().\n");
		goto out_free_sess;
	}
	t->nice = l->nice;

	if ((s = stream_new(sess, t, &appctx->obj_type)) == NULL) {
		Alert("Failed to initialize stream in peer_session_create().\n");
		goto out_free_task;
	}

	/* The tasks below are normally what is supposed to be done by
	 * fe->accept().
	 */
	s->flags = SF_ASSIGNED|SF_ADDR_SET;

	/* initiate an outgoing connection */
	si_set_state(&s->si[1], SI_ST_ASS);

	/* automatically prepare the stream interface to connect to the
	 * pre-initialized connection in si->conn.
	 */
	if (unlikely((conn = conn_new()) == NULL))
		goto out_free_strm;

	conn_prepare(conn, peer->proto, peer->xprt);
	si_attach_conn(&s->si[1], conn);

	conn->target = s->target = &s->be->obj_type;
	memcpy(&conn->addr.to, &peer->addr, sizeof(conn->addr.to));
	s->do_log = NULL;
	s->uniq_id = 0;

	s->res.flags |= CF_READ_DONTWAIT;

	l->nbconn++; /* warning! right now, it's up to the handler to decrease this */
	p->feconn++;/* beconn will be increased later */
	jobs++;
	if (!(s->sess->listener->options & LI_O_UNLIMITED))
		actconn++;
	totalconn++;

	return s;

	/* Error unrolling */
 out_free_strm:
	LIST_DEL(&s->list);
	pool_free2(pool2_stream, s);
 out_free_task:
	task_free(t);
 out_free_sess:
	session_free(sess);
 out_free_appctx:
	appctx_free(appctx);
 out_close:
	return s;
}

/*
 * Task processing function to manage re-connect and peer session
 * tasks wakeup on local update.
 */
static struct task *process_peer_sync(struct task * task)
{
	struct shared_table *st = (struct shared_table *)task->context;
	struct peer_session *ps;

	task->expire = TICK_ETERNITY;

	if (!stopping) {
		/* Normal case (not soft stop)*/
		if (((st->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FROMLOCAL) &&
		     (!nb_oldpids || tick_is_expired(st->resync_timeout, now_ms)) &&
		     !(st->flags & SHTABLE_F_RESYNC_ASSIGN)) {
			/* Resync from local peer needed
			   no peer was assigned for the lesson
			   and no old local peer found
			       or resync timeout expire */

			/* flag no more resync from local, to try resync from remotes */
			st->flags |= SHTABLE_F_RESYNC_LOCAL;

			/* reschedule a resync */
			st->resync_timeout = tick_add(now_ms, MS_TO_TICKS(5000));
		}

		/* For each session */
		for (ps = st->sessions; ps; ps = ps->next) {
			/* For each remote peers */
			if (!ps->peer->local) {
				if (!ps->stream) {
					/* no active stream */
					if (ps->statuscode == 0 ||
					    ps->statuscode == PEER_SESS_SC_SUCCESSCODE ||
					    ((ps->statuscode == PEER_SESS_SC_CONNECTCODE ||
					      ps->statuscode == PEER_SESS_SC_CONNECTEDCODE) &&
					     tick_is_expired(ps->reconnect, now_ms))) {
						/* connection never tried
						 * or previous stream established with success
						 * or previous stream failed during connection
						 * and reconnection timer is expired */

						/* retry a connect */
						ps->stream = peer_session_create(ps->peer, ps);
					}
					else if (ps->statuscode == PEER_SESS_SC_CONNECTCODE ||
						 ps->statuscode == PEER_SESS_SC_CONNECTEDCODE) {
						/* If previous session failed during connection
						 * but reconnection timer is not expired */

						/* reschedule task for reconnect */
						task->expire = tick_first(task->expire, ps->reconnect);
					}
					/* else do nothing */
				} /* !ps->stream */
				else if (ps->statuscode == PEER_SESS_SC_SUCCESSCODE) {
					/* current stream is active and established */
					if (((st->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FROMREMOTE) &&
					    !(st->flags & SHTABLE_F_RESYNC_ASSIGN) &&
					    !(ps->flags & PEER_F_LEARN_NOTUP2DATE)) {
						/* Resync from a remote is needed
						 * and no peer was assigned for lesson
						 * and current peer may be up2date */

						/* assign peer for the lesson */
						ps->flags |= PEER_F_LEARN_ASSIGN;
						st->flags |= SHTABLE_F_RESYNC_ASSIGN;

						/* awake peer stream task to handle a request of resync */
						task_wakeup(ps->stream->task, TASK_WOKEN_MSG);
					}
					else if ((int)(ps->pushed - ps->table->table->localupdate) < 0) {
						/* awake peer stream task to push local updates */
						task_wakeup(ps->stream->task, TASK_WOKEN_MSG);
					}
					/* else do nothing */
				} /* SUCCESSCODE */
			} /* !ps->peer->local */
		} /* for */

		/* Resync from remotes expired: consider resync is finished */
		if (((st->flags & SHTABLE_RESYNC_STATEMASK) == SHTABLE_RESYNC_FROMREMOTE) &&
		    !(st->flags & SHTABLE_F_RESYNC_ASSIGN) &&
		    tick_is_expired(st->resync_timeout, now_ms)) {
			/* Resync from remote peer needed
			 * no peer was assigned for the lesson
			 * and resync timeout expire */

			/* flag no more resync from remote, consider resync is finished */
			st->flags |= SHTABLE_F_RESYNC_REMOTE;
		}

		if ((st->flags & SHTABLE_RESYNC_STATEMASK) != SHTABLE_RESYNC_FINISHED) {
			/* Resync not finished*/
			/* reschedule task to resync timeout, to ended resync if needed */
			task->expire = tick_first(task->expire, st->resync_timeout);
		}
	} /* !stopping */
	else {
		/* soft stop case */
		if (task->state & TASK_WOKEN_SIGNAL) {
			/* We've just recieved the signal */
			if (!(st->flags & SHTABLE_F_DONOTSTOP)) {
				/* add DO NOT STOP flag if not present */
				jobs++;
				st->flags |= SHTABLE_F_DONOTSTOP;
				st->table->syncing++;
			}

			/* disconnect all connected peers */
			for (ps = st->sessions; ps; ps = ps->next) {
				if (ps->stream) {
					peer_session_forceshutdown(ps->stream);
					ps->stream = NULL;
				}
			}
		}
		ps = st->local_session;

		if (ps->flags & PEER_F_TEACH_COMPLETE) {
			if (st->flags & SHTABLE_F_DONOTSTOP) {
				/* resync of new process was complete, current process can die now */
				jobs--;
				st->flags &= ~SHTABLE_F_DONOTSTOP;
				st->table->syncing--;
			}
		}
		else if (!ps->stream) {
			/* If stream is not active */
			if (ps->statuscode == 0 ||
			    ps->statuscode == PEER_SESS_SC_SUCCESSCODE ||
			    ps->statuscode == PEER_SESS_SC_CONNECTEDCODE ||
			    ps->statuscode == PEER_SESS_SC_TRYAGAIN) {
				/* connection never tried
				 * or previous stream was successfully established
				 * or previous stream tcp connect success but init state incomplete
				 * or during previous connect, peer replies a try again statuscode */

				/* connect to the peer */
				ps->stream = peer_session_create(ps->peer, ps);
			}
			else {
				/* Other error cases */
				if (st->flags & SHTABLE_F_DONOTSTOP) {
					/* unable to resync new process, current process can die now */
					jobs--;
					st->flags &= ~SHTABLE_F_DONOTSTOP;
					st->table->syncing--;
				}
			}
		}
		else if (ps->statuscode == PEER_SESS_SC_SUCCESSCODE &&
		         (int)(ps->pushed - ps->table->table->localupdate) < 0) {
			/* current stream active and established
			   awake stream to push remaining local updates */
			task_wakeup(ps->stream->task, TASK_WOKEN_MSG);
		}
	} /* stopping */
	/* Wakeup for re-connect */
	return task;
}

/*
 * Function used to register a table for sync on a group of peers
 *
 */
void peers_register_table(struct peers *peers, struct stktable *table)
{
	struct shared_table *st;
	struct peer * curpeer;
	struct peer_session *ps;
	struct listener *listener;

	st = (struct shared_table *)calloc(1,sizeof(struct shared_table));
	st->table = table;
	st->next = peers->tables;
	st->resync_timeout = tick_add(now_ms, MS_TO_TICKS(5000));
	peers->tables = st;

	for (curpeer = peers->remote; curpeer; curpeer = curpeer->next) {
		ps = (struct peer_session *)calloc(1,sizeof(struct peer_session));
		ps->table = st;
		ps->peer = curpeer;
		if (curpeer->local)
			st->local_session = ps;
		ps->next = st->sessions;
		ps->reconnect = now_ms;
		st->sessions = ps;
		peers->peers_fe->maxconn += 3;
	}

	list_for_each_entry(listener, &peers->peers_fe->conf.listeners, by_fe)
		listener->maxconn = peers->peers_fe->maxconn;
	st->sync_task = task_new();
	st->sync_task->process = process_peer_sync;
	st->sync_task->expire = TICK_ETERNITY;
	st->sync_task->context = (void *)st;
	table->sync_task =st->sync_task;
	signal_register_task(0, table->sync_task, 0);
	task_wakeup(st->sync_task, TASK_WOKEN_INIT);
}

