/*
 * (C) 2016-2017 by Holger Hans Peter Freyther
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "mncc.h"
#include "mncc_protocol.h"
#include "app.h"
#include "logging.h"
#include "call.h"

#include <osmocom/gsm/protocol/gsm_03_40.h>

#include <osmocom/core/socket.h>
#include <osmocom/core/utils.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <unistd.h>

extern void *tall_mncc_ctx;

static void close_connection(struct mncc_connection *conn);

static void mncc_leg_release(struct mncc_call_leg *leg)
{
	osmo_timer_del(&leg->cmd_timeout);
	call_leg_release(&leg->base);
}

static void cmd_timeout(void *data)
{
	struct mncc_call_leg *leg = data;
	struct call_leg *other_leg;

	LOGP(DMNCC, LOGL_ERROR, "cmd(0x%x) never arrived for leg(%u)\n",
		leg->rsp_wanted, leg->callref);

	other_leg = call_leg_other(&leg->base);
	if (other_leg)
		other_leg->release_call(other_leg);
	mncc_leg_release(leg);
}

static void start_cmd_timer(struct mncc_call_leg *leg, uint32_t expected_next)
{
	leg->rsp_wanted = expected_next;

	leg->cmd_timeout.cb = cmd_timeout;
	leg->cmd_timeout.data = leg;
	osmo_timer_schedule(&leg->cmd_timeout, 5, 0);
}

static void stop_cmd_timer(struct mncc_call_leg *leg, uint32_t got_res)
{
	if (leg->rsp_wanted != got_res) {
		LOGP(DMNCC, LOGL_ERROR, "Wanted rsp(%u) but got(%u) for leg(%u)\n",
			leg->rsp_wanted, got_res, leg->callref);
		return;
	}

	LOGP(DMNCC, LOGL_DEBUG,
		"Got response(0x%x), stopping timer on leg(%u)\n",
		got_res, leg->callref);
	osmo_timer_del(&leg->cmd_timeout);
}

static struct mncc_call_leg *mncc_find_leg(uint32_t callref)
{
	struct call *call;

	llist_for_each_entry(call, &g_call_list, entry) {
		if (call->initial && call->initial->type == CALL_TYPE_MNCC) {
			struct mncc_call_leg *leg = (struct mncc_call_leg *) call->initial;
			if (leg->callref == callref)
				return leg;
		}
		if (call->remote && call->remote->type == CALL_TYPE_MNCC) {
			struct mncc_call_leg *leg = (struct mncc_call_leg *) call->remote;
			if (leg->callref == callref)
				return leg;
		}
	}

	return NULL;
}

static void mncc_fill_header(struct gsm_mncc *mncc, uint32_t msg_type, uint32_t callref)
{
	mncc->msg_type = msg_type;
	mncc->callref = callref;
}

static void mncc_write(struct mncc_connection *conn, struct gsm_mncc *mncc, uint32_t callref)
{
	int rc;

	/*
	 * TODO: we need to put cause in here for release or such? shall we return a
	 * static struct?
	 */
	rc = write(conn->fd.fd, mncc, sizeof(*mncc));
	if (rc != sizeof(*mncc)) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to send message call(%u)\n", callref);
		close_connection(conn);
	}
}

static void mncc_send(struct mncc_connection *conn, uint32_t msg_type, uint32_t callref)
{
	struct gsm_mncc mncc = { 0, };

	mncc_fill_header(&mncc, msg_type, callref);
	mncc_write(conn, &mncc, callref);
}

static void mncc_rtp_send(struct mncc_connection *conn, uint32_t msg_type, uint32_t callref)
{
	int rc;
	struct gsm_mncc_rtp mncc = { 0, };

	mncc.msg_type = msg_type;
	mncc.callref = callref;

	rc = write(conn->fd.fd, &mncc, sizeof(mncc));
	if (rc != sizeof(mncc)) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to send message call(%u)\n", callref);
		close_connection(conn);
	}
}

