/*
 * Copyright (C) 2014 Philipp Marek <philipp.marek@linbit.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include "booth.h"
#include "timer.h"
#include "transport.h"
#include "inline-fn.h"
#include "config.h"
#include "raft.h"
#include "ticket.h"
#include "log.h"



inline static void clear_election(struct ticket_config *tk)
{
	int i;
	struct booth_site *site;

	tk_log_debug("clear election");
	tk->votes_received = 0;
	foreach_node(i, site)
		tk->votes_for[site->index] = NULL;
}


inline static void record_vote(struct ticket_config *tk,
		struct booth_site *who,
		struct booth_site *vote)
{
	tk_log_debug("site %s votes for %s",
			site_string(who),
			site_string(vote));

	if (!tk->votes_for[who->index]) {
		tk->votes_for[who->index] = vote;
		tk->votes_received |= who->bitmask;
	} else {
		if (tk->votes_for[who->index] != vote)
			tk_log_warn("%s voted previously "
					"for %s and now wants to vote for %s (ignored)",
					site_string(who),
					site_string(tk->votes_for[who->index]),
					site_string(vote));
	}
}


static int cmp_msg_ticket(struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg)
{
	if (my_last_term(tk) != ntohl(msg->ticket.term)) {
		return my_last_term(tk) - ntohl(msg->ticket.term);
	}
	return 0;
}

static void update_term_from_msg(struct ticket_config *tk,
		struct boothc_ticket_msg *msg)
{
	uint32_t i;


	i = ntohl(msg->ticket.term);
	/* if we failed to start the election, then accept the term
	 * from the leader
	 * */
	if (tk->state == ST_CANDIDATE) {
		tk->current_term = i;
	} else {
		tk->current_term = max(i, tk->current_term);
	}
}


static void update_ticket_from_msg(struct ticket_config *tk,
		struct booth_site *sender,
		struct boothc_ticket_msg *msg)
{
	int duration;

	tk_log_debug("updating from %s (%d/%d)",
		site_string(sender),
		ntohl(msg->ticket.term), ntohl(msg->ticket.term_valid_for));
	duration = min(tk->term_duration, ntohl(msg->ticket.term_valid_for));
	tk->term_expires = get_secs(NULL) + duration;
	update_term_from_msg(tk, msg);
}


static void copy_ticket_from_msg(struct ticket_config *tk,
		struct boothc_ticket_msg *msg)
{
	tk->term_expires = get_secs(NULL) + ntohl(msg->ticket.term_valid_for);
	tk->current_term = ntohl(msg->ticket.term);
}

static void become_follower(struct ticket_config *tk,
		struct boothc_ticket_msg *msg)
{
	copy_ticket_from_msg(tk, msg);
	tk->state = ST_FOLLOWER;
	tk->delay_commit = 0;
	tk->in_election = 0;
	/* if we're following and the ticket was granted here
	 * then commit to CIB right away (we're probably restarting)
	 */
	if (tk->is_granted) {
		disown_ticket(tk);
		ticket_write(tk);
	}
}


static void won_elections(struct ticket_config *tk)
{
	tk->leader = local;
	tk->state = ST_LEADER;

	tk->term_expires = get_secs(NULL) + tk->term_duration;
	tk->election_end = 0;
	tk->voted_for = NULL;

	ticket_broadcast(tk, OP_HEARTBEAT, OP_ACK, RLT_SUCCESS, 0);
	ticket_activate_timeout(tk);
}


static int is_tie(struct ticket_config *tk)
{
	int i;
	struct booth_site *v;
	int count[MAX_NODES] = { 0, };
	int max_votes = 0, max_cnt = 0;

	for(i=0; i<booth_conf->site_count; i++) {
		v = tk->votes_for[i];
		if (!v)
			continue;
		count[v->index]++;
		max_votes = max(max_votes, count[v->index]);
	}

	for(i=0; i<booth_conf->site_count; i++) {
		if (count[i] == max_votes)
			max_cnt++;
	}

	return max_cnt > 1;
}

