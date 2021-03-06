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
 * This file is part of CaRT. It is the header file of bridging to mercury.
 */
#ifndef __CRT_MERCURY_H__
#define __CRT_MERCURY_H__

#include <gurt/list.h>

#include <mercury.h>
#include <mercury_types.h>
#include <mercury_macros.h>
#include <mercury_proc.h>
#include <mercury_proc_string.h>
#include <mercury_log.h>
#include <na.h>
#include <na_error.h>

/** the shared HG RPC ID used for all CRT opc */
#define CRT_HG_RPCID		(0xDA036868)
#define CRT_HG_ONEWAY_RPCID	(0xDA036869)

/** MAX number of HG handles in pool */
#define CRT_HG_POOL_MAX_NUM	(512)
/** number of prepost HG handles when enable pool */
#define CRT_HG_POOL_PREPOST_NUM	(16)

struct crt_rpc_priv;
struct crt_common_hdr;
struct crt_corpc_hdr;

/** type of NA plugin */
enum crt_na_type {
	CRT_NA_SM		= 0,
	CRT_NA_OFI_SOCKETS	= 1,
	CRT_NA_OFI_VERBS	= 2,
	CRT_NA_OFI_GNI		= 3,
	CRT_NA_OFI_PSM2		= 4
};

static inline bool
crt_na_type_is_ofi(int na_type)
{
	return (na_type >= CRT_NA_OFI_SOCKETS) &&
	       (na_type <= CRT_NA_OFI_PSM2);
}

struct crt_na_dict {
	char	*nad_str;
	int	nad_type;
	/* a flag of explicitly bind with IP:port to create NA class */
	bool	nad_port_bind;
};

extern struct crt_na_dict crt_na_dict[];

struct crt_hg_hdl {
	/* link to crt_hg_pool::chp_hg_list */
	d_list_t		chh_link;
	/* HG handle */
	hg_handle_t		chh_hdl;
};

struct crt_hg_pool {
	pthread_spinlock_t	chp_lock;
	/* number of HG handles in pool */
	int32_t			chp_num;
	/* maximum number of HG handles in pool */
	int32_t			chp_max_num;
	/* HG handle list */
	d_list_t		chp_list;
	bool			chp_enabled;
};

/** HG context */
struct crt_hg_context {
	bool			 chc_shared_na; /* flag for shared na_class */
	na_class_t		*chc_nacla; /* NA class */
	hg_class_t		*chc_hgcla; /* HG class */
	hg_context_t		*chc_hgctx; /* HG context */
	hg_class_t		*chc_bulkcla; /* bulk class */
	hg_context_t		*chc_bulkctx; /* bulk context */
	struct crt_hg_pool	 chc_hg_pool; /* HG handle pool */
};

/** HG level global data */
struct crt_hg_gdata {
	na_class_t		*chg_nacla; /* NA class */
	hg_class_t		*chg_hgcla; /* HG class */
};

/* addr lookup completion callback */
typedef int (*crt_hg_addr_lookup_cb_t)(hg_addr_t addr, void *arg);

struct crt_hg_addr_lookup_cb_args {
	crt_hg_addr_lookup_cb_t	 al_cb;
	void			*al_arg;
};

/* crt_hg.c */
int crt_hg_init(crt_phy_addr_t *addr, bool server);
int crt_hg_fini(void);
int crt_hg_ctx_init(struct crt_hg_context *hg_ctx, int idx);
int crt_hg_ctx_fini(struct crt_hg_context *hg_ctx);
int crt_hg_req_create(struct crt_hg_context *hg_ctx,
		      struct crt_rpc_priv *rpc_priv);
int crt_hg_req_destroy(struct crt_rpc_priv *rpc_priv);
int crt_hg_req_send(struct crt_rpc_priv *rpc_priv);
int crt_hg_reply_send(struct crt_rpc_priv *rpc_priv);
void crt_hg_reply_error_send(struct crt_rpc_priv *rpc_priv, int error_code);
int crt_hg_req_cancel(struct crt_rpc_priv *rpc_priv);
int crt_hg_progress(struct crt_hg_context *hg_ctx, int64_t timeout);
int crt_hg_addr_lookup(struct crt_hg_context *hg_ctx, const char *name,
		       crt_hg_addr_lookup_cb_t complete_cb, void *arg);
