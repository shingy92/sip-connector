/*
 * (C) 2016 by Holger Hans Peter Freyther
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

#include "app.h"
#include "call.h"
#include "logging.h"
#include "mncc.h"

void app_mncc_disconnected(struct mncc_connection *conn)
{
	struct call *call, *tmp;

	llist_for_each_entry_safe(call, tmp, &g_call_list, entry) {
		int has_mncc = 0;

		if (call->initial && call->initial->type == CALL_TYPE_MNCC)
			has_mncc = 1;
		if (call->remote && call->remote->type == CALL_TYPE_MNCC)
			has_mncc = 1;

		if (!has_mncc)
			continue;

		/*
		 * this call has a MNCC component and we will release it.
		 */
		LOGP(DAPP, LOGL_NOTICE,
			"Going to release call(%u) due MNCC.\n", call->id);
		call_leg_release(call, call->initial);
		call_leg_release(call, call->remote);
	}
}

/*
 * I hook SIP and MNCC together.
 */
void app_setup(struct app_config *cfg)
{
	cfg->mncc.conn.on_disconnect = app_mncc_disconnected;
}