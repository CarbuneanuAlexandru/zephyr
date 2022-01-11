/* main.c - Application main entry point */

/*
 * Copyright (c) 2015-2016 Intel Corporation
 * Copyright (c) 2021 Nordic Semiconductor
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include <zephyr/types.h>
#include <sys/util.h>
#include <sys/byteorder.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/l2cap.h>
#include "bs_types.h"
#include "bs_tracing.h"
#include "bstests.h"

#define LOG_MODULE_NAME main_l2cap_ecred
#include <logging/log.h>
LOG_MODULE_REGISTER(LOG_MODULE_NAME, LOG_LEVEL_DBG);

extern enum bst_result_t bst_result;
#define CREATE_FLAG(flag) static atomic_t flag = (atomic_t)false
#define SET_FLAG(flag) (void)atomic_set(&flag, (atomic_t)true)
#define UNSET_FLAG(flag) (void)atomic_set(&flag, (atomic_t)false)
#define WAIT_FOR_FLAG_SET(flag)		   \
	while (!(bool)atomic_get(&flag)) { \
		(void)k_sleep(K_MSEC(1));  \
	}
#define WAIT_FOR_FLAG_UNSET(flag)	  \
	while ((bool)atomic_get(&flag)) { \
		(void)k_sleep(K_MSEC(1)); \
	}

#define FAIL(...)				       \
	do {					       \
		bst_result = Failed;		       \
		bs_trace_error_time_line(__VA_ARGS__); \
	} while (0)

#define PASS(...)				    \
	do {					    \
		bst_result = Passed;		    \
		bs_trace_info_time(1, __VA_ARGS__); \
	} while (0)

static struct bt_conn *default_conn;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

#define DATA_MTU 2000
#define DATA_MPS 65
#define DATA_BUF_SIZE BT_L2CAP_SDU_BUF_SIZE(DATA_MTU)
#define L2CAP_CHANNELS 2
#define SERVERS 1
#define SDU_SEND_COUNT 40

NET_BUF_POOL_FIXED_DEFINE(rx_data_pool, L2CAP_CHANNELS, BT_L2CAP_BUF_SIZE(DATA_BUF_SIZE), 8, NULL);
NET_BUF_POOL_FIXED_DEFINE(tx_data_pool, L2CAP_CHANNELS + 1, BT_L2CAP_BUF_SIZE(DATA_MTU), 8, NULL);

static struct bt_l2cap_server servers[SERVERS];
void send_sdu_chan_worker(struct k_work *item);

static struct channel {
	uint8_t chan_id; /* Internal number that identifies L2CAP channel. */
	struct bt_l2cap_le_chan le;
	bool in_use;
	int sdus_received;
	int bytes_to_send;
	int itteration;
	struct net_buf *buf;
	struct k_work work;
	uint8_t payload[DATA_MTU];
} channels[L2CAP_CHANNELS];
CREATE_FLAG(is_connected);
#define MY_STACK_SIZE 512
#define MY_PRIORITY 5

K_THREAD_STACK_DEFINE(my_stack_area0, MY_STACK_SIZE);
struct k_work_q my_work_q0;
K_THREAD_STACK_DEFINE(my_stack_area1, MY_STACK_SIZE);
struct k_work_q my_work_q1;

static void init_workqs(void)
{
	k_work_queue_init(&my_work_q0);
	k_work_queue_start(&my_work_q0, my_stack_area0,
			   K_THREAD_STACK_SIZEOF(my_stack_area0), MY_PRIORITY,
			   NULL);

	k_work_queue_init(&my_work_q1);
	k_work_queue_start(&my_work_q1, my_stack_area1,
			   K_THREAD_STACK_SIZEOF(my_stack_area1), MY_PRIORITY,
			   NULL);
}

static struct channel *get_free_channel(void)
{
	struct channel *chan;

	for (int idx = 0; idx < L2CAP_CHANNELS; idx++) {
		if (channels[idx].in_use) {
			continue;
		}
		chan = &channels[idx];
		(void)memset(chan, 0, sizeof(*chan));
		chan->chan_id = idx;
		channels[idx].in_use = true;
		memset(chan->payload, idx, sizeof(chan->payload));
		k_work_init(&chan->work, send_sdu_chan_worker);
		return chan;
	}

	return NULL;
}