static bool send_rtp_connect(struct mncc_call_leg *leg, struct call_leg *other)
{
	struct gsm_mncc_rtp mncc = { 0, };
	int rc;

	/*
	 * Send RTP CONNECT and we handle the general failure of it by
	 * tearing down the call.
	 */
	mncc.msg_type = MNCC_RTP_CONNECT;
	mncc.callref = leg->callref;
	mncc.ip = htonl(other->ip);
	mncc.port = other->port;
	mncc.payload_type = other->payload_type;
	/*
	 * FIXME: mncc.payload_msg_type should already be compatible.. but
	 * payload_type should be different..
	 */
	rc = write(leg->conn->fd.fd, &mncc, sizeof(mncc));
	if (rc != sizeof(mncc)) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to send message leg(%u)\n",
			leg->callref);
		close_connection(leg->conn);
		return false;
	}
	return true;
}

static void mncc_call_leg_connect(struct call_leg *_leg)
{
	struct mncc_call_leg *leg;
	struct call_leg *other;

	OSMO_ASSERT(_leg->type == CALL_TYPE_MNCC);
	leg = (struct mncc_call_leg *) _leg;

	other = call_leg_other(_leg);
	OSMO_ASSERT(other);

	if (!send_rtp_connect(leg, other))
		return;

	start_cmd_timer(leg, MNCC_SETUP_COMPL_IND);
	mncc_send(leg->conn, MNCC_SETUP_RSP, leg->callref);
}

static void mncc_call_leg_ring(struct call_leg *_leg)
{
	struct gsm_mncc out_mncc = { 0, };
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	OSMO_ASSERT(_leg->type == CALL_TYPE_MNCC);
	leg = (struct mncc_call_leg *) _leg;

	mncc_fill_header(&out_mncc, MNCC_ALERT_REQ, leg->callref);
	/* GSM 04.08 10.5.4.21 */
	out_mncc.fields |= MNCC_F_PROGRESS;
	out_mncc.progress.coding = 3; /* Standard defined for the GSMßPLMNS */
	out_mncc.progress.location = 1; /* Private network serving the local user */
	out_mncc.progress.descr = 8; /* In-band information or appropriate pattern now available */

	mncc_write(leg->conn, &out_mncc, leg->callref);

	/*
	 * If we have remote IP/port let's connect it already.
	 * FIXME: We would like to keep this as recvonly...
	 */
	other_leg = call_leg_other(&leg->base);
	if (other_leg && other_leg->port != 0 && other_leg->ip != 0)
		send_rtp_connect(leg, other_leg);
}

static void mncc_call_leg_release(struct call_leg *_leg)
{
	struct mncc_call_leg *leg;

	OSMO_ASSERT(_leg->type == CALL_TYPE_MNCC);
	leg = (struct mncc_call_leg *) _leg;

	/* drop it directly, if not connected */
	if (leg->conn->state != MNCC_READY) {
		LOGP(DMNCC, LOGL_DEBUG,
			"MNCC not connected releasing leg leg(%u)\n", leg->callref);
		return mncc_leg_release(leg);
	}

	switch (leg->state) {
	case MNCC_CC_INITIAL:
		LOGP(DMNCC, LOGL_DEBUG,
			"Releasing call in initial-state leg(%u)\n", leg->callref);
		if (leg->dir == MNCC_DIR_MO) {
			mncc_send(leg->conn, MNCC_REJ_REQ, leg->callref);
			osmo_timer_del(&leg->cmd_timeout);
			mncc_leg_release(leg);
		} else {
			leg->base.in_release = true;
			start_cmd_timer(leg, MNCC_REL_CNF);
			mncc_send(leg->conn, MNCC_REL_REQ, leg->callref);
		}
		break;
	case MNCC_CC_PROCEEDING:
	case MNCC_CC_CONNECTED:
		LOGP(DMNCC, LOGL_DEBUG,
			"Releasing call in non-initial leg(%u)\n", leg->callref);
		leg->base.in_release = true;
		start_cmd_timer(leg, MNCC_REL_IND);
		mncc_send(leg->conn, MNCC_DISC_REQ, leg->callref);
		break;
	default:
		LOGP(DMNCC, LOGL_ERROR, "Unknown state leg(%u) state(%d)\n",
			leg->callref, leg->state);
		break;
	}
}

