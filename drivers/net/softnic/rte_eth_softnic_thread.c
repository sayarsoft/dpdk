/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2018 Intel Corporation
 */

#include <stdlib.h>

#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ring.h>

#include "rte_eth_softnic_internals.h"

/**
 * Master thread: data plane thread init
 */
void
softnic_thread_free(struct pmd_internals *softnic)
{
	uint32_t i;

	RTE_LCORE_FOREACH_SLAVE(i) {
		struct softnic_thread *t = &softnic->thread[i];

		/* MSGQs */
		if (t->msgq_req)
			rte_ring_free(t->msgq_req);

		if (t->msgq_rsp)
			rte_ring_free(t->msgq_rsp);
	}
}

int
softnic_thread_init(struct pmd_internals *softnic)
{
	uint32_t i;

	RTE_LCORE_FOREACH_SLAVE(i) {
		char ring_name[NAME_MAX];
		struct rte_ring *msgq_req, *msgq_rsp;
		struct softnic_thread *t = &softnic->thread[i];
		struct softnic_thread_data *t_data = &softnic->thread_data[i];
		uint32_t cpu_id = rte_lcore_to_socket_id(i);

		/* MSGQs */
		snprintf(ring_name, sizeof(ring_name), "%s-TH%u-REQ",
			softnic->params.name,
			i);

		msgq_req = rte_ring_create(ring_name,
			THREAD_MSGQ_SIZE,
			cpu_id,
			RING_F_SP_ENQ | RING_F_SC_DEQ);

		if (msgq_req == NULL) {
			softnic_thread_free(softnic);
			return -1;
		}

		snprintf(ring_name, sizeof(ring_name), "%s-TH%u-RSP",
			softnic->params.name,
			i);

		msgq_rsp = rte_ring_create(ring_name,
			THREAD_MSGQ_SIZE,
			cpu_id,
			RING_F_SP_ENQ | RING_F_SC_DEQ);

		if (msgq_rsp == NULL) {
			softnic_thread_free(softnic);
			return -1;
		}

		/* Master thread records */
		t->msgq_req = msgq_req;
		t->msgq_rsp = msgq_rsp;
		t->enabled = 1;

		/* Data plane thread records */
		t_data->n_pipelines = 0;
		t_data->msgq_req = msgq_req;
		t_data->msgq_rsp = msgq_rsp;
		t_data->timer_period =
			(rte_get_tsc_hz() * THREAD_TIMER_PERIOD_MS) / 1000;
		t_data->time_next = rte_get_tsc_cycles() + t_data->timer_period;
		t_data->time_next_min = t_data->time_next;
	}

	return 0;
}

static inline int
thread_is_running(uint32_t thread_id)
{
	enum rte_lcore_state_t thread_state;

	thread_state = rte_eal_get_lcore_state(thread_id);
	return (thread_state == RUNNING)? 1 : 0;
}

/**
 * Pipeline is running when:
 *    (A) Pipeline is mapped to a data plane thread AND
 *    (B) Its data plane thread is in RUNNING state.
 */
static inline int
pipeline_is_running(struct pipeline *p)
{
	if (p->enabled == 0)
		return 0;

	return thread_is_running(p->thread_id);
}

/**
 * Master thread & data plane threads: message passing
 */
enum thread_req_type {
	THREAD_REQ_MAX
};

struct thread_msg_req {
	enum thread_req_type type;
};

struct thread_msg_rsp {
	int status;
};

/**
 * Data plane threads: message handling
 */
static inline struct thread_msg_req *
thread_msg_recv(struct rte_ring *msgq_req)
{
	struct thread_msg_req *req;

	int status = rte_ring_sc_dequeue(msgq_req, (void **)&req);

	if (status != 0)
		return NULL;

	return req;
}

static inline void
thread_msg_send(struct rte_ring *msgq_rsp,
	struct thread_msg_rsp *rsp)
{
	int status;

	do {
		status = rte_ring_sp_enqueue(msgq_rsp, rsp);
	} while (status == -ENOBUFS);
}