static struct net_buf *chan_alloc_buf_cb(struct bt_l2cap_chan *chan)
{
	LOG_DBG("Allocated on chan %p", chan);
	return net_buf_alloc(&rx_data_pool, K_FOREVER);
}

static int chan_recv_cb(struct bt_l2cap_chan *l2cap_chan, struct net_buf *buf)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);
	int received_iterration = *((int *)(buf->data));

	LOG_DBG("received_iterration %i sdus_receied %i, chan_id: %d, data_length: %d", received_iterration,  chan->sdus_received, chan->chan_id, buf->len);
	if (received_iterration != chan->sdus_received) {
		FAIL("Received out of sequence data.");
	}
	int retval = memcmp(buf->data + sizeof(int), chan->payload + sizeof(int), buf->len - sizeof(int));
	if (retval) {
		FAIL("Payload received didn't match expected value memcmp returned %i", retval);
	}
	if (chan->chan_id == 0) {
		/*By the time we rx on chan 0, we should have alrady received on chan1*/
		if ((channels[1].sdus_received != (channels[0].sdus_received + 1))) {
			FAIL("Didn't receive on channel 1 first: channels[0].sdus_received:%i channels[1].sdus_received:%i",
			     channels[0].sdus_received,
			     channels[1].sdus_received);
		}
	}
	chan->sdus_received++;
	return 0;
}

static void chan_sent_cb(struct bt_l2cap_chan *l2cap_chan)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);

	chan->buf = 0;

	LOG_DBG("chan_id: %d", chan->chan_id);
}

static volatile int num_connect_chans;
static void chan_connected_cb(struct bt_l2cap_chan *l2cap_chan)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);

	LOG_DBG("chan_id: %d", chan->chan_id);

	LOG_DBG("tx.mtu %d, tx.mps: %d, rx.mtu: %d, rx.mps %d", sys_cpu_to_le16(chan->le.tx.mtu),
		sys_cpu_to_le16(chan->le.tx.mps), sys_cpu_to_le16(chan->le.rx.mtu),
		sys_cpu_to_le16(chan->le.rx.mps));

	num_connect_chans++;
}

static void chan_disconnected_cb(struct bt_l2cap_chan *l2cap_chan)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);

	LOG_DBG("chan_id: %d", chan->chan_id);

	chan->in_use = false;
}

static void chan_status_cb(struct bt_l2cap_chan *l2cap_chan, atomic_t *status)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);

	LOG_DBG("chan_id: %d, status: %ld", chan->chan_id, *status);
}

static void chan_released_cb(struct bt_l2cap_chan *l2cap_chan)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);

	LOG_DBG("chan_id: %d", chan->chan_id);
}

static void chan_reconfigured_cb(struct bt_l2cap_chan *l2cap_chan)
{
	struct channel *chan = CONTAINER_OF(l2cap_chan, struct channel, le);

	LOG_DBG("chan_id: %d", chan->chan_id);
}

static const struct bt_l2cap_chan_ops l2cap_ops = {
	.alloc_buf = chan_alloc_buf_cb,
	.recv = chan_recv_cb,
	.sent = chan_sent_cb,
	.connected = chan_connected_cb,
	.disconnected = chan_disconnected_cb,
	.status = chan_status_cb,
	.released = chan_released_cb,
	.reconfigured = chan_reconfigured_cb,
};

static void connect_num_channels(uint8_t num_l2cap_channels)
{
	struct channel *chan = NULL;

	#define L2CAP_ECRED_CHAN_MAX 5
	struct bt_l2cap_chan *allocated_channels[L2CAP_ECRED_CHAN_MAX] = { NULL };
	#undef L2CAP_ECRED_CHAN_MAX
	int err = 0;
	for (int i = 0; i < num_l2cap_channels; i++) {
		chan = get_free_channel();
		if (!chan) {
			FAIL("failed, chan not free");
			return;
		}
		chan->le.chan.ops = &l2cap_ops;
		chan->le.rx.mtu = DATA_MTU;
		chan->le.rx.mps = DATA_MPS;
		allocated_channels[i] = &chan->le.chan;
	}

	err = bt_l2cap_ecred_chan_connect(default_conn, allocated_channels, servers[0].psm);
	if (err) {
		FAIL("can't connect ecred %d ", err);
	}
}

