/* Copyright (C) 2016-2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * This file is part of CaRT. It implements the CaRT context related APIs.
 */
#define D_LOGFAC	DD_FAC(rpc)

#include "crt_internal.h"

static void crt_epi_destroy(struct crt_ep_inflight *epi);

static struct crt_ep_inflight *
epi_link2ptr(d_list_t *rlink)
{
	D_ASSERT(rlink != NULL);
	return container_of(rlink, struct crt_ep_inflight, epi_link);
}

static int
epi_op_key_get(struct d_hash_table *hhtab, d_list_t *rlink, void **key_pp)
{
	struct crt_ep_inflight *epi = epi_link2ptr(rlink);

	/* TODO: use global rank */
	*key_pp = (void *)&epi->epi_ep.ep_rank;
	return sizeof(epi->epi_ep.ep_rank);
}

static uint32_t
epi_op_key_hash(struct d_hash_table *hhtab, const void *key,
		unsigned int ksize)
{
	D_ASSERT(ksize == sizeof(d_rank_t));

	return (unsigned int)(*(const uint32_t *)key %
		(1U << CRT_EPI_TABLE_BITS));
}

static bool
epi_op_key_cmp(struct d_hash_table *hhtab, d_list_t *rlink,
	  const void *key, unsigned int ksize)
{
	struct crt_ep_inflight *epi = epi_link2ptr(rlink);

	D_ASSERT(ksize == sizeof(d_rank_t));
	/* TODO: use global rank */

	return epi->epi_ep.ep_rank == *(d_rank_t *)key;
}

static void
epi_op_rec_addref(struct d_hash_table *hhtab, d_list_t *rlink)
{
	epi_link2ptr(rlink)->epi_ref++;
}

static bool
epi_op_rec_decref(struct d_hash_table *hhtab, d_list_t *rlink)
{
	struct crt_ep_inflight *epi = epi_link2ptr(rlink);

	epi->epi_ref--;
	return epi->epi_ref == 0;
}

static void
epi_op_rec_free(struct d_hash_table *hhtab, d_list_t *rlink)
{
	crt_epi_destroy(epi_link2ptr(rlink));
}

static d_hash_table_ops_t epi_table_ops = {
	.hop_key_get		= epi_op_key_get,
	.hop_key_hash		= epi_op_key_hash,
	.hop_key_cmp		= epi_op_key_cmp,
	.hop_rec_addref		= epi_op_rec_addref,
	.hop_rec_decref		= epi_op_rec_decref,
	.hop_rec_free		= epi_op_rec_free,
};

static void
crt_epi_destroy(struct crt_ep_inflight *epi)
{
	D_ASSERT(epi != NULL);

	D_ASSERT(epi->epi_ref == 0);
	D_ASSERT(epi->epi_initialized == 1);

	D_ASSERT(d_list_empty(&epi->epi_req_waitq));
	D_ASSERT(epi->epi_req_wait_num == 0);

	D_ASSERT(d_list_empty(&epi->epi_req_q));
	D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);

	/* crt_list_del_init(&epi->epi_link); */
	D_MUTEX_DESTROY(&epi->epi_mutex);

	D_FREE_PTR(epi);
}

static int
crt_context_init(crt_context_t crt_ctx)
{
	struct crt_context	*ctx;
	uint32_t		 bh_node_cnt;
	int			 rc;

	D_ASSERT(crt_ctx != NULL);
	ctx = crt_ctx;

	rc = D_MUTEX_INIT(&ctx->cc_mutex, NULL);
	if (rc != 0)
		D_GOTO(out, rc);

	D_INIT_LIST_HEAD(&ctx->cc_link);

	/* create timeout binheap */
	bh_node_cnt = CRT_DEFAULT_CREDITS_PER_EP_CTX * 64;
	rc = d_binheap_create_inplace(DBH_FT_NOLOCK, bh_node_cnt,
				      NULL /* priv */, &crt_timeout_bh_ops,
				      &ctx->cc_bh_timeout);
	if (rc != 0) {
		D_ERROR("d_binheap_create_inplace failed, rc: %d.\n", rc);
		D_MUTEX_DESTROY(&ctx->cc_mutex);
		D_GOTO(out, rc);
	}

	/* create epi table, use external lock */
	rc =  d_hash_table_create_inplace(D_HASH_FT_NOLOCK, CRT_EPI_TABLE_BITS,
					  NULL, &epi_table_ops,
					  &ctx->cc_epi_table);
	if (rc != 0) {
		D_ERROR("d_hash_table_create_inplace failed, rc: %d.\n", rc);
		d_binheap_destroy_inplace(&ctx->cc_bh_timeout);
		D_MUTEX_DESTROY(&ctx->cc_mutex);
		D_GOTO(out, rc);
	}


out:
	return rc;
}