static struct booth_site *majority_votes(struct ticket_config *tk)
{
	int i, n;
	struct booth_site *v;
	int count[MAX_NODES] = { 0, };


	for(i=0; i<booth_conf->site_count; i++) {
		v = tk->votes_for[i];
		if (!v)
			continue;

		n = v->index;
		count[n]++;
		tk_log_debug("Majority: %d %s wants %d %s => %d",
				i, site_string(&booth_conf->site[i]),
				n, site_string(v),
				count[n]);

		if (count[n]*2 <= booth_conf->site_count)
			continue;


		tk_log_debug("Majority reached: %d of %d for %s",
				count[n], booth_conf->site_count,
				site_string(v));
		return v;
	}

	return NULL;
}


void elections_end(struct ticket_config *tk)
{
	time_t now;
	struct booth_site *new_leader;

	now = get_secs(NULL);
	if (now > tk->election_end) {
		/* This is previous election timed out */
		tk_log_info("elections finished");
	}

	tk->in_election = 0;
	new_leader = majority_votes(tk);
	if (new_leader == local) {
		tk_log_info("granted successfully here");
		won_elections(tk);
	} else if (new_leader) {
		tk_log_info("ticket granted at %s",
				site_string(new_leader));
	} else {
		tk_log_info("nobody won elections, new elections");
		if (!new_election(tk, NULL, is_tie(tk), OR_AGAIN)) {
			ticket_activate_timeout(tk);
		}
	}
}


static int newer_term(struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg,
		int in_election)
{
	uint32_t term;

	/* it may happen that we hear about our newer term */
	if (leader == local)
		return 0;

	term = ntohl(msg->ticket.term);
	/* §5.1 */
	if (term > tk->current_term) {
		tk->state = ST_FOLLOWER;
		if (!in_election) {
			tk->leader = leader;
			tk_log_info("from %s: higher term %d vs. %d, following %s",
					site_string(sender),
					term, tk->current_term,
					ticket_leader_string(tk));
		} else {
			tk_log_debug("from %s: higher term %d vs. %d (election)",
					site_string(sender),
					term, tk->current_term);
		}

		tk->current_term = term;
		return 1;
	}

	return 0;
}

static int term_too_low(struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg)
{
	uint32_t term;

	term = ntohl(msg->ticket.term);
	/* §5.1 */
	if (term < tk->current_term) {
		tk_log_info("sending reject to %s, its term too low "
			"(%d vs. %d)", site_string(sender),
			term, tk->current_term
			);
		send_reject(sender, tk, RLT_TERM_OUTDATED, msg);
		return 1;
	}

	return 0;
}




/* For follower. */
static int answer_HEARTBEAT (
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	uint32_t term;

	term = ntohl(msg->ticket.term);
	tk_log_debug("heartbeat from leader: %s, have %s; term %d vs %d",
			site_string(leader), ticket_leader_string(tk),
			term, tk->current_term);

	if (term < tk->current_term) {
		if (sender == tk->leader) {
			tk_log_info("trusting leader %s with a lower term (%d vs %d)",
				site_string(leader), term, tk->current_term);
		} else if (is_owned(tk)) {
			tk_log_warn("different leader %s with a lower term "
					"(%d vs %d), sending reject",
				site_string(leader), term, tk->current_term);
			return send_reject(sender, tk, RLT_TERM_OUTDATED, msg);
		}
	}

	/* got heartbeat, no rejects expected anymore */
	tk->expect_more_rejects = 0;

	/* Needed? */
	newer_term(tk, sender, leader, msg, 0);

	become_follower(tk, msg);
	/* Racy??? */
	assert(sender == leader || !leader);

	tk->leader = leader;

	/* Ack the heartbeat (we comply). */
	return send_msg(OP_ACK, tk, sender, msg);
}