static void
thread_msg_handle(struct softnic_thread_data *t)
{
	for ( ; ; ) {
		struct thread_msg_req *req;
		struct thread_msg_rsp *rsp;

		req = thread_msg_recv(t->msgq_req);
		if (req == NULL)
			break;

		switch (req->type) {
		default:
			rsp = (struct thread_msg_rsp *)req;
			rsp->status = -1;
		}

		thread_msg_send(t->msgq_rsp, rsp);
	}
}

/**
 * Master thread & data plane threads: message passing
 */
enum pipeline_req_type {
	/* Port IN */
	PIPELINE_REQ_PORT_IN_ENABLE,
	PIPELINE_REQ_PORT_IN_DISABLE,

	PIPELINE_REQ_MAX
};

struct pipeline_msg_req {
	enum pipeline_req_type type;
	uint32_t id; /* Port IN, port OUT or table ID */
};

struct pipeline_msg_rsp {
	int status;
};

/**
 * Master thread
 */
static struct pipeline_msg_req *
pipeline_msg_alloc(void)
{
	size_t size = RTE_MAX(sizeof(struct pipeline_msg_req),
		sizeof(struct pipeline_msg_rsp));

	return calloc(1, size);
}

static void
pipeline_msg_free(struct pipeline_msg_rsp *rsp)
{
	free(rsp);
}

static struct pipeline_msg_rsp *
pipeline_msg_send_recv(struct pipeline *p,
	struct pipeline_msg_req *req)
{
	struct rte_ring *msgq_req = p->msgq_req;
	struct rte_ring *msgq_rsp = p->msgq_rsp;
	struct pipeline_msg_rsp *rsp;
	int status;

	/* send */
	do {
		status = rte_ring_sp_enqueue(msgq_req, req);
	} while (status == -ENOBUFS);

	/* recv */
	do {
		status = rte_ring_sc_dequeue(msgq_rsp, (void **)&rsp);
	} while (status != 0);

	return rsp;
}

int
softnic_pipeline_port_in_enable(struct pmd_internals *softnic,
	const char *pipeline_name,
	uint32_t port_id)
{
	struct pipeline *p;
	struct pipeline_msg_req *req;
	struct pipeline_msg_rsp *rsp;
	int status;

	/* Check input params */
	if (pipeline_name == NULL)
		return -1;

	p = softnic_pipeline_find(softnic, pipeline_name);
	if (p == NULL ||
		port_id >= p->n_ports_in)
		return -1;

	if (!pipeline_is_running(p)) {
		status = rte_pipeline_port_in_enable(p->p, port_id);
		return status;
	}

	/* Allocate request */
	req = pipeline_msg_alloc();
	if (req == NULL)
		return -1;

	/* Write request */
	req->type = PIPELINE_REQ_PORT_IN_ENABLE;
	req->id = port_id;

	/* Send request and wait for response */
	rsp = pipeline_msg_send_recv(p, req);
	if (rsp == NULL)
		return -1;

	/* Read response */
	status = rsp->status;

	/* Free response */
	pipeline_msg_free(rsp);

	return status;
}

int
softnic_pipeline_port_in_disable(struct pmd_internals *softnic,
	const char *pipeline_name,
	uint32_t port_id)
{
	struct pipeline *p;
	struct pipeline_msg_req *req;
	struct pipeline_msg_rsp *rsp;
	int status;

	/* Check input params */
	if (pipeline_name == NULL)
		return -1;

	p = softnic_pipeline_find(softnic, pipeline_name);
	if (p == NULL ||
		port_id >= p->n_ports_in)
		return -1;

	if (!pipeline_is_running(p)) {
		status = rte_pipeline_port_in_disable(p->p, port_id);
		return status;
	}

	/* Allocate request */
	req = pipeline_msg_alloc();
	if (req == NULL)
		return -1;

	/* Write request */
	req->type = PIPELINE_REQ_PORT_IN_DISABLE;
	req->id = port_id;

	/* Send request and wait for response */
	rsp = pipeline_msg_send_recv(p, req);
	if (rsp == NULL)
		return -1;

	/* Read response */
	status = rsp->status;

	/* Free response */
	pipeline_msg_free(rsp);

	return status;
}