int
crt_context_create(crt_context_t *crt_ctx)
{
	struct crt_context	*ctx = NULL;
	int			rc = 0;

	if (crt_ctx == NULL) {
		D_ERROR("invalid parameter of NULL crt_ctx.\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	if (crt_gdata.cg_share_na &&
	    crt_gdata.cg_ctx_num >= crt_gdata.cg_ctx_max_num) {
		D_ERROR("Number of active contexts (%d) reached limit (%d).\n",
			crt_gdata.cg_ctx_num, crt_gdata.cg_ctx_max_num);
		D_GOTO(out, -DER_AGAIN);
	}

	D_ALLOC_PTR(ctx);
	if (ctx == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	rc = crt_context_init(ctx);
	if (rc != 0) {
		D_ERROR("crt_context_init failed, rc: %d.\n", rc);
		D_FREE_PTR(ctx);
		D_GOTO(out, rc);
	}

	D_RWLOCK_WRLOCK(&crt_gdata.cg_rwlock);


	rc = crt_hg_ctx_init(&ctx->cc_hg_ctx, crt_gdata.cg_ctx_num);
	if (rc != 0) {
		D_ERROR("crt_hg_ctx_init failed rc: %d.\n", rc);
		crt_context_destroy(ctx, true);
		D_FREE_PTR(ctx);
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		D_GOTO(out, rc);
	}

	ctx->cc_idx = crt_gdata.cg_ctx_num;
	d_list_add_tail(&ctx->cc_link, &crt_gdata.cg_ctx_list);
	crt_gdata.cg_ctx_num++;

	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	*crt_ctx = (crt_context_t)ctx;

out:
	return rc;
}

int
crt_context_register_rpc_task(crt_context_t ctx, crt_rpc_task_t process_cb,
			      void *arg)
{
	struct crt_context *crt_ctx = ctx;

	if (ctx == CRT_CONTEXT_NULL || process_cb == NULL) {
		D_ERROR("Invalid parameter: ctx %p cb %p\n",
			ctx, process_cb);
		return -DER_INVAL;
	}

	crt_ctx->cc_rpc_cb = process_cb;
	crt_ctx->cc_rpc_cb_arg = arg;
	return 0;
}

void
crt_rpc_complete(struct crt_rpc_priv *rpc_priv, int rc)
{
	D_ASSERT(rpc_priv != NULL);

	if (rc == -DER_CANCELED)
		rpc_priv->crp_state = RPC_STATE_CANCELED;
	else if (rc == -DER_TIMEDOUT)
		rpc_priv->crp_state = RPC_STATE_TIMEOUT;
	else if (rc == -DER_UNREACH)
		rpc_priv->crp_state = RPC_STATE_FWD_UNREACH;
	else
		rpc_priv->crp_state = RPC_STATE_COMPLETED;

	if (rpc_priv->crp_complete_cb != NULL) {
		struct crt_cb_info	cbinfo;

		cbinfo.cci_rpc = &rpc_priv->crp_pub;
		cbinfo.cci_arg = rpc_priv->crp_arg;
		cbinfo.cci_rc = rc;
		if (cbinfo.cci_rc == 0)
			cbinfo.cci_rc = rpc_priv->crp_reply_hdr.cch_rc;
		if (cbinfo.cci_rc != 0)
			D_ERROR("rpc_priv %p (opc: %#x, to rank %d tag %d) "
				"failed, rc: %d.\n", rpc_priv,
				rpc_priv->crp_pub.cr_opc,
				rpc_priv->crp_pub.cr_ep.ep_rank,
				rpc_priv->crp_pub.cr_ep.ep_tag,
				cbinfo.cci_rc);
		rpc_priv->crp_complete_cb(&cbinfo);
	}
}

/* Flag bits definition for crt_ctx_epi_abort */
#define CRT_EPI_ABORT_FORCE	(0x1)
#define CRT_EPI_ABORT_WAIT	(0x2)

/* abort the RPCs in inflight queue and waitq in the epi. */
static int
crt_ctx_epi_abort(d_list_t *rlink, void *arg)
{
	struct crt_ep_inflight	*epi;
	struct crt_context	*ctx;
	struct crt_rpc_priv	*rpc_priv, *rpc_next;
	bool			 msg_logged;
	int			 flags, force, wait;
	uint64_t		 ts_start, ts_now;
	int			 rc = 0;

	D_ASSERT(rlink != NULL);
	D_ASSERT(arg != NULL);
	epi = epi_link2ptr(rlink);
	ctx = epi->epi_ctx;
	D_ASSERT(ctx != NULL);

	/* empty queue, nothing to do */
	if (d_list_empty(&epi->epi_req_waitq) &&
	    d_list_empty(&epi->epi_req_q))
		D_GOTO(out, rc = 0);

	flags = *(int *)arg;
	force = flags & CRT_EPI_ABORT_FORCE;
	wait = flags & CRT_EPI_ABORT_WAIT;
	if (force == 0) {
		D_ERROR("cannot abort endpoint (idx %d, rank %d, req_wait_num "
			DF_U64", req_num "DF_U64", reply_num "DF_U64", "
			"inflight "DF_U64", with force == 0.\n", ctx->cc_idx,
			epi->epi_ep.ep_rank, epi->epi_req_wait_num,
			epi->epi_req_num, epi->epi_reply_num,
			epi->epi_req_num - epi->epi_reply_num);
		D_GOTO(out, rc = -DER_BUSY);
	}

	/* abort RPCs in waitq */
	msg_logged = false;
	d_list_for_each_entry_safe(rpc_priv, rpc_next, &epi->epi_req_waitq,
				   crp_epi_link) {
		D_ASSERT(epi->epi_req_wait_num > 0);
		if (msg_logged == false) {
			D_DEBUG(DB_NET, "destroy context (idx %d, rank %d, "
				"req_wait_num "DF_U64").\n", ctx->cc_idx,
				epi->epi_ep.ep_rank, epi->epi_req_wait_num);
			msg_logged = true;
		}
		/* Just remove from wait_q, decrease the wait_num and destroy
		 * the request. Trigger the possible completion callback. */
		D_ASSERT(rpc_priv->crp_state == RPC_STATE_QUEUED);
		d_list_del_init(&rpc_priv->crp_epi_link);
		epi->epi_req_wait_num--;
		crt_rpc_complete(rpc_priv, -DER_CANCELED);
		/* corresponds to ref taken when adding to waitq */
		RPC_DECREF(rpc_priv);
	}

	/* abort RPCs in inflight queue */
	msg_logged = false;
	d_list_for_each_entry_safe(rpc_priv, rpc_next, &epi->epi_req_q,
				   crp_epi_link) {
		D_ASSERT(epi->epi_req_num > epi->epi_reply_num);
		if (msg_logged == false) {
			D_DEBUG(DB_NET,
				"destroy context (idx %d, rank %d, "
				"epi_req_num "DF_U64", epi_reply_num "
				""DF_U64", inflight "DF_U64").\n",
				ctx->cc_idx, epi->epi_ep.ep_rank,
				epi->epi_req_num, epi->epi_reply_num,
				epi->epi_req_num - epi->epi_reply_num);
			msg_logged = true;
		}

		rc = crt_req_abort(&rpc_priv->crp_pub);
		if (rc != 0) {
			D_DEBUG(DB_NET,
				"crt_req_abort(opc: %#x) failed, rc: %d.\n",
				rpc_priv->crp_pub.cr_opc, rc);
			rc = 0;
			continue;
		}
	}

	ts_start = d_timeus_secdiff(0);
	while (wait != 0) {
		/* make sure all above aborting finished */
		if (d_list_empty(&epi->epi_req_waitq) &&
		    d_list_empty(&epi->epi_req_q)) {
			wait = 0;
		} else {
			D_MUTEX_UNLOCK(&ctx->cc_mutex);
			rc = crt_progress(ctx, 1, NULL, NULL);
			D_MUTEX_LOCK(&ctx->cc_mutex);
			if (rc != 0 && rc != -DER_TIMEDOUT) {
				D_ERROR("crt_progress failed, rc %d.\n", rc);
				break;
			}
			ts_now = d_timeus_secdiff(0);
			if (ts_now - ts_start > 2 * CRT_DEFAULT_TIMEOUT_US) {
				D_ERROR("stop progress due to timed out.\n");
				rc = -DER_TIMEDOUT;
				break;
			}
		}
	}

out:
	return rc;
}

int
crt_context_destroy(crt_context_t crt_ctx, int force)
{
	struct crt_context	*ctx;
	int			flags;
	int			rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL) {
		D_ERROR("invalid parameter (NULL crt_ctx).\n");
		D_GOTO(out, rc = -DER_INVAL);
	}
	if (!crt_initialized()) {
		D_ERROR("CRT not initialized.\n");
		D_GOTO(out, rc = -DER_UNINIT);
	}

	ctx = crt_ctx;
	rc = crt_grp_ctx_invalid(ctx, false /* locked */);
	if (rc != 0) {
		D_ERROR("crt_grp_ctx_invalid failed, rc: %d.\n", rc);
		D_GOTO(out, rc);
	}

	flags = (force != 0) ? (CRT_EPI_ABORT_FORCE | CRT_EPI_ABORT_WAIT) : 0;
	D_MUTEX_LOCK(&ctx->cc_mutex);

	rc = d_hash_table_traverse(&ctx->cc_epi_table, crt_ctx_epi_abort,
				   &flags);
	if (rc != 0) {
		D_DEBUG(DB_TRACE, "destroy context (idx %d, force %d), "
			"d_hash_table_traverse failed rc: %d.\n",
			ctx->cc_idx, force, rc);
		D_MUTEX_UNLOCK(&ctx->cc_mutex);
		D_GOTO(out, rc);
	}

	rc = d_hash_table_destroy_inplace(&ctx->cc_epi_table,
					  true /* force */);
	if (rc != 0) {
		D_ERROR("destroy context (idx %d, force %d), "
			"d_hash_table_destroy_inplace failed, rc: %d.\n",
			ctx->cc_idx, force, rc);
		D_MUTEX_UNLOCK(&ctx->cc_mutex);
		D_GOTO(out, rc);
	}

	d_binheap_destroy_inplace(&ctx->cc_bh_timeout);

	D_MUTEX_UNLOCK(&ctx->cc_mutex);
	D_MUTEX_DESTROY(&ctx->cc_mutex);

	rc = crt_hg_ctx_fini(&ctx->cc_hg_ctx);
	if (rc == 0) {
		D_RWLOCK_WRLOCK(&crt_gdata.cg_rwlock);
		crt_gdata.cg_ctx_num--;
		d_list_del_init(&ctx->cc_link);
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);
		D_FREE_PTR(ctx);
	} else {
		D_ERROR("crt_hg_ctx_fini failed rc: %d.\n", rc);
	}

out:
	return rc;
}