static int process_UPDATE (
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	if (is_owned(tk) && sender != tk->leader) {
		tk_log_warn("different leader %s wants to update "
				"our ticket, sending reject",
			site_string(leader));
		return send_reject(sender, tk, RLT_TERM_OUTDATED, msg);
	}

	tk_log_debug("leader %s wants to update our ticket",
			site_string(leader));

	tk->leader = leader;
	copy_ticket_from_msg(tk, msg);
	ticket_write(tk);

	/* run ticket_cron if the ticket expires */
	set_ticket_wakeup(tk);

	return send_msg(OP_ACK, tk, sender, msg);
}

static int process_REVOKE (
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	int rv;

	if (tk->state == ST_INIT && tk->leader == no_leader) {
		/* assume that our ack got lost */
		rv = send_msg(OP_ACK, tk, sender, msg);
	} else if (tk->leader != sender) {
		tk_log_error("%s wants to revoke ticket, "
				"but it is not granted there (ignoring)",
				site_string(sender));
		return 1;
	} else if (tk->state != ST_FOLLOWER) {
		tk_log_error("unexpected ticket revoke from %s "
				"(in state %s) (ignoring)",
				site_string(sender),
				state_to_string(tk->state));
		return 1;
	} else {
		tk_log_info("%s revokes ticket",
				site_string(tk->leader));
		reset_ticket(tk);
		tk->leader = no_leader;
		ticket_write(tk);
		rv = send_msg(OP_ACK, tk, sender, msg);
	}

	return rv;
}


/* For leader. */
static int process_ACK(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	uint32_t term;

	term = ntohl(msg->ticket.term);

	if (newer_term(tk, sender, leader, msg, 0)) {
		/* unexpected higher term */
		tk_log_warn("got higher term from %s (%d vs. %d)",
				site_string(sender),
				term, tk->current_term);
		return 0;
	}

	/* Don't send a reject. */
	if (term < tk->current_term) {
		/* Doesn't know what he's talking about - perhaps
		 * doesn't receive our packets? */
		tk_log_warn("unexpected term "
				"from %s (%d vs. %d) (ignoring)",
				site_string(sender),
				term, tk->current_term);
		return 0;
	}

	/* if the ticket is to be revoked, further processing is not
	 * interesting (and dangerous) */
	if (tk->next_state == ST_INIT || tk->state == ST_INIT)
		return 0;

	/* for heartbeats we make do with the majority */
	if (tk->last_request == OP_HEARTBEAT &&
			term == tk->current_term &&
			leader == tk->leader) {

		if (majority_of_bits(tk, tk->acks_received)) {
			/* OK, at least half of the nodes are reachable;
			 * Update the ticket and send update messages out
			 */
			return leader_update_ticket(tk);
		}
	}

	return 0;
}


static int process_VOTE_FOR(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
		)
{
	/* leader wants to step down? */
	if (leader == no_leader && sender == tk->leader &&
			(tk->state == ST_FOLLOWER || tk->state == ST_CANDIDATE)) {
		tk_log_info("%s wants to give the ticket away",
			site_string(tk->leader));
		reset_ticket(tk);
		tk->state = ST_FOLLOWER;
		if (local->type == SITE) {
			ticket_write(tk);
			schedule_election(tk, OR_STEPDOWN);
		}
		return 0;
	}

	if (tk->state != ST_CANDIDATE) {
		/* lost candidate status, somebody rejected our proposal */
		tk_log_debug("candidate status lost, ignoring vote_for from %s",
			site_string(sender));
		return 0;
	}

	if (term_too_low(tk, sender, leader, msg))
		return 0;

	if (newer_term(tk, sender, leader, msg, 0)) {
		clear_election(tk);
	}

	record_vote(tk, sender, leader);

	/* only if all voted can we take the ticket now, otherwise
	 * wait for timeout in ticket_cron */
	if (!tk->acks_expected) {
		/* §5.2 */
		elections_end(tk);
	}

	return 0;
}