static void disconnect_all_channels(void)
{
	int err = 0;

	for (int i = 0; i < ARRAY_SIZE(channels); i++) {
		if (channels[i].in_use) {
			LOG_DBG("Disconnecting channel: %d)", channels[i].chan_id);
			err = bt_l2cap_chan_disconnect(&channels[i].le.chan);
			if (err) {
				LOG_DBG("can't disconnnect channel (err: %d)", err);
			}
			channels[i].in_use = false;
		}
	}
}

static int accept(struct bt_conn *conn, struct bt_l2cap_chan **l2cap_chan)
{
	struct channel *chan;

	chan = get_free_channel();
	if (!chan) {
		return -ENOMEM;
	}

	chan->le.chan.ops = &l2cap_ops;
	chan->le.tx.mtu = DATA_MTU;
	chan->le.rx.mtu = DATA_MTU;

	*l2cap_chan = &chan->le.chan;

	return 0;
}

static struct bt_l2cap_server *get_free_server(void)
{
	for (int i = 0U; i < SERVERS; i++) {
		if (servers[i].psm) {
			continue;
		}

		return &servers[i];
	}

	return NULL;
}

static void register_l2cap_server(void)
{
	struct bt_l2cap_server *server;

	server = get_free_server();
	if (!server) {
		FAIL("Failed to get free server");
		return;
	}

	server->accept = accept;
	server->psm = 0;

	if (bt_l2cap_server_register(server) < 0) {
		FAIL("Failed to get free server");
		return;
	}

	LOG_DBG("L2CAP server registered, PSM:0x%X", server->psm);
}

static void connected(struct bt_conn *conn, uint8_t conn_err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (conn_err) {
		FAIL("Failed to connect to %s (%u)", addr, conn_err);
		bt_conn_unref(default_conn);
		default_conn = NULL;
		return;
	}

	default_conn = bt_conn_ref(conn);
	LOG_DBG("%s", addr);

	SET_FLAG(is_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	LOG_DBG("%s (reason 0x%02x)", addr, reason);

	if (default_conn != conn) {
		FAIL("Conn mismatch disconnect %s %s)", default_conn, conn);
		return;
	}

	bt_conn_unref(default_conn);
	default_conn = NULL;
	UNSET_FLAG(is_connected);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};
static void send_sdu(int itteration, int chan_idx, int bytes);

void send_sdu_chan_worker(struct k_work *item)
{
	struct channel  *ch =
		CONTAINER_OF(item, struct channel, work);

	send_sdu(ch->itteration, ch->chan_id, ch->bytes_to_send);
}

static void send_sdu(int itteration, int chan_idx, int bytes)
{
	struct bt_l2cap_chan *chan = &channels[chan_idx].le.chan;

	*((int *)channels[chan_idx].payload) = itteration; /*First 4 bytes in sent payload is itteration count*/
	if (channels[chan_idx].buf != 0) {
		LOG_ERR("Buf should have been dealocated by now");
	}
	struct net_buf *buf = net_buf_alloc(&tx_data_pool, K_NO_WAIT);

	if (buf == 0) {
		LOG_ERR("Failed to get buff on ch %i, iteration %i should never happen", chan_idx, chan_idx);
	}
	channels[chan_idx].buf = buf;
	net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, channels[chan_idx].payload, bytes);

	LOG_DBG("bt_l2cap_chan_sending ch: %i bytes: %i itteration: %i", chan_idx, bytes, itteration);
	int ret = bt_l2cap_chan_send(chan, buf);

	LOG_DBG("bt_l2cap_chan_send returned: %i", ret);

	if (ret < 0) {
		LOG_DBG("Error: send failed error: %i", ret);
		channels[chan_idx].buf = 0;
		net_buf_unref(buf);
	}
}