int
crt_ep_abort(crt_endpoint_t *ep)
{
	struct crt_context	*ctx = NULL;
	d_list_t		*rlink;
	int			 flags;
	int			 rc = 0;

	D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);

	d_list_for_each_entry(ctx, &crt_gdata.cg_ctx_list, cc_link) {
		rc = 0;
		D_MUTEX_LOCK(&ctx->cc_mutex);
		rlink = d_hash_rec_find(&ctx->cc_epi_table,
					(void *)&ep->ep_rank,
					sizeof(ep->ep_rank));
		if (rlink != NULL) {
			flags = CRT_EPI_ABORT_FORCE;
			rc = crt_ctx_epi_abort(rlink, &flags);
			d_hash_rec_decref(&ctx->cc_epi_table, rlink);
		}
		D_MUTEX_UNLOCK(&ctx->cc_mutex);
		if (rc != 0) {
			D_ERROR("context (idx %d), ep_abort (rank %d), "
				"failed rc: %d.\n",
				ctx->cc_idx, ep->ep_rank, rc);
			break;
		}
	}

	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	return rc;
}

/* caller should already hold crt_ctx->cc_mutex */
int
crt_req_timeout_track(crt_rpc_t *req)
{
	struct crt_context	*crt_ctx;
	struct crt_rpc_priv	*rpc_priv;
	int			 rc;

	crt_ctx = req->cr_ctx;
	D_ASSERT(crt_ctx != NULL);
	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);

	if (rpc_priv->crp_in_binheap == 1)
		D_GOTO(out, rc = 0);

	/* add to binheap for timeout tracking */
	RPC_ADDREF(rpc_priv); /* decref in crt_req_timeout_untrack */
	rc = d_binheap_insert(&crt_ctx->cc_bh_timeout,
			      &rpc_priv->crp_timeout_bp_node);
	if (rc == 0) {
		rpc_priv->crp_in_binheap = 1;
	} else {
		D_ERROR("rpc_priv %p (opc %#x), d_binheap_insert "
			"failed, rc: %d.\n", rpc_priv,
			rpc_priv->crp_pub.cr_opc, rc);
		RPC_DECREF(rpc_priv);
	}

