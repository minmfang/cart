/* Copyright (C) 2018 Intel Corporation
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
#include <semaphore.h>

#include <gurt/common.h>
#include <cart/api.h>

#include "test_proto_common.h"


static void
rpc_cb_common(const struct crt_cb_info *cb_info)
{
	crt_rpc_t			*rpc_req;
	struct ping_in_t		*rpc_req_input;
	struct ping_out_t		*rpc_req_output;

	rpc_req = cb_info->cci_rpc;
	D_ASSERT(rpc_req != NULL);

	switch (rpc_req->cr_opc) {
	case CRT_PROTO_OPC(OPC_MY_PROTO, 1, 1):
		rpc_req_input = crt_req_get(rpc_req);
		rpc_req_output = crt_reply_get(rpc_req);
		D_DEBUG(DB_TRACE, "bounced back magic number %u\n",
			rpc_req_output->po_magic);
		D_ASSERT(rpc_req_output->po_magic
			 == rpc_req_input->pi_magic + 1);
		sem_post(&test.tg_token_to_proceed);
		break;
	case OPC_SHUTDOWN:
		test.tg_shutdown = 1;
		sem_post(&test.tg_token_to_proceed);
		break;
	default:
		break;
	}
}

static void
test_init()
{
	uint32_t	flag;
	int		rc;

	fprintf(stderr, "local group: %s remote group: %s\n",
		test.tg_local_group_name, test.tg_remote_group_name);

	rc = sem_init(&test.tg_token_to_proceed, 0, 0);
	D_ASSERTF(rc == 0, "sem_init() failed.\n");

	flag = test.tg_is_service ? CRT_FLAG_BIT_SERVER : 0;
	rc = crt_init(test.tg_local_group_name, flag);
	D_ASSERTF(rc == 0, "crt_init() failed, rc: %d\n", rc);

	rc = crt_group_rank(NULL, &test.tg_my_rank);
	D_ASSERTF(rc == 0, "crt_group_rank() failed. rc: %d\n", rc);

	rc = crt_proto_register(&my_proto_fmt_0);
	D_ASSERT(rc == 0);
	rc = crt_proto_register(&my_proto_fmt_1);
	D_ASSERT(rc == 0);
	rc = crt_context_create(&test.tg_crt_ctx);
	D_ASSERTF(rc == 0, "crt_context_create() failed. rc: %d\n", rc);

	rc = pthread_create(&test.tg_tid, NULL, progress_thread,
			    &test.tg_thread_id);
	D_ASSERTF(rc == 0, "pthread_create() failed. rc: %d\n", rc);
}

static void
attach_to_server()
{
	int	num_retries = 120;
	int	retry;
	int	rc;

	/* try until success to avoid intermittent failures under valgrind. */
	D_DEBUG(DB_TRACE, "about attach to server\n");
	for (retry = 0; retry < num_retries; retry++) {
		sleep(1);
		rc = crt_group_attach(test.tg_remote_group_name,
				      &test.tg_remote_group);
		if (rc == DER_SUCCESS)
			break;
	}
	D_ASSERTF(rc == 0, "crt_group_attach failed, rc: %d\n", rc);
	D_ASSERTF(test.tg_remote_group != NULL, "NULL attached srv_grp\n");
}

static void
query_cb(struct crt_proto_query_cb_info *cb_info)
{
	uint32_t	*high_ver;

	high_ver = cb_info->pq_arg;

	D_ASSERT(cb_info->pq_arg != NULL);
	D_ASSERT(cb_info->pq_rc == DER_SUCCESS);

	*high_ver = cb_info->pq_ver;
}

