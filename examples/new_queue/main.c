/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   Copyright(c) 2015 George Washington University
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This sample application is a simple multi-process application which
 * demostrates sharing of queues and memory pools between processes, and
 * using those queues/pools for communication between the processes.
 *
 * Application is designed to run with two processes, a primary and a
 * secondary, and each accepts commands on the commandline, the most
 * important of which is "send", which just sends a string to the other
 * process.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <termios.h>
#include <sys/queue.h>

#include <rte_common.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_launch.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_ring.h>
#include <rte_log.h>
#include <rte_mempool.h>
#include <rte_malloc.h>
#include <cmdline_rdline.h>
#include <cmdline_parse.h>
#include <cmdline_socket.h>
#include <cmdline.h>
#include "mp_commands.h"

#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

static const char *_MSG_POOL = "MSG_POOL";
static const char *_SEC_2_PRI = "SEC_2_PRI";
static const char *_PRI_2_SEC = "PRI_2_SEC";
static const char *_NEW_RING = "NEW_TEST_RING";
static const char *_NEW_MSG_POOL = "NEW_MSG_POOL";
const unsigned string_size = 64;

struct rte_ring *send_ring, *recv_ring, *new_ring;
struct rte_mempool *message_pool, *new_message_pool;
volatile int quit = 0;

struct dummy {
        int num1;
        int num2;
};

/**
 * This function gets some space from the mempool, and then it will
 * assign values to the two ints inside the dummy struct. Next, it
 * enqueues the struct onto the ring buffer.
 *
 * @param
 *     void
 * @return
 *     0 on success, anything else otherwise.
 */
static int
fill_ring(void) {
        struct dummy *dum;

        if (rte_mempool_get(new_message_pool, (void *)&dum) < 0) {
                rte_panic("Failed to get new message buffer\n");
        }

        if (dum == NULL) {
                rte_panic("Failed to rte_malloc struct\n");
                exit(-1);
        }

        dum->num1 = 45;
        dum->num2 = 32;

        if (rte_ring_enqueue(new_ring, dum) < 0) {
                printf("Problem enqueuing into new ring\n");
                rte_mempool_put(new_message_pool, dum);
        }

        return 0;
}

/**
 * This struct dequeues the ring buffer and get the struct that fill_ring(void)
 * enqueued.  It will then print out the two values for confirmation.
 *
 * @param
 *     void
 * @return
 *     0 if success, anything else otherwise
 */
static int
get_from_ring(void) {
        void *val;

        if (rte_ring_dequeue(new_ring, &val) < 0) {
                printf("Problem dequeuing from new ring -- %d\n", x);
                usleep(5);
        }

        printf("core %u: message from new queue ---- %d\t%d\n", rte_lcore_id(), (int)(((struct dummy *) val)->num1), (int)(((struct dummy *) val)->num2));

        rte_mempool_put(new_message_pool, val);

        return 0;
}

static int
lcore_recv(__attribute__((unused)) void *arg)
{
	unsigned lcore_id = rte_lcore_id();

	printf("Starting core %u\n", lcore_id);
	while (!quit){
		void *msg;
		if (rte_ring_dequeue(recv_ring, &msg) < 0){
			usleep(5);
			continue;
		}
		printf("core %u: Received '%s'\n", lcore_id, (char *)msg);
		rte_mempool_put(message_pool, msg);
	}

	return 0;
}

int
main(int argc, char **argv)
{
	const unsigned flags = 0;
	const unsigned ring_size = 64;
	const unsigned pool_size = 1024;
	const unsigned pool_cache = 32;
	const unsigned priv_data_sz = 0;

	int ret;
	unsigned lcore_id;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init EAL\n");

	if (rte_eal_process_type() == RTE_PROC_PRIMARY){
		send_ring = rte_ring_create(_PRI_2_SEC, ring_size, rte_socket_id(), flags);
		recv_ring = rte_ring_create(_SEC_2_PRI, ring_size, rte_socket_id(), flags);
		message_pool = rte_mempool_create(_MSG_POOL, pool_size,
				string_size, pool_cache, priv_data_sz,
				NULL, NULL, NULL, NULL,
				rte_socket_id(), flags);

                // Making a new ring buffer in hugepages to store strings for new queue example
                // Set it to have the name = NEW_TEST_RING, size = 64, socket of current proc,
                // and use SP/SC flags
                new_ring = rte_ring_create(_NEW_RING, ring_size, rte_socket_id(), flags);
                // mempool to hold messages for new ring
                new_message_pool = rte_mempool_create(_NEW_MSG_POOL, pool_size,
                                        sizeof(struct dummy), pool_cache, priv_data_sz,
                                        NULL, NULL, NULL, NULL,
                                        rte_socket_id(), flags);

                fill_ring();
	} else {
		recv_ring = rte_ring_lookup(_PRI_2_SEC);
		send_ring = rte_ring_lookup(_SEC_2_PRI);
		message_pool = rte_mempool_lookup(_MSG_POOL);

                // Lookup ring with name NEW_TEST_RING created by primary process and put its
                // hugepage pointer in variable new_rin
                new_ring = rte_ring_lookup(_NEW_RING);
                new_message_pool = rte_mempool_lookup(_NEW_MSG_POOL);

                get_from_ring();
	}

	if (send_ring == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting sending ring\n");
	if (recv_ring == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting receiving ring\n");
	if (message_pool == NULL)
		rte_exit(EXIT_FAILURE, "Problem getting message pool\n");
        if (new_ring == NULL)
                rte_exit(EXIT_FAILURE, "Problem getting new_ring\n");
        if (new_message_pool == NULL)
                rte_exit(EXIT_FAILURE, "Problem getting new message pool\n");

	RTE_LOG(INFO, APP, "Finished Process Init.\n");

	/* call lcore_recv() on every slave lcore */
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		rte_eal_remote_launch(lcore_recv, NULL, lcore_id);
	}

	/* call cmd prompt on master lcore */
	struct cmdline *cl = cmdline_stdin_new(simple_mp_ctx, "\nsimple_mp > ");
	if (cl == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create cmdline instance\n");
	cmdline_interact(cl);
	cmdline_stdin_exit(cl);

	rte_eal_mp_wait_lcore();
	return 0;
}