/*
 * include/proto/dumpstats.h
 * This file contains definitions of some primitives to dedicated to
 * statistics output.
 *
 * Copyright (C) 2000-2011 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _PROTO_DUMPSTATS_H
#define _PROTO_DUMPSTATS_H

#include <common/config.h>
#include <types/stream_interface.h>

/* Flags for applet.ctx.stats.flags */
#define STAT_FMT_HTML   0x00000001      /* dump the stats in HTML format */
#define STAT_HIDE_DOWN  0x00000008	/* hide 'down' servers in the stats page */
#define STAT_NO_REFRESH 0x00000010	/* do not automatically refresh the stats page */
#define STAT_ADMIN      0x00000020	/* indicate a stats admin level */
#define STAT_BOUND      0x00800000	/* bound statistics to selected proxies/types/services */

#define STATS_TYPE_FE  0
#define STATS_TYPE_BE  1
#define STATS_TYPE_SV  2
#define STATS_TYPE_SO  3

/* unix stats socket states */
#define STAT_CLI_INIT   0   /* initial state */
#define STAT_CLI_END    1   /* final state, let's close */
#define STAT_CLI_GETREQ 2   /* wait for a request */
#define STAT_CLI_OUTPUT 3   /* all states after this one are responses */
#define STAT_CLI_PROMPT 3   /* display the prompt (first output, same code) */
#define STAT_CLI_PRINT  4   /* display message in cli->msg */

#define STAT_CLI_O_INFO 5   /* dump info */
#define STAT_CLI_O_SESS 6   /* dump sessions */
#define STAT_CLI_O_ERR  7   /* dump errors */
#define STAT_CLI_O_TAB  8   /* dump tables */
#define STAT_CLI_O_CLR  9   /* clear tables */
#define STAT_CLI_O_SET  10  /* set entries in tables */
#define STAT_CLI_O_STAT 11  /* dump stats */

extern struct si_applet http_stats_applet;

void stats_io_handler(struct stream_interface *si);

int process_weight_change_request(struct server *sv, const char *weight_str);

#endif /* _PROTO_DUMPSTATS_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