static int process_REJECTED(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
		)
{
	uint32_t rv;

	rv   = ntohl(msg->header.result);

	if (tk->state == ST_CANDIDATE &&
			leader == local) {
		/* the sender has us as the leader (!)
		 * the elections will time out, then we can try again
		 */
		tk_log_warn("ticket was granted to us "
				"(and we didn't know)");
		tk->expect_more_rejects = 1;
		return 0;
	}

	if (tk->state == ST_CANDIDATE &&
			rv == RLT_TERM_OUTDATED) {
		tk_log_warn("ticket outdated (term %d), granted to %s",
				ntohl(msg->ticket.term),
				site_string(leader)
				);
		tk->leader = leader;
		tk->expect_more_rejects = 1;
		become_follower(tk, msg);
		return 0;
	}

	if (tk->state == ST_CANDIDATE &&
			rv == RLT_TERM_STILL_VALID) {
		if (tk->lost_leader == leader) {
			if (tk->election_reason == OR_TKT_LOST) {
				tk_log_warn("%s still has the ticket valid, "
						"we'll backup a bit",
						site_string(sender));
			} else {
				tk_log_warn("%s unexpectedly rejects elections",
						site_string(sender));
			}
		} else {
			tk_log_warn("ticket was granted to %s "
					"(and we didn't know)",
					site_string(leader));
		}
		tk->leader = leader;
		become_follower(tk, msg);
		tk->expect_more_rejects = 1;
		return 0;
	}

	if (tk->state == ST_CANDIDATE &&
			rv == RLT_YOU_OUTDATED) {
		tk->leader = leader;
		tk->expect_more_rejects = 1;
		if (leader && leader != no_leader) {
			tk_log_warn("our ticket is outdated, granted to %s",
				site_string(leader));
			become_follower(tk, msg);
		} else {
			tk_log_warn("our ticket is outdated and revoked");
			update_ticket_from_msg(tk, sender, msg);
			tk->state = ST_INIT;
		}
		return 0;
	}

	if (!tk->expect_more_rejects) {
		tk_log_warn("from %s: in state %s, got %s (unexpected reject)",
				site_string(sender),
				state_to_string(tk->state),
				state_to_string(rv));
	}

	return 0;
}


static int ticket_seems_ok(struct ticket_config *tk)
{
	int time_left;

	time_left = term_time_left(tk);
	if (!time_left)
		return 0; /* quite sure */
	if (tk->state == ST_CANDIDATE)
		return 0; /* in state of flux */
	if (tk->state == ST_LEADER)
		return 1; /* quite sure */
	if (tk->state == ST_FOLLOWER &&
			time_left >= tk->term_duration/3)
		return 1; /* almost quite sure */
	return 0;
}


static int test_reason(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
		)
{
	int reason;

	reason = ntohl(msg->header.reason);
	if (reason == OR_TKT_LOST) {
		if (tk->state == ST_INIT &&
				tk->leader == no_leader) {
			tk_log_warn("%s claims that the ticket is lost, "
					"but it's in %s state (reject sent)",
					site_string(sender),
					state_to_string(tk->state)
				);
			return RLT_YOU_OUTDATED;
		}
		if (ticket_seems_ok(tk)) {
			tk_log_warn("%s claims that the ticket is lost, "
					"but it is ok here (reject sent)",
					site_string(sender));
			return RLT_TERM_STILL_VALID;
		}
	}
	return 0;
}


