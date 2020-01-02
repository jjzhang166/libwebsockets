/*
 * libwebsockets - small server side websockets and web server implementation
 *
 * Copyright (C) 2010 - 2019 Andy Green <andy@warmcat.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <private-lib-core.h>

static int
rops_handle_POLLIN_raw_skt(struct lws_context_per_thread *pt, struct lws *wsi,
			   struct lws_pollfd *pollfd)
{
	struct lws_tokens ebuf;
	int n, buffered;

	/* pending truncated sends have uber priority */

	if (lws_has_buffered_out(wsi)) {
		if (!(pollfd->revents & LWS_POLLOUT))
			return LWS_HPI_RET_HANDLED;

		/* drain the output buflist */
		if (lws_issue_raw(wsi, NULL, 0) < 0)
			goto fail;
		/*
		 * we can't afford to allow input processing to send
		 * something new, so spin around he event loop until
		 * he doesn't have any partials
		 */
		return LWS_HPI_RET_HANDLED;
	}


#if defined(LWS_WITH_SERVER)
	if (!lwsi_role_client(wsi) &&  lwsi_state(wsi) != LRS_ESTABLISHED) {

		lwsl_debug("%s: %p: wsistate 0x%x\n", __func__, wsi,
			   wsi->wsistate);

		if (lwsi_state(wsi) != LRS_SSL_INIT)
			if (lws_server_socket_service_ssl(wsi,
							  LWS_SOCK_INVALID))
				return LWS_HPI_RET_PLEASE_CLOSE_ME;

		return LWS_HPI_RET_HANDLED;
	}
#endif

	if ((pollfd->revents & pollfd->events & LWS_POLLIN) &&
	    /* any tunnel has to have been established... */
	    lwsi_state(wsi) != LRS_SSL_ACK_PENDING &&
	    /* we are actually connected */
	    lwsi_state(wsi) != LRS_WAITING_CONNECT &&
	    !(wsi->favoured_pollin &&
	      (pollfd->revents & pollfd->events & LWS_POLLOUT))) {

		buffered = lws_buflist_aware_read(pt, wsi, &ebuf, __func__);
		switch (ebuf.len) {
		case 0:
			lwsl_info("%s: read 0 len\n", __func__);
			wsi->seen_zero_length_recv = 1;
			if (lws_change_pollfd(wsi, LWS_POLLIN, 0))
				goto fail;

			/*
			 * we need to go to fail here, since it's the only
			 * chance we get to understand that the socket has
			 * closed
			 */
			// goto try_pollout;
			goto fail;

		case LWS_SSL_CAPABLE_ERROR:
			goto fail;
		case LWS_SSL_CAPABLE_MORE_SERVICE:
			goto try_pollout;
		}

#if defined(LWS_WITH_UDP)
		if (wsi->context->udp_loss_sim_rx_pc) {
			uint16_t u16;
			/*
			 * We should randomly drop some of these
			 */

			if (lws_get_random(wsi->context, &u16, 2) == 2 &&
			    ((u16 * 100) / 0xffff) <=
				    wsi->context->udp_loss_sim_rx_pc) {
				lwsl_warn("%s: dropping udp rx\n", __func__);
				/* pretend it was handled */
				n = ebuf.len;
				goto post_rx;
			}
		}
#endif

		n = user_callback_handle_rxflow(wsi->protocol->callback,
						wsi, LWS_CALLBACK_RAW_RX,
						wsi->user_space, ebuf.token,
						ebuf.len);
#if defined(LWS_WITH_UDP)
post_rx:
#endif
		if (n < 0) {
			lwsl_info("LWS_CALLBACK_RAW_RX_fail\n");
			goto fail;
		}

		if (lws_buflist_aware_finished_consuming(wsi, &ebuf, ebuf.len,
							 buffered, __func__))
			return LWS_HPI_RET_PLEASE_CLOSE_ME;
	} else
		if (wsi->favoured_pollin &&
		    (pollfd->revents & pollfd->events & LWS_POLLOUT))
			/* we balanced the last favouring of pollin */
			wsi->favoured_pollin = 0;

try_pollout:

	if (!(pollfd->revents & LWS_POLLOUT))
		return LWS_HPI_RET_HANDLED;

#if defined(LWS_WITH_CLIENT)
	if (lwsi_state(wsi) == LRS_WAITING_CONNECT &&
	    !lws_client_connect_3_connect(wsi, NULL, NULL, 0, NULL))
		return LWS_HPI_RET_WSI_ALREADY_DIED;
#endif

	/* one shot */
	if (lws_change_pollfd(wsi, LWS_POLLOUT, 0)) {
		lwsl_notice("%s a\n", __func__);
		goto fail;
	}

	/* clear back-to-back write detection */
	wsi->could_have_pending = 0;

	lws_stats_bump(pt, LWSSTATS_C_WRITEABLE_CB, 1);