static void test_peripheral_main(void)
{
	int err;

	LOG_DBG("*L2CAP ECRED Peripheral started*");
	init_workqs();
	err = bt_enable(NULL);
	if (err) {
		FAIL("Can't enable Bluetooth (err %d)", err);
		return;
	}
	LOG_DBG("Peripheral Bluetooth initialized.");

	LOG_DBG("Connectable advertising...");
	err = bt_le_adv_start(BT_LE_ADV_CONN_NAME, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		FAIL("Advertising failed to start (err %d)", err);
		return;
	}
	LOG_DBG("Advertising started.");

	LOG_DBG("Peripheral waiting for connection...");
	WAIT_FOR_FLAG_SET(is_connected);
	LOG_DBG("Peripheral Connected.");

	/* Wait a bit to ensure that all LLCP have time to finish */
	k_sleep(K_MSEC(1000));

	register_l2cap_server();

	connect_num_channels(L2CAP_CHANNELS);

	k_sleep(K_MSEC(500));
	for (int i = 0; i < SDU_SEND_COUNT; i++) {
		LOG_DBG("Matv: Iteration %i Sendign on chan0", i);
		channels[0].itteration = i;
		channels[0].bytes_to_send = DATA_MTU - 500;
		k_work_submit_to_queue(&my_work_q0, &channels[0].work);
		LOG_DBG("Matv: Iteration %i Sendign on chan1", i);
		channels[1].itteration = i;
		channels[1].bytes_to_send = DATA_MPS - 2;
		k_work_submit_to_queue(&my_work_q1, &channels[1].work);
		k_sleep(K_MSEC(5000));
	}

	disconnect_all_channels();
	/* Disconnect */
	LOG_DBG("Peripheral Disconnecting....");
	err = bt_conn_disconnect(default_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	LOG_DBG("Peripheral tried to disconnect");
	if (err) {
		FAIL("Disconnection failed (err %d)", err);
		return;
	}
	WAIT_FOR_FLAG_UNSET(is_connected);
	LOG_DBG("Peripheral Disconnected.");
	k_sleep(K_MSEC(100));/*give a litle time for central to finish processing of disconnect event*/
	PASS("L2CAP ECRED Peripheral tests Passed");
	bs_trace_silent_exit(0);
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct bt_le_conn_param *param;
	int err;

	err = bt_le_scan_stop();
	if (err) {
		FAIL("Stop LE scan failed (err %d)", err);
		return;
	}

	param = BT_LE_CONN_PARAM_DEFAULT;
	err = bt_conn_le_create(addr, BT_CONN_LE_CREATE_CONN, param, &default_conn);
	if (err) {
		FAIL("Create conn failed (err %d)", err);
		return;
	}
}

static void test_central_main(void)
{
	struct bt_le_scan_param scan_param = {
		.type = BT_LE_SCAN_TYPE_ACTIVE,
		.options = BT_LE_SCAN_OPT_NONE,
		.interval = BT_GAP_SCAN_FAST_INTERVAL,
		.window = BT_GAP_SCAN_FAST_WINDOW,
	};

	int err;

	LOG_DBG("*L2CAP ECRED Central started*");

	err = bt_enable(NULL);
	if (err) {
		FAIL("Can't enable Bluetooth (err %d)\n", err);
		return;
	}
	LOG_DBG("Central Bluetooth initialized.\n");

	err = bt_le_scan_start(&scan_param, device_found);
	if (err) {
		FAIL("Scanning failed to start (err %d)\n", err);
		return;
	}

	LOG_DBG("Scanning successfully started\n");

	LOG_DBG("Central waiting for connection...\n");
	WAIT_FOR_FLAG_SET(is_connected);
	LOG_DBG("Central Connected.\n");
	register_l2cap_server();
	/* Wait for disconnect */
	WAIT_FOR_FLAG_UNSET(is_connected);
	LOG_DBG("received PDUs on chan0 %i and chan1 %i", channels[0].sdus_received, channels[1].sdus_received);
	if (channels[0].sdus_received < SDU_SEND_COUNT || channels[1].sdus_received < SDU_SEND_COUNT) {
		FAIL("received less than %i", SDU_SEND_COUNT);
	}
	LOG_DBG("Central Disconnected.");

	PASS("L2CAP ECRED Central tests Passed\n");
}

static void test_init(void)
{
	bst_result = In_progress;
}
static void test_tick(bs_time_t HW_device_time)
{
}

static const struct bst_test_instance test_def[] = { { .test_id = "peripheral",
						       .test_descr = "Peripheral L2CAP ECRED",
						       .test_post_init_f = test_init,
						       .test_tick_f = test_tick,
						       .test_main_f = test_peripheral_main },
						     { .test_id = "central",
						       .test_descr = "Central L2CAP ECRED",
						       .test_post_init_f = test_init,
						       .test_tick_f = test_tick,
						       .test_main_f = test_central_main },
						     BSTEST_END_MARKER };

struct bst_test_list *test_main_l2cap_ecred_install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_def);
}