static void close_connection(struct mncc_connection *conn)
{
	osmo_fd_unregister(&conn->fd);
	close(conn->fd.fd);
	osmo_timer_schedule(&conn->reconnect, 5, 0);
	conn->state = MNCC_DISCONNECTED;
	if (conn->on_disconnect)
		conn->on_disconnect(conn);
}

static void continue_mo_call(struct mncc_call_leg *leg)
{
	char *dest, *source;

	/* TODO.. continue call obviously only for MO call right now */
	mncc_send(leg->conn, MNCC_CALL_PROC_REQ, leg->callref);
	leg->state = MNCC_CC_PROCEEDING;

	if (leg->called.type == GSM340_TYPE_INTERNATIONAL)
		dest = talloc_asprintf(leg, "+%.32s", leg->called.number);
	else
		dest = talloc_asprintf(leg, "%.32s", leg->called.number);

	if (leg->conn->app->use_imsi_as_id)
		source = talloc_asprintf(leg, "%.16s", leg->imsi);
	else
		source = talloc_asprintf(leg, "%.32s", leg->calling.number);

	app_route_call(leg->base.call, source, dest);
}

static void continue_mt_call(struct mncc_call_leg *leg)
{
	struct call_leg *other_leg;

	/* TODO.. check codec selection */
	other_leg = call_leg_other(&leg->base);
	if (!other_leg)
		return;

	/* assume the type is compatible */
	other_leg->payload_type = leg->base.payload_type;
}

static void continue_call(struct mncc_call_leg *leg)
{
	if (leg->dir == MNCC_DIR_MO)
		return continue_mo_call(leg);
	return continue_mt_call(leg);
}

static void check_rtp_connect(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc_rtp *rtp;
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	if (rc < sizeof(*rtp)) {
		LOGP(DMNCC, LOGL_ERROR, "gsm_mncc_rtp of wrong size %d < %zu\n",
			rc, sizeof(*rtp));
		return close_connection(conn);
	}

	rtp = (struct gsm_mncc_rtp *) buf;
	leg = mncc_find_leg(rtp->callref);
	if (!leg) {
		LOGP(DMNCC, LOGL_ERROR, "leg(%u) can not be found\n", rtp->callref);
		return mncc_send(conn, MNCC_REJ_REQ, rtp->callref);
	}

	/* extract information about where the RTP is */
	if (rtp->ip != 0 || rtp->port != 0 || rtp->payload_type != 0)
		return;

	LOGP(DMNCC, LOGL_ERROR, "leg(%u) rtp connect failed\n", rtp->callref);

	other_leg = call_leg_other(&leg->base);
	if (other_leg)
		other_leg->release_call(other_leg);
	leg->base.release_call(&leg->base);
}

static void check_rtp_create(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc_rtp *rtp;
	struct mncc_call_leg *leg;

	if (rc < sizeof(*rtp)) {
		LOGP(DMNCC, LOGL_ERROR, "gsm_mncc_rtp of wrong size %d < %zu\n",
			rc, sizeof(*rtp));
		return close_connection(conn);
	}

	rtp = (struct gsm_mncc_rtp *) buf;
	leg = mncc_find_leg(rtp->callref);
	if (!leg) {
		LOGP(DMNCC, LOGL_ERROR, "call(%u) can not be found\n", rtp->callref);
		return mncc_send(conn, MNCC_REJ_REQ, rtp->callref);
	}

	/* extract information about where the RTP is */
	leg->base.ip = rtp->ip;
	leg->base.port = rtp->port;
	leg->base.payload_type = rtp->payload_type;
	leg->base.payload_msg_type = rtp->payload_msg_type;

	/* TODO.. now we can continue with the call */
	LOGP(DMNCC, LOGL_DEBUG,
		"RTP cnt leg(%u) ip(%u), port(%u) pt(%u) ptm(%u)\n",
		leg->callref, leg->base.ip, leg->base.port,
		leg->base.payload_type, leg->base.payload_msg_type);
	stop_cmd_timer(leg, MNCC_RTP_CREATE);
	continue_call(leg);
}