/* §5.2 */
static int answer_REQ_VOTE(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
		)
{
	int valid;
	struct boothc_ticket_msg omsg;
	cmd_result_t inappr_reason;

	inappr_reason = test_reason(tk, sender, leader, msg);
	if (inappr_reason)
		return send_reject(sender, tk, inappr_reason, msg);

	valid = term_time_left(tk);

	/* allow the leader to start new elections on valid tickets */
	if (sender != tk->leader && valid) {
		tk_log_warn("election from %s rejected "
			"(we have %s as ticket owner), ticket still valid for %ds",
			site_string(sender), site_string(tk->leader), valid);
		return send_reject(sender, tk, RLT_TERM_STILL_VALID, msg);
	}

	if (term_too_low(tk, sender, leader, msg))
		return 0;

	/* set this, so that we know not to send status for the
	 * ticket */
	tk->in_election = 1;

	/* if it's a newer term or ... */
	if (newer_term(tk, sender, leader, msg, 1)) {
		clear_election(tk);
		goto vote_for_sender;
	}


	/* ... we didn't vote yet, then vote for the sender */
	/* §5.2, §5.4 */
	if (!tk->voted_for) {
vote_for_sender:
		tk->voted_for = sender;
		record_vote(tk, sender, leader);
	}


	init_ticket_msg(&omsg, OP_VOTE_FOR, OP_REQ_VOTE, RLT_SUCCESS, 0, tk);
	omsg.ticket.leader = htonl(get_node_id(tk->voted_for));
	return booth_udp_send(sender, &omsg, sizeof(omsg));
}


int new_election(struct ticket_config *tk,
	struct booth_site *preference, int update_term, cmd_reason_t reason)
{
	struct booth_site *new_leader;
	time_t now;

	if (local->type != SITE)
		return 0;

	get_secs(&now);
	tk_log_debug("start new election?, now=%" PRIi64 ", end %" PRIi64,
			(int64_t)wall_ts(now), (int64_t)(wall_ts(tk->election_end)));
	if (now < tk->election_end)
		return 1;

	/* §5.2 */
	/* If there was _no_ answer, don't keep incrementing the term number
	 * indefinitely. If there was no peer, there'll probably be no one
	 * listening now either. However, we don't know if we were
	 * invoked due to a timeout (caller does).
	 */
	if (update_term) {
		/* save the previous term, we may need to send out the
		 * MY_INDEX message */
		if (tk->state != ST_CANDIDATE) {
			memcpy(tk->last_valid_tk, tk, sizeof(struct ticket_config));
		}
		tk->current_term++;
	}

	tk->term_expires = 0;
	tk->election_end = now + tk->timeout;
	tk->in_election = 1;

	tk_log_info("starting new election (term=%d)",
			tk->current_term);
	clear_election(tk);

	if(preference)
		new_leader = preference;
	else
		new_leader = (local->type == SITE) ? local : NULL;
	record_vote(tk, local, new_leader);
	tk->voted_for = new_leader;

	tk->leader = no_leader;
	tk->state = ST_CANDIDATE;

	/* some callers may want just to repeat on timeout */
	if (reason == OR_AGAIN) {
		reason = tk->election_reason;
	} else {
		tk->election_reason = reason;
	}

	ticket_broadcast(tk, OP_REQ_VOTE, OP_VOTE_FOR, RLT_SUCCESS, reason);
	ticket_activate_timeout(tk);
	add_random_delay(tk);
	return 0;
}


/* we were a leader and somebody says that they have a more up
 * to date ticket
 * there was probably connectivity loss
 * tricky
 */
static int leader_handle_newer_ticket(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	update_term_from_msg(tk, msg);
	if (leader != no_leader && leader && leader != local) {
		/* eek, two leaders, split brain */
		/* normally shouldn't happen; run election */
		tk_log_error("from %s: ticket granted to %s! (revoking locally)",
				site_string(sender),
				site_string(leader)
				);
	} else if (term_time_left(tk)) {
		/* eek, two leaders, split brain */
		/* normally shouldn't happen; run election */
		tk_log_error("from %s: ticket granted to %s! (revoking locally)",
				site_string(sender),
				site_string(leader)
				);
	}
	tk->next_state = ST_LEADER;
	return 0;
}