#if defined(LWS_WITH_STATS)
	if (wsi->active_writable_req_us) {
		uint64_t ul = lws_now_usecs() -
				wsi->active_writable_req_us;

		lws_stats_bump(pt, LWSSTATS_US_WRITABLE_DELAY_AVG, ul);
		lws_stats_max(pt,
			  LWSSTATS_US_WORST_WRITABLE_DELAY, ul);
		wsi->active_writable_req_us = 0;
	}
#endif
	n = user_callback_handle_rxflow(wsi->protocol->callback,
			wsi, LWS_CALLBACK_RAW_WRITEABLE,
			wsi->user_space, NULL, 0);
	if (n < 0) {
		lwsl_info("writeable_fail\n");
		goto fail;
	}

	return LWS_HPI_RET_HANDLED;

fail:
	lws_close_free_wsi(wsi, LWS_CLOSE_STATUS_NOSTATUS, "raw svc fail");

	return LWS_HPI_RET_WSI_ALREADY_DIED;
}

#if defined(LWS_WITH_SERVER)
static int
rops_adoption_bind_raw_skt(struct lws *wsi, int type, const char *vh_prot_name)
{
	/* no http but socket... must be raw skt */
	if ((type & LWS_ADOPT_HTTP) || !(type & LWS_ADOPT_SOCKET) ||
	    (type & _LWS_ADOPT_FINISH))
		return 0; /* no match */

#if defined(LWS_WITH_UDP)
	if (type & LWS_ADOPT_FLAG_UDP)
		/*
		 * these can be >128 bytes, so just alloc for UDP
		 */
		wsi->udp = lws_malloc(sizeof(*wsi->udp), "udp struct");
#endif

	lws_role_transition(wsi, 0, (type & LWS_ADOPT_ALLOW_SSL) ? LRS_SSL_INIT :
				LRS_ESTABLISHED, &role_ops_raw_skt);

	if (vh_prot_name)
		lws_bind_protocol(wsi, wsi->protocol, __func__);
	else
		/* this is the only time he will transition */
		lws_bind_protocol(wsi,
			&wsi->vhost->protocols[wsi->vhost->raw_protocol_index],
			__func__);

	return 1; /* bound */
}
#endif

#if defined(LWS_WITH_CLIENT)
static int
rops_client_bind_raw_skt(struct lws *wsi,
			 const struct lws_client_connect_info *i)
{
	if (!i) {

		/* finalize */

		if (!wsi->user_space && wsi->stash->cis[CIS_METHOD])
			if (lws_ensure_user_space(wsi))
				return 1;

		return 0;
	}

	/* we are a fallback if nothing else matched */

	if (!i->local_protocol_name ||
	    strcmp(i->local_protocol_name, "raw-proxy"))
		lws_role_transition(wsi, LWSIFR_CLIENT, LRS_UNCONNECTED,
			    &role_ops_raw_skt);

	return 1; /* matched */
}
#endif

struct lws_role_ops role_ops_raw_skt = {
	/* role name */			"raw-skt",
	/* alpn id */			NULL,
	/* check_upgrades */		NULL,
	/* pt_init_destroy */		NULL,
	/* init_vhost */		NULL,
	/* destroy_vhost */		NULL,
	/* service_flag_pending */	NULL,
	/* handle_POLLIN */		rops_handle_POLLIN_raw_skt,
	/* handle_POLLOUT */		NULL,
	/* perform_user_POLLOUT */	NULL,
	/* callback_on_writable */	NULL,
	/* tx_credit */			NULL,
	/* write_role_protocol */	NULL,
	/* encapsulation_parent */	NULL,
	/* alpn_negotiated */		NULL,
	/* close_via_role_protocol */	NULL,
	/* close_role */		NULL,
	/* close_kill_connection */	NULL,
	/* destroy_role */		NULL,
#if defined(LWS_WITH_SERVER)
	/* adoption_bind */		rops_adoption_bind_raw_skt,
#else
					NULL,
#endif
#if defined(LWS_WITH_CLIENT)
	/* client_bind */		rops_client_bind_raw_skt,
#else
					NULL,
#endif
	/* issue_keepalive */		NULL,
	/* adoption_cb clnt, srv */	{ LWS_CALLBACK_RAW_CONNECTED,
					  LWS_CALLBACK_RAW_ADOPT },
	/* rx_cb clnt, srv */		{ LWS_CALLBACK_RAW_RX,
					  LWS_CALLBACK_RAW_RX },
	/* writeable cb clnt, srv */	{ LWS_CALLBACK_RAW_WRITEABLE,
					  LWS_CALLBACK_RAW_WRITEABLE},
	/* close cb clnt, srv */	{ LWS_CALLBACK_RAW_CLOSE,
					  LWS_CALLBACK_RAW_CLOSE },
	/* protocol_bind cb c, srv */	{ LWS_CALLBACK_RAW_SKT_BIND_PROTOCOL,
					  LWS_CALLBACK_RAW_SKT_BIND_PROTOCOL },
	/* protocol_unbind cb c, srv */	{ LWS_CALLBACK_RAW_SKT_DROP_PROTOCOL,
					  LWS_CALLBACK_RAW_SKT_DROP_PROTOCOL },
	/* file_handle */		0,
};