out:
	return rc;
}

/* caller should already hold crt_ctx->cc_mutex */
void
crt_req_timeout_untrack(crt_rpc_t *req)
{
	struct crt_context	*crt_ctx;
	struct crt_rpc_priv	*rpc_priv;

	crt_ctx = req->cr_ctx;
	D_ASSERT(crt_ctx != NULL);
	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);

	/* remove from timeout binheap */
	if (rpc_priv->crp_in_binheap == 1) {
		rpc_priv->crp_in_binheap = 0;
		d_binheap_remove(&crt_ctx->cc_bh_timeout,
				 &rpc_priv->crp_timeout_bp_node);
		RPC_DECREF(rpc_priv); /* addref in crt_req_timeout_track */
	}
}

static void
crt_exec_timeout_cb(struct crt_rpc_priv *rpc_priv)
{
	struct crt_timeout_cb_priv	*timeout_cb_priv;
	d_list_t			*curr_node;
	d_list_t			*tmp_node;

	if (crt_plugin_gdata.cpg_inited == 0)
		return;
	if (rpc_priv == NULL) {
		D_ERROR("Invalid parameter, rpc_priv == NULL\n");
		return;
	}
	D_RWLOCK_RDLOCK(&crt_plugin_gdata.cpg_timeout_rwlock);
	d_list_for_each_safe(curr_node, tmp_node,
			     &crt_plugin_gdata.cpg_timeout_cbs) {
		timeout_cb_priv =
			container_of(curr_node, struct crt_timeout_cb_priv,
				     ctcp_link);
		D_RWLOCK_UNLOCK(&crt_plugin_gdata.cpg_timeout_rwlock);
		timeout_cb_priv->ctcp_func(rpc_priv->crp_pub.cr_ctx,
					   &rpc_priv->crp_pub,
					   timeout_cb_priv->ctcp_args);
		D_RWLOCK_RDLOCK(&crt_plugin_gdata.cpg_timeout_rwlock);
	}
	D_RWLOCK_UNLOCK(&crt_plugin_gdata.cpg_timeout_rwlock);
}

static bool
crt_req_timeout_reset(struct crt_rpc_priv *rpc_priv)
{
	struct crt_opc_info	*opc_info;
	struct crt_context	*crt_ctx;
	crt_endpoint_t		*tgt_ep;
	bool			 is_evicted;
	int			 rc;

	crt_ctx = rpc_priv->crp_pub.cr_ctx;
	opc_info = rpc_priv->crp_opc_info;
	D_ASSERT(opc_info != NULL);

	if (opc_info->coi_reset_timer == 0) {
		D_DEBUG(DB_NET, "rpc_priv %p reset_timer not enabled.\n",
			rpc_priv);
		return false;
	}
	if (rpc_priv->crp_state == RPC_STATE_CANCELED ||
	    rpc_priv->crp_state == RPC_STATE_COMPLETED) {
		D_DEBUG(DB_NET,
			"rpc_priv %p state %#x, not reseting timer.\n",
			rpc_priv, rpc_priv->crp_state);
		return false;
	}

	tgt_ep = &rpc_priv->crp_pub.cr_ep;
	is_evicted = crt_rank_evicted(tgt_ep->ep_grp, tgt_ep->ep_rank);
	if (is_evicted) {
		D_DEBUG(DB_NET,
			"rpc_priv %p grp %p, rank %d already evicted.\n",
			rpc_priv, tgt_ep->ep_grp, tgt_ep->ep_rank);
		return false;
	}
	D_DEBUG(DB_NET, "rpc_priv %p reset_timer enabled.\n", rpc_priv);

	rpc_priv->crp_timeout_ts = crt_get_timeout(rpc_priv);
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	rc = crt_req_timeout_track(&rpc_priv->crp_pub);
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
	if (rc != 0) {
		D_ERROR("crt_req_timeout_track(opc: %#x) failed, rc: %d.\n",
			rpc_priv->crp_pub.cr_opc, rc);
		return false;
	}

	return true;
}