/* reply to STATUS */
static int process_MY_INDEX (
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	int i;
	int expired;

	expired = !msg->ticket.term_valid_for;
	i = cmp_msg_ticket(tk, sender, leader, msg);

	if (i > 0) {
		/* let them know about our newer ticket */
		/* but if we're voting in elections, our ticket is not
		 * valid yet, don't send it */
		if (!tk->in_election)
			send_msg(OP_MY_INDEX, tk, sender, msg);
		if (tk->state == ST_LEADER) {
			tk_log_info("sending ticket update to %s",
					site_string(sender));
			return send_msg(OP_UPDATE, tk, sender, msg);
		}
	}

	/* we have a newer or equal ticket and theirs is expired,
	 * nothing more to do here */
	if (i >= 0 && expired) {
		return 0;
	}

	if (tk->state == ST_LEADER) {
		/* we're the leader, thread carefully */
		if (expired) {
			/* if their ticket is expired,
			 * nothing more to do */
			return 0;
		}
		if (i < 0) {
			/* they have a newer ticket, trouble if we're already leader
			 * for it */
			tk_log_warn("from %s: more up to date ticket at %s",
					site_string(sender),
					site_string(leader)
					);
			return leader_handle_newer_ticket(tk, sender, leader, msg);
		} else {
			/* we have the ticket and we don't care */
			return 0;
		}
	}

	/* their ticket is either newer or not expired, don't
	 * ignore it */
	update_ticket_from_msg(tk, sender, msg);
	tk->leader = leader;
	update_ticket_state(tk, sender);
	set_ticket_wakeup(tk);
	return 0;
}


int raft_answer(
		struct ticket_config *tk,
		struct booth_site *sender,
		struct booth_site *leader,
		struct boothc_ticket_msg *msg
	       )
{
	int cmd, req;
	int rv;

	rv = 0;
	cmd = ntohl(msg->header.cmd);
	req = ntohl(msg->header.request);

	if (req)
		tk_log_debug("got %s (req %s) from %s",
				state_to_string(cmd),
				state_to_string(req),
				site_string(sender));
	else
		tk_log_debug("got %s from %s",
				state_to_string(cmd),
				site_string(sender));


	switch (cmd) {
	case OP_REQ_VOTE:
		rv = answer_REQ_VOTE(tk, sender, leader, msg);
		break;
	case OP_VOTE_FOR:
		rv = process_VOTE_FOR(tk, sender, leader, msg);
		break;
	case OP_ACK:
		if (tk->leader == local &&
				tk->state == ST_LEADER)
			rv = process_ACK(tk, sender, leader, msg);
		break;
	case OP_HEARTBEAT:
		if ((tk->leader != local || !term_time_left(tk)) &&
				(tk->state == ST_INIT || tk->state == ST_FOLLOWER ||
				tk->state == ST_CANDIDATE))
			rv = answer_HEARTBEAT(tk, sender, leader, msg);
		else {
			tk_log_warn("unexpected message %s, from %s",
				state_to_string(cmd),
				site_string(sender));
			if (ticket_seems_ok(tk))
				send_reject(sender, tk, RLT_TERM_STILL_VALID, msg);
			rv = -EINVAL;
		}
		break;
	case OP_UPDATE:
		if (((tk->leader != local && tk->leader == leader) || !is_owned(tk)) &&
				(tk->state == ST_INIT || tk->state == ST_FOLLOWER ||
				tk->state == ST_CANDIDATE)) {
			rv = process_UPDATE(tk, sender, leader, msg);
		} else {
			tk_log_warn("unexpected message %s, from %s",
				state_to_string(cmd),
				site_string(sender));
			if (ticket_seems_ok(tk))
				send_reject(sender, tk, RLT_TERM_STILL_VALID, msg);
			rv = -EINVAL;
		}
		break;
	case OP_REJECTED:
		rv = process_REJECTED(tk, sender, leader, msg);
		break;
	case OP_REVOKE:
		rv = process_REVOKE(tk, sender, leader, msg);
		break;
	case OP_MY_INDEX:
		rv = process_MY_INDEX(tk, sender, leader, msg);
		break;
	case OP_STATUS:
		if (!tk->in_election)
			rv = send_msg(OP_MY_INDEX, tk, sender, msg);
		break;
	default:
		tk_log_error("unknown message %s, from %s",
			state_to_string(cmd), site_string(sender));
		rv = -EINVAL;
	}
	return rv;
}