static void
test_run()
{
	crt_rpc_t		*rpc_req = NULL;
	struct ping_in_t	*rpc_req_input;
	crt_endpoint_t		 tgt_ep = {0};
	crt_opcode_t		 my_opc;
	uint32_t		 my_ver_array[] = {0, 2, 5, 1, 4, 3, 7};
	uint32_t		 high_ver = 0xFFFFFFFF;
	int			 rc;


	D_DEBUG(DB_TRACE, "test_run\n");

	if (test.tg_should_attach)
		attach_to_server();

	crt_group_size(test.tg_remote_group, &test.tg_remote_group_size);
	D_DEBUG(DB_TRACE, "size of %s is %d\n", test.tg_remote_group_name,
		test.tg_remote_group_size);

	tgt_ep.ep_grp = test.tg_remote_group;
	tgt_ep.ep_rank = 0;

	rc = crt_proto_query(&tgt_ep, OPC_MY_PROTO, my_ver_array, 7, query_cb,
			     &high_ver);
	D_ASSERT(rc == 0);

	while (high_ver == 0xFFFFFFFF)
		sched_yield();

	D_DEBUG(DB_TRACE, "high_ver %u.\n", high_ver);
	D_ASSERT(high_ver == 1);

	/* get the opcode of the second RPC in version 1 of OPC_MY_PROTO */
	my_opc = CRT_PROTO_OPC(OPC_MY_PROTO, 1, 1);
	rc = crt_req_create(test.tg_crt_ctx, &tgt_ep, my_opc, &rpc_req);

	D_ASSERTF(rc == 0 && rpc_req != NULL, "crt_req_create() failed,"
		  " rc: %d rpc_req: %p\n", rc, rpc_req);

	rpc_req_input = crt_req_get(rpc_req);
	D_ASSERTF(rpc_req_input != NULL, "crt_req_get() failed."
		  " rpc_req_input: %p\n", rpc_req_input);

	rpc_req_input->pi_magic = 2345;

	rc = crt_req_send(rpc_req, rpc_cb_common, NULL);
	D_ASSERTF(rc == 0, "crt_req_send() failed. rc: %d\n", rc);

	test_sem_timedwait(&test.tg_token_to_proceed, 61, __LINE__);
}

/************************************************/
static void
shutdown_server()
{
	crt_endpoint_t		 tgt_ep = {0};
	crt_rpc_t		*rpc_req = NULL;
	int			 ii;
	int			 rc;

	for (ii = 0; ii < test.tg_remote_group_size; ii++) {
		tgt_ep.ep_grp = test.tg_remote_group;
		tgt_ep.ep_rank = ii;
		rc = crt_req_create(test.tg_crt_ctx, &tgt_ep,
				OPC_SHUTDOWN, &rpc_req);
		D_ASSERTF(rc == 0 && rpc_req != NULL,
			  "crt_req_create() failed. "
			  "rc: %d, rpc_req: %p\n", rc, rpc_req);
		rc = crt_req_send(rpc_req, rpc_cb_common, NULL);
		D_ASSERTF(rc == 0, "crt_req_send() failed. rc: %d\n", rc);

		test_sem_timedwait(&test.tg_token_to_proceed, 61, __LINE__);
	}
}

static void
test_fini()
{
	int	rc;

	if (test.tg_should_attach && test.tg_my_rank == 0)
		shutdown_server();

	if (test.tg_should_attach) {
		rc = crt_group_detach(test.tg_remote_group);
		D_ASSERTF(rc == 0, "crt_group_detach failed, rc: %d\n", rc);
	}

	test.tg_shutdown = 1;

	rc = pthread_join(test.tg_tid, NULL);
	D_ASSERTF(rc == 0, "pthread_join failed. rc: %d\n", rc);
	D_DEBUG(DB_TRACE, "joined progress thread.\n");


	rc = crt_context_destroy(test.tg_crt_ctx, 1);
	D_ASSERTF(rc == 0, "crt_context_destroy() failed. rc: %d\n", rc);

	rc = sem_destroy(&test.tg_token_to_proceed);
	D_ASSERTF(rc == 0, "sem_destroy() failed.\n");

	rc = crt_finalize();
	D_ASSERTF(rc == 0, "crt_finalize() failed. rc: %d\n", rc);
	D_DEBUG(DB_TRACE, "exiting.\n");
}

int
main(int argc, char **argv)
{
	int	rc;

	rc = test_parse_args(argc, argv);
	if (rc != 0) {
		fprintf(stderr, "test_parse_args() failed, rc: %d.\n", rc);
		return rc;
	}
	test_init();
	test_run();
	test_fini();

	return rc;
}
