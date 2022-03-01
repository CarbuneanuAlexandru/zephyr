/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/att.h>
#include "common.h"

#define SAMPLE_DATA 1
#define NUM_NOTIF 10

CREATE_FLAG(flag_is_connected);
static struct bt_conn *t_conn;

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	if (err != 0) {
		FAIL("Failed to connect to %s (%u)\n", addr, err);
		return;
	}

	printk("Connected to %s\n", addr);

	t_conn = conn;
	SET_FLAG(flag_is_connected);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (conn != t_conn) {
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

	printk("Disconnected: %s (reason 0x%02x)\n", addr, reason);

	bt_conn_unref(t_conn);

	t_conn = NULL;
	UNSET_FLAG(flag_is_connected);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

void send_notification(void)
{
	uint8_t sample_dat = SAMPLE_DATA;
	int err;

	err = bt_gatt_notify(default_conn, local_attr, &sample_dat, sizeof(sample_dat));
	if (err) {
		printk("GATT notify failed (err %d)\n", err);
	}
}

static void test_main(void)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		FAIL("Bluetooth discover failed (err %d)\n", err);
	}

	err = bt_le_scan_start(BT_LE_SCAN_PASSIVE, device_found);
	if (err != 0) {
		FAIL("Scanning failed to start (err %d)\n", err);
	}

	printk("Scanning successfully started\n");

	WAIT_FOR_FLAG(flag_is_connected);

	err = bt_eatt_connect(default_conn, CONFIG_BT_EATT_MAX);
	if (err) {
		FAIL("Sending credit based connection request failed (err %d)\n", err);
	}

	printk("############# Notification EATT test\n");
	for (int indx; indx < NUM_NOTIF; indx++) {
		send_notification();
	}

	PASS("EATT client Passed\n");
}

static const struct bst_test_instance test_vcs[] = {
	{
		.test_id = "client",
		.test_post_init_f = test_init,
		.test_tick_f = test_tick,
		.test_main_f = test_main
	},
	BSTEST_END_MARKER
};

struct bst_test_list *test_gatt_client_install(struct bst_test_list *tests)
{
	return bst_add_tests(tests, test_vcs);
}