static inline void
crt_req_timeout_hdlr(struct crt_rpc_priv *rpc_priv)
{
	struct crt_grp_priv		*grp_priv;
	crt_endpoint_t			*tgt_ep;
	crt_rpc_t			*ul_req;
	struct crt_uri_lookup_in	*ul_in;

	if (crt_req_timeout_reset(rpc_priv)) {
		D_DEBUG(DB_NET,
			"rpc_opc: %#x reached timeout. Renewed for another cycle.\n",
			rpc_priv->crp_pub.cr_opc);
		return;
	};

	tgt_ep = &rpc_priv->crp_pub.cr_ep;
	if (tgt_ep->ep_grp == NULL)
		grp_priv = crt_gdata.cg_grp->gg_srv_pri_grp;
	else
		grp_priv = container_of(tgt_ep->ep_grp, struct crt_grp_priv,
					gp_pub);

	switch (rpc_priv->crp_state) {
	case RPC_STATE_URI_LOOKUP:
		ul_req = rpc_priv->crp_ul_req;
		D_ASSERT(ul_req != NULL);
		ul_in = crt_req_get(ul_req);
		D_ERROR("rpc opc: %#x timedout due to URI_LOOKUP to group %s, "
			"rank %d through PSR %d timedout.\n",
			rpc_priv->crp_pub.cr_opc, ul_in->ul_grp_id,
			ul_in->ul_rank, ul_req->cr_ep.ep_rank);
		crt_req_abort(ul_req);
		/*
		 * don't crt_rpc_complete rpc_priv here, because crt_req_abort
		 * above will lead to ul_req's completion callback --
		 * crt_req_uri_lookup_psr_cb() be called inside there will
		 * complete this rpc_priv.
		 */
		/* crt_rpc_complete(rpc_priv, -DER_PROTO); */
		break;
	case RPC_STATE_ADDR_LOOKUP:
		D_ERROR("rpc opc: %#x timedout due to ADDR_LOOKUP to group %s,"
			" rank %d, tgt_uri %s timedout.\n",
			rpc_priv->crp_pub.cr_opc, grp_priv->gp_pub.cg_grpid,
			tgt_ep->ep_rank, rpc_priv->crp_tgt_uri);
		crt_rpc_complete(rpc_priv, -DER_UNREACH);
		break;
	case RPC_STATE_FWD_UNREACH:
		D_ERROR("rpc opc: %#x to group %s, rank %d, tgt_uri %s "
			"can't reach the target.\n",
			rpc_priv->crp_pub.cr_opc, grp_priv->gp_pub.cg_grpid,
			tgt_ep->ep_rank, rpc_priv->crp_tgt_uri);
		crt_context_req_untrack(&rpc_priv->crp_pub);
		crt_rpc_complete(rpc_priv, -DER_UNREACH);
		RPC_DECREF(rpc_priv);
		break;
	default:
		if (rpc_priv->crp_on_wire) {
			/* At this point, RPC should always be completed by
			 * Mercury
			 */
			crt_req_abort(&rpc_priv->crp_pub);
		}
		break;
	}
}

static void
crt_context_timeout_check(struct crt_context *crt_ctx)
{
	struct crt_rpc_priv		*rpc_priv, *next;
	struct d_binheap_node		*bh_node;
	d_list_t			 timeout_list;
	uint64_t			 ts_now;

	D_ASSERT(crt_ctx != NULL);

	D_INIT_LIST_HEAD(&timeout_list);
	ts_now = d_timeus_secdiff(0);

	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	while (1) {
		bh_node = d_binheap_root(&crt_ctx->cc_bh_timeout);
		if (bh_node == NULL)
			break;
		rpc_priv = container_of(bh_node, struct crt_rpc_priv,
					crp_timeout_bp_node);
		if (rpc_priv->crp_timeout_ts > ts_now)
			break;

		/* +1 to prevent it from being released in timeout_untrack */
		RPC_ADDREF(rpc_priv);
		crt_req_timeout_untrack(&rpc_priv->crp_pub);

		d_list_add_tail(&rpc_priv->crp_tmp_link, &timeout_list);
		D_ERROR("ctx_id %d, rpc_priv %p (status: %#x) (opc %#x) "
			"timed out, tgt rank %d, tag %d.\n", crt_ctx->cc_idx,
			rpc_priv, rpc_priv->crp_state,
			rpc_priv->crp_pub.cr_opc,
			rpc_priv->crp_pub.cr_ep.ep_rank,
			rpc_priv->crp_pub.cr_ep.ep_tag);
	};
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

	/* handle the timeout RPCs */
	d_list_for_each_entry_safe(rpc_priv, next, &timeout_list,
				   crp_tmp_link) {
		/* check for and execute RPC timeout callbacks here */
		crt_exec_timeout_cb(rpc_priv);
		d_list_del_init(&rpc_priv->crp_tmp_link);
		crt_req_timeout_hdlr(rpc_priv);
		RPC_DECREF(rpc_priv);
	}
}

/*
 * Track the rpc request per context
 * return CRT_REQ_TRACK_IN_INFLIGHQ - tacked in crt_ep_inflight::epi_req_q
 *        CRT_REQ_TRACK_IN_WAITQ    - queued in crt_ep_inflight::epi_req_waitq
 *        negative value            - other error case such as -DER_NOMEM
 */
