/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>

#include <zephyr/ipc/ipc_service.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/flash.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#include "sensor_data.h"

static struct sensor_data_batch ipc_data;

/* External flash. */
#define SPI_FLASH_TEST_REGION_OFFSET 0xFF000

const struct device *flash_dev = DEVICE_DT_GET_ONE(jedec_mspi_nor);

static void write_ext_flash_work_handler(struct k_work *work)
{
	int rc;

	if (!device_is_ready(flash_dev)) {
		LOG_ERR("%s: device not ready", flash_dev->name);
		return;
	}

	rc = flash_write(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, ipc_data.data, sizeof(ipc_data.data));
	if (rc != 0) {
		LOG_ERR("Flash write failed: %d", rc);
	}

	LOG_INF("Flash written");
}
static K_WORK_DEFINE(write_ext_flash_work, write_ext_flash_work_handler);

/* Data used to present mock sensor data for advertising. */
struct manuf_data_item {
	uint16_t company_id;
	uint8_t ipc_xfer_count;
	uint8_t sample_first_byte[SENSOR_DATA_BATCH_CNT];
} __packed;

union manuf_data {
	struct manuf_data_item item;
	uint8_t data[sizeof(struct manuf_data_item)];
};

static union manuf_data manuf_data = {
	.item = {
		.company_id = 0x0059, /* Nordic Semiconductor ASA */
		.ipc_xfer_count = 0,
		.sample_first_byte = {0},
	}
};

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)

/* Use only the advertising data. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, &manuf_data.item, sizeof(manuf_data.item))
};

static void adv_update_work_handler(struct k_work *work)
{
	LOG_INF("Adv update handler");

	int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Failed to update advertisement data: %d", err);
	} else {
		LOG_INF("Advertisement data updated");
	}
}
static K_WORK_DEFINE(adv_update_work, adv_update_work_handler);

static void process_ipc_data(void)
{
	/* Let's call it "data processing".*/
	manuf_data.item.ipc_xfer_count = (manuf_data.item.ipc_xfer_count + 1) % 255;
	for (int i = 0; i < ARRAY_SIZE(manuf_data.item.sample_first_byte); i++) {
		manuf_data.item.sample_first_byte[i] = ipc_data.data[i].data[0];
	}

	if (IS_ENABLED(CONFIG_APP_BT_ADVERTISING)) {
		k_work_submit(&adv_update_work);
	} else {
		LOG_INF("BT advertising disabled, skipping adv update");
	}
}

/* IPC endpoint for communication with PPR core. */
static const struct device *ipc1_instance = DEVICE_DT_GET(DT_NODELABEL(ipc1));
static struct ipc_ept ep;
static K_SEM_DEFINE(bound_sem, 0, 1);

static void ep_bound(void *priv)
{
	k_sem_give(&bound_sem);
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	memcpy(&ipc_data, data, sizeof(ipc_data));
	LOG_HEXDUMP_DBG(ipc_data.data, sizeof(ipc_data.data), "IPC data");

	if (IS_ENABLED(CONFIG_APP_WRITE_EXT_FLASH)) {
		/* Write the batched sensor data from PPR core to external flash. */
		k_work_submit(&write_ext_flash_work);
	}

	/* Process the batched sensor data for advertising. */
	process_ipc_data();
}

static struct ipc_ept_cfg ep_cfg = {
	.name = "ep0",
	.cb = {
		.bound    = ep_bound,
		.received = ep_recv,
	},
};

static int ipc_init(void)
{
	int err;

	err = ipc_service_open_instance(ipc1_instance);
	if (err && (err != -EALREADY)) {
		return err;
	}

	err = ipc_service_register_endpoint(ipc1_instance, &ep, &ep_cfg);
	if (err) {
		return err;
	}

	k_sem_take(&bound_sem, K_FOREVER);

	LOG_INF("IPC initialized");

	return 0;
}

static int bt_stack_init(void)
{
	int err = 0;

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Failed to enable BT: %d", err);
		return err;
	}

	LOG_INF("BT enabled");

	if (IS_ENABLED(CONFIG_APP_BT_ADVERTISING)) {
		err = bt_le_adv_start(
			BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN,
					BT_GAP_MS_TO_ADV_INTERVAL(1000),
					BT_GAP_MS_TO_ADV_INTERVAL(1000), NULL),
					ad, ARRAY_SIZE(ad), NULL, 0);
		LOG_INF("BT advertising started");
	} else {
		LOG_INF("BT advertising disabled");
	}

	return err;
}

int main(void)
{
	int err;

	err = ipc_init();
	if (err) {
		LOG_ERR("Failed to initialize IPC: %d", err);
		return err;
	}

	err = bt_stack_init();
	if (err) {
		LOG_ERR("Failed to initialize BT stack: %d", err);
		return err;
	}

	return 0;
}