int crt_hg_addr_free(struct crt_hg_context *hg_ctx, hg_addr_t addr);
int crt_hg_get_addr(hg_class_t *hg_class, char *addr_str, size_t *str_size);

int crt_rpc_handler_common(hg_handle_t hg_hdl);

/* crt_hg_proc.c */
int crt_proc_common_hdr(crt_proc_t proc, struct crt_common_hdr *hdr);
int crt_proc_corpc_hdr(crt_proc_t proc, struct crt_corpc_hdr *hdr);
int crt_hg_unpack_header(hg_handle_t hg_hdl, struct crt_rpc_priv *rpc_priv,
			 crt_proc_t *proc);
void crt_hg_header_copy(struct crt_rpc_priv *in, struct crt_rpc_priv *out);
void crt_hg_unpack_cleanup(crt_proc_t proc);
int crt_proc_internal(struct crf_field *drf, crt_proc_t proc, void *data);
int crt_proc_input(struct crt_rpc_priv *rpc_priv, crt_proc_t proc);
int crt_proc_output(struct crt_rpc_priv *rpc_priv, crt_proc_t proc);
int crt_hg_unpack_body(struct crt_rpc_priv *rpc_priv, crt_proc_t proc);
int crt_proc_in_common(crt_proc_t proc, crt_rpc_input_t *data);
int crt_proc_out_common(crt_proc_t proc, crt_rpc_output_t *data);

/* some simple helper functions */
typedef hg_rpc_cb_t crt_hg_rpc_cb_t;
static inline int
crt_hg_reg(hg_class_t *hg_class, hg_id_t rpcid, crt_proc_cb_t in_proc_cb,
	   crt_proc_cb_t out_proc_cb, crt_hg_rpc_cb_t rpc_cb)
{
	hg_return_t hg_ret;
	int         rc = 0;

	D_ASSERT(hg_class != NULL);

	hg_ret = HG_Register(hg_class, rpcid, (hg_proc_cb_t)in_proc_cb,
			     (hg_proc_cb_t)out_proc_cb, rpc_cb);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Register(rpcid: %#lx) failed, hg_ret: %d.\n",
			rpcid, hg_ret);
		rc = -DER_HG;
	}
	return rc;
}

static inline int
crt_hg_bulk_free(crt_bulk_t bulk_hdl)
{
	hg_return_t	hg_ret;
	int		rc = 0;

	hg_ret = HG_Bulk_free(bulk_hdl);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Bulk_free failed, hg_ret: %d.\n", hg_ret);
		rc = -DER_HG;
	}

	return rc;
}

static inline int
crt_hg_bulk_get_len(crt_bulk_t bulk_hdl, size_t *bulk_len)
{
	hg_size_t	hg_size;

	if (bulk_len == NULL) {
		D_ERROR("bulk_len is NULL\n");
		return -DER_INVAL;
	}

	if (bulk_hdl == CRT_BULK_NULL) {
		D_ERROR("bulk_hdl is NULL\n");
		return -DER_INVAL;
	}

	hg_size = HG_Bulk_get_size(bulk_hdl);
	*bulk_len = hg_size;

	return 0;
}

static inline int
crt_hg_bulk_get_sgnum(crt_bulk_t bulk_hdl, unsigned int *bulk_sgnum)
{
	hg_uint32_t	hg_sgnum;

	D_ASSERT(bulk_sgnum != NULL);
	hg_sgnum = HG_Bulk_get_segment_count(bulk_hdl);
	*bulk_sgnum = hg_sgnum;

	return 0;
}

int crt_hg_bulk_create(struct crt_hg_context *hg_ctx, d_sg_list_t *sgl,
		       crt_bulk_perm_t bulk_perm, crt_bulk_t *bulk_hdl);
int crt_hg_bulk_access(crt_bulk_t bulk_hdl, d_sg_list_t *sgl);
int crt_hg_bulk_transfer(struct crt_bulk_desc *bulk_desc,
			 crt_bulk_cb_t complete_cb,
			 void *arg, crt_bulk_opid_t *opid);
static inline int
crt_hg_bulk_cancel(crt_bulk_opid_t opid)
{
	return HG_Bulk_cancel(opid);
}

#endif /* __CRT_MERCURY_H__ */