int
crt_context_req_track(crt_rpc_t *req)
{
	struct crt_rpc_priv	*rpc_priv;
	struct crt_context	*crt_ctx;
	struct crt_ep_inflight	*epi;
	d_list_t		*rlink;
	d_rank_t		 ep_rank;
	int			 rc = 0;

	D_ASSERT(req != NULL);
	crt_ctx = req->cr_ctx;
	D_ASSERT(crt_ctx != NULL);

	if (req->cr_opc == CRT_OPC_URI_LOOKUP) {
		D_DEBUG(DB_NET, "bypass tracking for URI_LOOKUP.\n");
		D_GOTO(out, rc = CRT_REQ_TRACK_IN_INFLIGHQ);
	}
	/* TODO use global rank */
	ep_rank = req->cr_ep.ep_rank;

	/* lookup the crt_ep_inflight (create one if not found) */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	rlink = d_hash_rec_find(&crt_ctx->cc_epi_table, (void *)&ep_rank,
				sizeof(ep_rank));
	if (rlink == NULL) {
		D_ALLOC_PTR(epi);
		if (epi == NULL) {
			D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
			D_GOTO(out, rc = -DER_NOMEM);
		}

		/* init the epi fields */
		D_INIT_LIST_HEAD(&epi->epi_link);
		epi->epi_ep.ep_rank = ep_rank;
		epi->epi_ctx = crt_ctx;
		D_INIT_LIST_HEAD(&epi->epi_req_q);
		epi->epi_req_num = 0;
		epi->epi_reply_num = 0;
		D_INIT_LIST_HEAD(&epi->epi_req_waitq);
		epi->epi_req_wait_num = 0;
		/* epi_ref init as 1 to avoid other thread delete it but here
		 * still need to access it, decref before exit this routine. */
		epi->epi_ref = 1;
		epi->epi_initialized = 1;
		rc = D_MUTEX_INIT(&epi->epi_mutex, NULL);
		if (rc != 0) {
			D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
			D_GOTO(out, rc);
		}

		rc = d_hash_rec_insert(&crt_ctx->cc_epi_table, &ep_rank,
				       sizeof(ep_rank), &epi->epi_link,
				       true /* exclusive */);
		if (rc != 0)
			D_ERROR("d_hash_rec_insert failed, rc: %d.\n", rc);
	} else {
		epi = epi_link2ptr(rlink);
		D_ASSERT(epi->epi_ctx == crt_ctx);
	}
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

	if (rc != 0)
		D_GOTO(out, rc);

	/* add the RPC req to crt_ep_inflight */
	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);
	D_MUTEX_LOCK(&epi->epi_mutex);
	D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);
	rpc_priv->crp_timeout_ts = crt_get_timeout(rpc_priv);
	rpc_priv->crp_epi = epi;
	RPC_ADDREF(rpc_priv);
	if (crt_gdata.cg_credit_ep_ctx != 0 &&
	    (epi->epi_req_num - epi->epi_reply_num) >=
	     crt_gdata.cg_credit_ep_ctx) {
		d_list_add_tail(&rpc_priv->crp_epi_link,
				&epi->epi_req_waitq);
		epi->epi_req_wait_num++;
		rpc_priv->crp_state = RPC_STATE_QUEUED;
		rc = CRT_REQ_TRACK_IN_WAITQ;
	} else {
		D_MUTEX_LOCK(&crt_ctx->cc_mutex);
		rc = crt_req_timeout_track(req);
		D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
		if (rc == 0) {
			d_list_add_tail(&rpc_priv->crp_epi_link,
					&epi->epi_req_q);
			epi->epi_req_num++;
			rc = CRT_REQ_TRACK_IN_INFLIGHQ;
		} else {
			D_ERROR("crt_req_timeout_track failed, rc: %d.\n", rc);
			/* roll back the addref above */
			RPC_DECREF(rpc_priv);
		}
	}

	D_MUTEX_UNLOCK(&epi->epi_mutex);

	/* reference taken by d_hash_rec_find or "epi->epi_ref = 1" above */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	d_hash_rec_decref(&crt_ctx->cc_epi_table, &epi->epi_link);
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

out:
	return rc;
}

void
crt_context_req_untrack(crt_rpc_t *req)
{
	struct crt_rpc_priv	*rpc_priv, *next;
	struct crt_ep_inflight	*epi;
	struct crt_context	*crt_ctx;
	int64_t			 credits, inflight;
	d_list_t		 submit_list;
	int			 rc;

	D_ASSERT(req != NULL);
	crt_ctx = req->cr_ctx;
	D_ASSERT(crt_ctx != NULL);
	rpc_priv = container_of(req, struct crt_rpc_priv, crp_pub);

	if (req->cr_opc == CRT_OPC_URI_LOOKUP) {
		D_DEBUG(DB_NET, "bypass untracking for URI_LOOKUP.\n");
		return;
	}

	D_ASSERT(rpc_priv->crp_state == RPC_STATE_INITED    ||
		 rpc_priv->crp_state == RPC_STATE_COMPLETED ||
		 rpc_priv->crp_state == RPC_STATE_TIMEOUT ||
		 rpc_priv->crp_state == RPC_STATE_ADDR_LOOKUP ||
		 rpc_priv->crp_state == RPC_STATE_URI_LOOKUP ||
		 rpc_priv->crp_state == RPC_STATE_CANCELED ||
		 rpc_priv->crp_state == RPC_STATE_FWD_UNREACH);
	epi = rpc_priv->crp_epi;
	D_ASSERT(epi != NULL);

	D_INIT_LIST_HEAD(&submit_list);

	D_MUTEX_LOCK(&epi->epi_mutex);
	/* remove from inflight queue */
	d_list_del_init(&rpc_priv->crp_epi_link);
	if (rpc_priv->crp_state == RPC_STATE_COMPLETED)
		epi->epi_reply_num++;
	else /* RPC_CANCELED or RPC_INITED or RPC_TIMEOUT */
		epi->epi_req_num--;
	D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);

	if (!crt_req_timedout(req)) {
		D_MUTEX_LOCK(&crt_ctx->cc_mutex);
		crt_req_timeout_untrack(req);
		D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
	}

	/* decref corresponding to addref in crt_context_req_track */
	RPC_DECREF(rpc_priv);

	/* done if flow control disabled */
	if (crt_gdata.cg_credit_ep_ctx == 0) {
		D_MUTEX_UNLOCK(&epi->epi_mutex);
		return;
	}

	/* process waitq */
	inflight = epi->epi_req_num - epi->epi_reply_num;
	D_ASSERT(inflight >= 0 && inflight <= crt_gdata.cg_credit_ep_ctx);
	credits = crt_gdata.cg_credit_ep_ctx - inflight;
	while (credits > 0 && !d_list_empty(&epi->epi_req_waitq)) {
		D_ASSERT(epi->epi_req_wait_num > 0);
		rpc_priv = d_list_entry(epi->epi_req_waitq.next,
					struct crt_rpc_priv, crp_epi_link);
		rpc_priv->crp_state = RPC_STATE_INITED;
		rpc_priv->crp_timeout_ts = crt_get_timeout(rpc_priv);

		D_MUTEX_LOCK(&crt_ctx->cc_mutex);
		rc = crt_req_timeout_track(&rpc_priv->crp_pub);
		D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);
		if (rc != 0)
			D_ERROR("crt_req_timeout_track failed, rc: %d.\n", rc);

		/* remove from waitq and add to in-flight queue */
		d_list_move_tail(&rpc_priv->crp_epi_link, &epi->epi_req_q);
		epi->epi_req_wait_num--;
		D_ASSERT(epi->epi_req_wait_num >= 0);
		epi->epi_req_num++;
		D_ASSERT(epi->epi_req_num >= epi->epi_reply_num);

		/* add to resend list */
		d_list_add_tail(&rpc_priv->crp_tmp_link, &submit_list);
		credits--;
	}
	D_MUTEX_UNLOCK(&epi->epi_mutex);

	/* re-submit the rpc req */
	d_list_for_each_entry_safe(rpc_priv, next, &submit_list,
				  crp_tmp_link) {
		d_list_del_init(&rpc_priv->crp_tmp_link);

		rc = crt_req_send_internal(rpc_priv);
		if (rc == 0)
			continue;

		RPC_ADDREF(rpc_priv);
		D_ERROR("crt_req_send_internal failed, rc: %d, opc: %#x.\n",
			rc, rpc_priv->crp_pub.cr_opc);
		rpc_priv->crp_state = RPC_STATE_INITED;
		crt_context_req_untrack(&rpc_priv->crp_pub);
		/* for error case here */
		crt_rpc_complete(rpc_priv, rc);
		RPC_DECREF(rpc_priv);
	}
}