static int continue_setup(struct mncc_connection *conn, struct gsm_mncc *mncc)
{
	if (mncc->called.plan != GSM340_PLAN_ISDN) {
		LOGP(DMNCC, LOGL_ERROR,
			"leg(%u) has non(%d) ISDN dial plan. not supported.\n",
			mncc->callref, mncc->called.plan);
		return 0;
	}

	return 1;
}

static void check_setup(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct call *call;
	struct mncc_call_leg *leg;

	if (rc != sizeof(*data)) {
		LOGP(DMNCC, LOGL_ERROR, "gsm_mncc of wrong size %d vs. %zu\n",
			rc, sizeof(*data));
		return close_connection(conn);
	}

	data = (struct gsm_mncc *) buf;

	/* screen arguments */
	if ((data->fields & MNCC_F_CALLED) == 0) {
		LOGP(DMNCC, LOGL_ERROR,
			"MNCC leg(%u) without called addr fields(%u)\n",
			data->callref, data->fields);
		return mncc_send(conn, MNCC_REJ_REQ, data->callref);
	}
	if ((data->fields & MNCC_F_CALLING) == 0) {
		LOGP(DMNCC, LOGL_ERROR,
			"MNCC leg(%u) without calling addr fields(%u)\n",
			data->callref, data->fields);
		return mncc_send(conn, MNCC_REJ_REQ, data->callref);
	}

	/* TODO.. bearer caps and better audio handling */
	if (!continue_setup(conn, data)) {
		LOGP(DMNCC, LOGL_ERROR,
			"MNCC screening parameters failed leg(%u)\n", data->callref);
		return mncc_send(conn, MNCC_REJ_REQ, data->callref);
	}

	/* Create an RTP port and then allocate a call */
	call = call_mncc_create();
	if (!call) {
		LOGP(DMNCC, LOGL_ERROR,
			"MNCC leg(%u) failed to allocate call\n", data->callref);
		return mncc_send(conn, MNCC_REJ_REQ, data->callref);
	}

	leg = (struct mncc_call_leg *) call->initial;
	leg->base.connect_call = mncc_call_leg_connect;
	leg->base.ring_call = mncc_call_leg_ring;
	leg->base.release_call = mncc_call_leg_release;
	leg->callref = data->callref;
	leg->conn = conn;
	leg->state = MNCC_CC_INITIAL;
	leg->dir = MNCC_DIR_MO;
	memcpy(&leg->called, &data->called, sizeof(leg->called));
	memcpy(&leg->calling, &data->calling, sizeof(leg->calling));
	memcpy(&leg->imsi, data->imsi, sizeof(leg->imsi));

	LOGP(DMNCC, LOGL_DEBUG,
		"Created call(%u) with MNCC leg(%u) IMSI(%.16s)\n",
		call->id, leg->callref, data->imsi);

	start_cmd_timer(leg, MNCC_RTP_CREATE);
	mncc_rtp_send(conn, MNCC_RTP_CREATE, data->callref);
}

