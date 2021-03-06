/* Copyright (C) 2017-2018 Intel Corporation
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
 * This tests a threaded server handling RPCs on a single context
 */
#include <stdio.h>
#include "threaded_rpc.h"

static int		done;
static crt_context_t	crt_ctx;
static int		msg_counts[MSG_COUNT];
static pthread_mutex_t	lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	cond = PTHREAD_COND_INITIALIZER;

#define NUM_THREADS 16
#define STOP 1

static int check_status(void *arg)
{
	int	*status = (int *)arg;

	return (*status == STOP);
}

static void *progress(void *arg)
{
	int	*status = (int *)arg;
	int	 rc;

	__sync_fetch_and_add(status, -1);

	do {
		rc = crt_progress(crt_ctx, 1000*1000, check_status, status);
		if (rc == -DER_TIMEDOUT)
			sched_yield();
		else if (rc != 0)
			printf("crt_progress failed rc: %d", rc);
	} while (*status != STOP);

	return NULL;
}
static void signal_done(void)
{
	D_MUTEX_LOCK(&lock);
	done = 1;
	pthread_cond_signal(&cond);
	D_MUTEX_UNLOCK(&lock);
}

static void rpc_handler(crt_rpc_t *rpc)
{
	struct rpc_in	*in;
	struct rpc_out	*output;
	int		 rc;
	int		 i;

	in = crt_req_get(rpc);
	output = crt_reply_get(rpc);

	for (i = 0; i < MSG_COUNT; i++) {
		if (in->msg == msg_values[i] && in->payload == MSG_IN_VALUE) {
			__sync_fetch_and_add(&msg_counts[i], 1);
			output->msg = MSG_OUT_VALUE;
			output->value = in->msg;
			break;
		}
	}

	rc = crt_reply_send(rpc);
	if (rc != 0)
		printf("Failed to send reply, rc = %d\n", rc);

	if (i == MSG_STOP) {
		printf("Received stop rpc\n");
		signal_done();
	} else if (i == MSG_COUNT) {
		printf("Bad rpc message received %#x %#x\n", in->msg,
		       in->payload);
		signal_done();
	}
}

int main(int argc, char **argv)
{
	pthread_t		thread[NUM_THREADS];
	struct crt_req_format	fmt = INIT_FMT();
	int			status = 0;
	int			rc;
	int			i;

	rc = crt_init("manyserver", CRT_FLAG_BIT_SERVER);
	if (rc != 0) {
		printf("Could not start server, rc = %d", rc);
		return -1;
	}

	crt_rpc_srv_register(RPC_ID, 0, &fmt, rpc_handler);

	crt_context_create(&crt_ctx);
	for (rc = 0; rc < NUM_THREADS; rc++)
		pthread_create(&thread[rc], NULL, progress, &status);

	printf("Waiting for threads to start\n");
	while (status != -NUM_THREADS)
		sched_yield();

	printf("Waiting for stop rpc\n");
	D_MUTEX_LOCK(&lock);
	while (done == 0)
		rc = pthread_cond_wait(&cond, &lock);

	D_MUTEX_UNLOCK(&lock);
	printf("Stop rpc exited with rc = %d\n", rc);

	status = STOP;
	printf("Waiting for threads to stop\n");

	for (rc = 0; rc < NUM_THREADS; rc++)
		pthread_join(thread[rc], NULL);

	drain_queue(crt_ctx);

	printf("Server message counts:\n");
	for (i = 0; i < MSG_COUNT; i++)
		printf("\tSERVER\t%-10s:\t%10d\n", msg_strings[i],
		       msg_counts[i]);

	crt_context_destroy(crt_ctx, false);
	crt_finalize();

	return 0;
}