crt_context_t
crt_context_lookup(int ctx_idx)
{
	struct crt_context	*ctx;
	bool			found = false;

	D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);
	d_list_for_each_entry(ctx, &crt_gdata.cg_ctx_list, cc_link) {
		if (ctx->cc_idx == ctx_idx) {
			found = true;
			break;
		}
	}
	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	return (found == true) ? ctx : NULL;
}

int
crt_context_idx(crt_context_t crt_ctx, int *ctx_idx)
{
	struct crt_context	*ctx;
	int			rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL || ctx_idx == NULL) {
		D_ERROR("invalid parameter, crt_ctx: %p, ctx_idx: %p.\n",
			crt_ctx, ctx_idx);
		D_GOTO(out, rc = -DER_INVAL);
	}

	ctx = crt_ctx;
	*ctx_idx = ctx->cc_idx;

out:
	return rc;
}

int
crt_context_num(int *ctx_num)
{
	if (ctx_num == NULL) {
		D_ERROR("invalid parameter of NULL ctx_num.\n");
		return -DER_INVAL;
	}

	*ctx_num = crt_gdata.cg_ctx_num;
	return 0;
}

bool
crt_context_empty(int locked)
{
	bool rc = false;

	if (locked == 0)
		D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);

	rc = d_list_empty(&crt_gdata.cg_ctx_list);

	if (locked == 0)
		D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	return rc;
}

static void
crt_exec_progress_cb(crt_context_t ctx)
{
	struct crt_prog_cb_priv	*crt_prog_cb_priv;
	d_list_t		*curr_node;
	d_list_t		*tmp_node;

	if (crt_plugin_gdata.cpg_inited == 0)
		return;

	if (ctx == NULL) {
		D_ERROR("Invalid parameter.\n");
		return;
	}
	D_RWLOCK_RDLOCK(&crt_plugin_gdata.cpg_prog_rwlock);
	d_list_for_each_safe(curr_node, tmp_node,
			     &crt_plugin_gdata.cpg_prog_cbs) {
		crt_prog_cb_priv =
			container_of(curr_node, struct crt_prog_cb_priv,
				     cpcp_link);
		D_RWLOCK_UNLOCK(&crt_plugin_gdata.cpg_prog_rwlock);
		crt_prog_cb_priv->cpcp_func(ctx, crt_prog_cb_priv->cpcp_args);
		D_RWLOCK_RDLOCK(&crt_plugin_gdata.cpg_prog_rwlock);
	}
	D_RWLOCK_UNLOCK(&crt_plugin_gdata.cpg_prog_rwlock);
}

int
crt_progress(crt_context_t crt_ctx, int64_t timeout,
	     crt_progress_cond_cb_t cond_cb, void *arg)
{
	struct crt_context	*ctx;
	int64_t			 hg_timeout;
	uint64_t		 now;
	uint64_t		 end = 0;
	int			 crt_ctx_idx;
	int			 rc = 0;

	/** validate input parameters */
	if (crt_ctx == CRT_CONTEXT_NULL) {
		D_ERROR("invalid parameter (NULL crt_ctx).\n");
		D_GOTO(out, rc = -DER_INVAL);
	}

	/**
	 * Invoke the callback once first, in case the condition is met before
	 * calling progress
	 */
	if (cond_cb) {
		/** execute callback */
		rc = cond_cb(arg);
		if (rc > 0)
			/** exit as per the callback request */
			D_GOTO(out, rc = 0);
		if (rc < 0)
			/**
			 * something wrong happened during the callback
			 * execution
			 */
			D_GOTO(out, rc);
	}

	rc = crt_context_idx(crt_ctx, &crt_ctx_idx);
	if (rc != 0) {
		D_ERROR("crt_context_idx() failed, rc: %d.\n", rc);
		D_GOTO(out, rc);
	}
	ctx = crt_ctx;
	if (timeout == 0 || cond_cb == NULL) { /** fast path */
		crt_context_timeout_check(ctx);
		/* check for and execute progress callbacks here */
		if (crt_ctx_idx == 0)
			crt_exec_progress_cb(crt_ctx);

		rc = crt_hg_progress(&ctx->cc_hg_ctx, timeout);
		if (rc && rc != -DER_TIMEDOUT) {
			D_ERROR("crt_hg_progress failed, rc: %d.\n", rc);
			D_GOTO(out, rc);
		}

		if (cond_cb) {
			int ret;

			/**
			 * Don't clobber rc which might be set to
			 * -DER_TIMEDOUT
			 */
			ret = cond_cb(arg);
			/** be careful with return code */
			if (ret > 0)
				D_GOTO(out, rc = 0);
			if (ret < 0)
				D_GOTO(out, rc = ret);
		}

		D_GOTO(out, rc);
	}

	/** Progress with callback and non-null timeout */
	D_ASSERT(timeout != 0);
	if (timeout < 0) {
		/**
		 * For infinite timeout, use a mercury timeout of 1 ms to avoid
		 * being blocked indefinitely if another thread has called
		 * crt_hg_progress() behind our back
		 */
		hg_timeout = 1000;
	} else { /** timeout > 0 */
		now = d_timeus_secdiff(0);
		end = now + timeout;
		/** similarly, probe more frequently if timeout is large */
		if (timeout > 1000 * 1000)
			hg_timeout = 1000 * 1000;
		else
			hg_timeout = timeout;
	}

	while (true) {
		crt_context_timeout_check(ctx);
		/* check for and execute progress callbacks here */
		if (crt_ctx_idx == 0)
			crt_exec_progress_cb(ctx);

		rc = crt_hg_progress(&ctx->cc_hg_ctx, hg_timeout);
		if (rc && rc != -DER_TIMEDOUT) {
			D_ERROR("crt_hg_progress failed with %d\n", rc);
			D_GOTO(out, rc = 0);
		}

		/** execute callback */
		rc = cond_cb(arg);
		if (rc > 0)
			D_GOTO(out, rc = 0);
		if (rc < 0)
			D_GOTO(out, rc);

		/** check for timeout, if not infinite */
		if (timeout > 0) {
			now = d_timeus_secdiff(0);
			if (now >= end) {
				rc = -DER_TIMEDOUT;
				break;
			}
			if (end - now > 1000 * 1000)
				hg_timeout = 1000 * 1000;
			else
				hg_timeout = end - now;
		}
	}
out:
	return rc;
}