static struct mncc_call_leg *find_leg(struct mncc_connection *conn,
					char *buf, int rc, struct gsm_mncc **mncc)
{
	struct mncc_call_leg *leg;

	if (rc != sizeof(**mncc)) {
		LOGP(DMNCC, LOGL_ERROR, "gsm_mncc of wrong size %d vs. %zu\n",
			rc, sizeof(**mncc));
		close_connection(conn);
		return NULL;
	}

	*mncc = (struct gsm_mncc *) buf;
	leg = mncc_find_leg((*mncc)->callref);
	if (!leg) {
		LOGP(DMNCC, LOGL_ERROR, "call(%u) can not be found\n", (*mncc)->callref);
		return NULL;
	}

	return leg;
}

static void check_disc_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC,
		LOGL_DEBUG, "leg(%u) was disconnected. Releasing\n", data->callref);
	leg->base.in_release = true;
	start_cmd_timer(leg, MNCC_REL_CNF);
	mncc_send(leg->conn, MNCC_REL_REQ, leg->callref);

	other_leg = call_leg_other(&leg->base);
	if (other_leg)
		other_leg->release_call(other_leg);
}

static void check_rel_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	if (leg->base.in_release)
		stop_cmd_timer(leg, MNCC_REL_IND);
	else {
		struct call_leg *other_leg;
		other_leg = call_leg_other(&leg->base);
		if (other_leg)
			other_leg->release_call(other_leg);
	}
	LOGP(DMNCC, LOGL_DEBUG, "leg(%u) was released.\n", data->callref);
	mncc_leg_release(leg);
}

static void check_rel_cnf(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	stop_cmd_timer(leg, MNCC_REL_CNF);
	LOGP(DMNCC, LOGL_DEBUG, "leg(%u) was cnf released.\n", data->callref);
	mncc_leg_release(leg);
}

static void check_stp_cmpl_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_NOTICE, "leg(%u) is now connected.\n", leg->callref);
	stop_cmd_timer(leg, MNCC_SETUP_COMPL_IND);
	leg->state = MNCC_CC_CONNECTED;
}

static void check_rej_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	other_leg = call_leg_other(&leg->base);
	if (other_leg)
		other_leg->release_call(other_leg);
	LOGP(DMNCC, LOGL_DEBUG, "leg(%u) was rejected.\n", data->callref);
	mncc_leg_release(leg);
}

static void check_cnf_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_DEBUG,
		"leg(%u) confirmend. creating RTP socket.\n",
		leg->callref);

	start_cmd_timer(leg, MNCC_RTP_CREATE);
	mncc_rtp_send(conn, MNCC_RTP_CREATE, data->callref);
}

static void check_alrt_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_DEBUG,
		"leg(%u) is alerting.\n", leg->callref);

	other_leg = call_leg_other(&leg->base);
	if (!other_leg) {
		LOGP(DMNCC, LOGL_ERROR, "leg(%u) other leg gone!\n",
			leg->callref);
		mncc_call_leg_release(&leg->base);
		return;
	}

	other_leg->ring_call(other_leg);
}

static void check_hold_ind(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_DEBUG,
		"leg(%u) is req hold. rejecting.\n", leg->callref);
	mncc_send(leg->conn, MNCC_HOLD_REJ, leg->callref);
}

static void check_stp_cnf(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_DEBUG, "leg(%u) setup completed\n", leg->callref);

	other_leg = call_leg_other(&leg->base);
	if (!other_leg) {
		LOGP(DMNCC, LOGL_ERROR, "leg(%u) other leg gone!\n",
			leg->callref);
		mncc_call_leg_release(&leg->base);
		return;
	}

	if (!send_rtp_connect(leg, other_leg))
		return;
	leg->state = MNCC_CC_CONNECTED;
	mncc_send(leg->conn, MNCC_SETUP_COMPL_REQ, leg->callref);

	other_leg->connect_call(other_leg);
}