/**
 * Data plane threads: message handling
 */
static inline struct pipeline_msg_req *
pipeline_msg_recv(struct rte_ring *msgq_req)
{
	struct pipeline_msg_req *req;

	int status = rte_ring_sc_dequeue(msgq_req, (void **)&req);

	if (status != 0)
		return NULL;

	return req;
}

static inline void
pipeline_msg_send(struct rte_ring *msgq_rsp,
	struct pipeline_msg_rsp *rsp)
{
	int status;

	do {
		status = rte_ring_sp_enqueue(msgq_rsp, rsp);
	} while (status == -ENOBUFS);
}

static struct pipeline_msg_rsp *
pipeline_msg_handle_port_in_enable(struct pipeline_data *p,
	struct pipeline_msg_req *req)
{
	struct pipeline_msg_rsp *rsp = (struct pipeline_msg_rsp *)req;
	uint32_t port_id = req->id;

	rsp->status = rte_pipeline_port_in_enable(p->p,
		port_id);

	return rsp;
}

static struct pipeline_msg_rsp *
pipeline_msg_handle_port_in_disable(struct pipeline_data *p,
	struct pipeline_msg_req *req)
{
	struct pipeline_msg_rsp *rsp = (struct pipeline_msg_rsp *)req;
	uint32_t port_id = req->id;

	rsp->status = rte_pipeline_port_in_disable(p->p,
		port_id);

	return rsp;
}

static void
pipeline_msg_handle(struct pipeline_data *p)
{
	for ( ; ; ) {
		struct pipeline_msg_req *req;
		struct pipeline_msg_rsp *rsp;

		req = pipeline_msg_recv(p->msgq_req);
		if (req == NULL)
			break;

		switch (req->type) {
		case PIPELINE_REQ_PORT_IN_ENABLE:
			rsp = pipeline_msg_handle_port_in_enable(p, req);
			break;

		case PIPELINE_REQ_PORT_IN_DISABLE:
			rsp = pipeline_msg_handle_port_in_disable(p, req);
			break;

		default:
			rsp = (struct pipeline_msg_rsp *)req;
			rsp->status = -1;
		}

		pipeline_msg_send(p->msgq_rsp, rsp);
	}
}

/**
 * Data plane threads: main
 */
int
rte_pmd_softnic_run(uint16_t port_id)
{
	struct rte_eth_dev *dev = &rte_eth_devices[port_id];
	struct pmd_internals *softnic;
	struct softnic_thread_data *t;
	uint32_t thread_id, j;

#ifdef RTE_LIBRTE_ETHDEV_DEBUG
	RTE_ETH_VALID_PORTID_OR_ERR_RET(port_id, 0);
#endif

	softnic = dev->data->dev_private;
	thread_id = rte_lcore_id();
	t = &softnic->thread_data[thread_id];
	t->iter++;

	/* Data Plane */
	for (j = 0; j < t->n_pipelines; j++)
		rte_pipeline_run(t->p[j]);

	/* Control Plane */
	if ((t->iter & 0xFLLU) == 0) {
		uint64_t time = rte_get_tsc_cycles();
		uint64_t time_next_min = UINT64_MAX;

		if (time < t->time_next_min)
			return 0;

		/* Pipeline message queues */
		for (j = 0; j < t->n_pipelines; j++) {
			struct pipeline_data *p =
				&t->pipeline_data[j];
			uint64_t time_next = p->time_next;

			if (time_next <= time) {
				pipeline_msg_handle(p);
				rte_pipeline_flush(p->p);
				time_next = time + p->timer_period;
				p->time_next = time_next;
			}

			if (time_next < time_next_min)
				time_next_min = time_next;
		}

		/* Thread message queues */
		{
			uint64_t time_next = t->time_next;

			if (time_next <= time) {
				thread_msg_handle(t);
				time_next = time + t->timer_period;
				t->time_next = time_next;
			}

			if (time_next < time_next_min)
				time_next_min = time_next;
		}

		t->time_next_min = time_next_min;
	}

	return 0;
}