/**
 * to use this function, the user has to:
 * 1) define a callback function user_cb
 * 2) call crt_register_progress_cb(user_cb);
 */
int
crt_register_progress_cb(crt_progress_cb cb, void *arg)
{
	/* save the function pointer and arg to a global list */
	struct crt_prog_cb_priv		*crt_prog_cb_priv = NULL;
	int				 rc = 0;

	D_ALLOC_PTR(crt_prog_cb_priv);
	if (crt_prog_cb_priv == NULL)
		D_GOTO(out, rc = -DER_NOMEM);
	crt_prog_cb_priv->cpcp_func = cb;
	crt_prog_cb_priv->cpcp_args = arg;
	D_RWLOCK_WRLOCK(&crt_plugin_gdata.cpg_prog_rwlock);
	d_list_add_tail(&crt_prog_cb_priv->cpcp_link,
			&crt_plugin_gdata.cpg_prog_cbs);
	D_RWLOCK_UNLOCK(&crt_plugin_gdata.cpg_prog_rwlock);

out:
	return rc;
}

/**
 * to use this function, the user has to:
 * 1) define a callback function user_cb
 * 2) call crt_register_timeout_cb_core(user_cb);
 */
int
crt_register_timeout_cb(crt_timeout_cb cb, void *arg)
{
	/* TODO: save the function pointer somewhere for retreival later on */
	struct crt_timeout_cb_priv	*timeout_cb_priv = NULL;
	int				 rc = 0;

	D_ALLOC_PTR(timeout_cb_priv);
	if (timeout_cb_priv == NULL)
		D_GOTO(out, rc = -DER_NOMEM);
	timeout_cb_priv->ctcp_func = cb;
	timeout_cb_priv->ctcp_args = arg;
	D_RWLOCK_WRLOCK(&crt_plugin_gdata.cpg_timeout_rwlock);
	d_list_add_tail(&timeout_cb_priv->ctcp_link,
			&crt_plugin_gdata.cpg_timeout_cbs);
	D_RWLOCK_UNLOCK(&crt_plugin_gdata.cpg_timeout_rwlock);

out:
	return rc;
}

int
crt_context_set_timeout(crt_context_t crt_ctx, uint32_t timeout_sec)
{
	struct crt_context	*ctx;
	int			rc = 0;

	if (crt_ctx == CRT_CONTEXT_NULL) {
		D_ERROR("NULL context passed\n");
		D_GOTO(exit, rc = -DER_INVAL);
	}

	if (timeout_sec == 0) {
		D_ERROR("Invalid value 0 for timeout specified\n");
		D_GOTO(exit, rc = -DER_INVAL);
	}

	ctx = crt_ctx;
	ctx->cc_timeout_sec = timeout_sec;

exit:
	return rc;
}

/* Execute handling for unreachable rpcs */
void
crt_req_force_timeout(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*crt_ctx;

	D_DEBUG(DB_TRACE, "Handling unreachable rpc, rpc_priv=%p\n",
			rpc_priv);

	if (rpc_priv == NULL) {
		D_ERROR("Invalid argument, rpc_priv == NULL\n");
		return;
	}

	if (rpc_priv->crp_pub.cr_opc == CRT_OPC_URI_LOOKUP) {
		D_DEBUG(DB_TRACE, "Skipping for opcode: %#x",
			CRT_OPC_URI_LOOKUP);
		return;
	}

	/* Handle unreachable rpcs similarly to timed out rpcs */
	RPC_ADDREF(rpc_priv);
	crt_ctx = rpc_priv->crp_pub.cr_ctx;

	/**
	 *  set the RPC's expiration time stamp to the past, move it to the top
	 *  of the heap.
	 */
	D_MUTEX_LOCK(&crt_ctx->cc_mutex);
	crt_req_timeout_untrack(&rpc_priv->crp_pub);
	rpc_priv->crp_timeout_ts = 0;
	crt_req_timeout_track(&rpc_priv->crp_pub);
	D_MUTEX_UNLOCK(&crt_ctx->cc_mutex);

	crt_exec_timeout_cb(rpc_priv);

	RPC_DECREF(rpc_priv);
}