static void check_dtmf_start(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc out_mncc = { 0, };
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;
	struct call_leg *other_leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_DEBUG, "leg(%u) DTMF key=%c\n", leg->callref, data->keypad);

	other_leg = call_leg_other(&leg->base);
	if (other_leg && other_leg->dtmf)
		other_leg->dtmf(other_leg, data->keypad);

	mncc_fill_header(&out_mncc, MNCC_START_DTMF_RSP, leg->callref);
	out_mncc.fields |= MNCC_F_KEYPAD;
	out_mncc.keypad = data->keypad;
	mncc_write(conn, &out_mncc, leg->callref);
}

static void check_dtmf_stop(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc out_mncc = { 0, };
	struct gsm_mncc *data;
	struct mncc_call_leg *leg;

	leg = find_leg(conn, buf, rc, &data);
	if (!leg)
		return;

	LOGP(DMNCC, LOGL_DEBUG, "leg(%u) DTMF key=%c\n", leg->callref, data->keypad);

	mncc_fill_header(&out_mncc, MNCC_STOP_DTMF_RSP, leg->callref);
	out_mncc.fields |= MNCC_F_KEYPAD;
	out_mncc.keypad = data->keypad;
	mncc_write(conn, &out_mncc, leg->callref);
}

static void check_hello(struct mncc_connection *conn, char *buf, int rc)
{
	struct gsm_mncc_hello *hello;

	if (rc != sizeof(*hello)) {
		LOGP(DMNCC, LOGL_ERROR, "Hello shorter than expected %d vs. %zu\n",
			rc, sizeof(*hello));
		return close_connection(conn);
	}

	hello = (struct gsm_mncc_hello *) buf;
	LOGP(DMNCC, LOGL_NOTICE, "Got hello message version %d\n", hello->version);

	if (hello->version != MNCC_SOCK_VERSION) {
		LOGP(DMNCC, LOGL_NOTICE, "Incompatible version(%d) expected %d\n",
			hello->version, MNCC_SOCK_VERSION);
		return close_connection(conn);
	}

	conn->state = MNCC_READY;
}

int mncc_create_remote_leg(struct mncc_connection *conn, struct call *call)
{
	struct mncc_call_leg *leg;
	struct gsm_mncc mncc = { 0, };
	int rc;

	leg = talloc_zero(call, struct mncc_call_leg);
	if (!leg) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to allocate leg call(%u)\n",
			call->id);
		return -1;
	}

	leg->base.type = CALL_TYPE_MNCC;
	leg->base.connect_call = mncc_call_leg_connect;
	leg->base.ring_call = mncc_call_leg_ring;
	leg->base.release_call = mncc_call_leg_release;
	leg->base.call = call;

	leg->callref = call->id;

	leg->conn = conn;
	leg->state = MNCC_CC_INITIAL;
	leg->dir = MNCC_DIR_MT;

	mncc.msg_type = MNCC_SETUP_REQ;
	mncc.callref = leg->callref;

	mncc.fields |= MNCC_F_CALLING;
	mncc.calling.plan = 1;
	mncc.calling.type = 0x0;
	strncpy(mncc.calling.number, call->source, sizeof(mncc.calling.number));

	if (conn->app->use_imsi_as_id) {
		snprintf(mncc.imsi, 15, "%s", call->dest);
	} else {
		mncc.fields |= MNCC_F_CALLED;
		mncc.called.plan = 1;
		mncc.called.type = 0x0;
		strncpy(mncc.called.number, call->dest, sizeof(mncc.called.number));
	}

	/*
	 * TODO/FIXME:
	 *  - Determine/request channel based on offered audio codecs
	 *  - Screening, redirect?
	 *  - Synth. the bearer caps based on codecs?
	 */
	rc = write(conn->fd.fd, &mncc, sizeof(mncc));
	if (rc != sizeof(mncc)) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to send message leg(%u)\n",
			leg->callref);
		close_connection(conn);
		talloc_free(leg);
		return -1;
	}

	call->remote = &leg->base;
	return 0;
}

static void mncc_reconnect(void *data)
{
	int rc;
	struct mncc_connection *conn = data;

	rc = osmo_sock_unix_init_ofd(&conn->fd, SOCK_SEQPACKET, 0,
					conn->app->mncc.path, OSMO_SOCK_F_CONNECT);
	if (rc < 0) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to connect(%s). Retrying\n",
			conn->app->mncc.path);
		conn->state = MNCC_DISCONNECTED;
		osmo_timer_schedule(&conn->reconnect, 5, 0);
		return;
	}

	LOGP(DMNCC, LOGL_NOTICE, "Reconnected to %s\n", conn->app->mncc.path);
	conn->state = MNCC_WAIT_VERSION;
}

static int mncc_data(struct osmo_fd *fd, unsigned int what)
{
	char buf[4096];
	uint32_t msg_type;
	int rc;
	struct mncc_connection *conn = fd->data;

	rc = read(fd->fd, buf, sizeof(buf));
	if (rc <= 0) {
		LOGP(DMNCC, LOGL_ERROR, "Failed to read %d/%s. Re-connecting.\n",
			rc, strerror(errno));
		goto bad_data;
	}
	if (rc <= 4) {
		LOGP(DMNCC, LOGL_ERROR, "Data too short with: %d\n", rc);
		goto bad_data;
	}

	memcpy(&msg_type, buf, 4);
	switch (msg_type) {
	case MNCC_SOCKET_HELLO:
		check_hello(conn, buf, rc);
		break;
	case MNCC_SETUP_IND:
		check_setup(conn, buf, rc);
		break;
	case MNCC_RTP_CREATE:
		check_rtp_create(conn, buf, rc);
		break;
	case MNCC_RTP_CONNECT:
		check_rtp_connect(conn, buf, rc);
		break;
	case MNCC_DISC_IND:
		check_disc_ind(conn, buf, rc);
		break;
	case MNCC_REL_IND:
		check_rel_ind(conn, buf, rc);
		break;
	case MNCC_REJ_IND:
		check_rej_ind(conn, buf, rc);
		break;
	case MNCC_REL_CNF:
		check_rel_cnf(conn, buf, rc);
		break;
	case MNCC_SETUP_COMPL_IND:
		check_stp_cmpl_ind(conn, buf, rc);
		break;
	case MNCC_SETUP_CNF:
		check_stp_cnf(conn, buf, rc);
		break;
	case MNCC_CALL_CONF_IND:
		check_cnf_ind(conn, buf, rc);
		break;
	case MNCC_ALERT_IND:
		check_alrt_ind(conn, buf, rc);
		break;
	case MNCC_HOLD_IND:
		check_hold_ind(conn, buf, rc);
		break;
	case MNCC_START_DTMF_IND:
		check_dtmf_start(conn, buf, rc);
		break;
	case MNCC_STOP_DTMF_IND:
		check_dtmf_stop(conn, buf, rc);
		break;
	default:
		LOGP(DMNCC, LOGL_ERROR, "Unhandled message type %d/0x%x\n",
			msg_type, msg_type);
		break;
	}
	return 0;

bad_data:
	close_connection(conn);
	return 0;
}

void mncc_connection_init(struct mncc_connection *conn, struct app_config *cfg)
{
	conn->reconnect.cb = mncc_reconnect;
	conn->reconnect.data = conn;
	conn->fd.cb = mncc_data;
	conn->fd.data = conn;
	conn->app = cfg;
	conn->state = MNCC_DISCONNECTED;
}

void mncc_connection_start(struct mncc_connection *conn)
{
	LOGP(DMNCC, LOGL_NOTICE, "Scheduling MNCC connect\n");
	osmo_timer_schedule(&conn->reconnect, 0, 0);
}

const struct value_string mncc_conn_state_vals[] = {
	{ MNCC_DISCONNECTED,	"DISCONNECTED"	},
	{ MNCC_WAIT_VERSION,	"WAITING"	},
	{ MNCC_READY,		"READY"		},
	{ 0, NULL },
};
