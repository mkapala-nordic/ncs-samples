#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>

#include <zephyr/ipc/ipc_service.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/flash.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

/*
 * *** User input ***
 */
#define START_ADVERTISING 1
#define ADVERTISING_INTERVAL 1600 //in 625ms steps
#define SPI_SAMPLE_SIZE 35
#define SPI_SAMPLE_CNT 10
#define IPC_BUFFER_SIZE (SPI_SAMPLE_SIZE * SPI_SAMPLE_CNT)


static uint8_t ipc_data[IPC_BUFFER_SIZE];

/*
 * *** External flash settings ***
 */
#define SPI_FLASH_TEST_REGION_OFFSET 0xFF000
const struct device *flash_dev = DEVICE_DT_GET_ONE(jedec_mspi_nor);

static void write_ext_flash(struct k_work *work){
	if (!device_is_ready(flash_dev)) {
		printk("%s: device not ready.\n", flash_dev->name);
		return;
	}
	int rc;
	uint8_t len = 100;
	rc = flash_write(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, ipc_data, len);
	if (rc != 0) {
		printf("Flash write failed! %d\n", rc);
	}
	LOG_INF("Flash written");
}
K_WORK_DEFINE(thread_write_flash_id, write_ext_flash);

/*
 * *** BT settings ***
 */

union manuf_data {
	struct {
		uint16_t company_id;
		uint8_t ipc_xfer_count;
		uint8_t sample_first_byte[SPI_SAMPLE_CNT];
	};
	uint8_t data[sizeof(uint16_t) + sizeof(uint8_t) + (sizeof(uint8_t) * SPI_SAMPLE_CNT)];
};

static union manuf_data manuf_data = {
	.company_id = 0x0059, /* Nordic Semiconductor ASA */
	.ipc_xfer_count = 0,
	.sample_first_byte = {0},
};

#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, manuf_data.data, sizeof(manuf_data.data))
};

// static const struct bt_data sd[] = {
// };

static void adv_update_work_handler(struct k_work *work);
static K_WORK_DEFINE(adv_update_work, adv_update_work_handler);
static void adv_update_work_handler(struct k_work *work)
{
	LOG_INF("Adv update handler");

	// int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	int err = bt_le_adv_update_data(ad, ARRAY_SIZE(ad), NULL, 0);
	if (err) {
		LOG_ERR("Failed to update advertisement data: %d", err);
	} else {
		LOG_INF("Advertisement data updated");
	}
}

static void process_ipc_data(void)
{
	/* Let's call it "data processing".*/
	manuf_data.ipc_xfer_count = (manuf_data.ipc_xfer_count + 1) % 255;
	for (int i = 0; i < ARRAY_SIZE(manuf_data.sample_first_byte); i++) {
		size_t offset = i * SPI_SAMPLE_SIZE;
		manuf_data.sample_first_byte[i] = ipc_data[offset];
	}

	k_work_submit(&adv_update_work);
}

/*
 * *** IPC settings ***
 */
static K_SEM_DEFINE(bound_sem, 0, 1);
static void ep_bound(void *priv)
{
	k_sem_give(&bound_sem);
}
static void ep_recv(const void *data, size_t len, void *priv)
{
	// Write IPC data from PPR core directly to external flash
	memcpy(ipc_data, data, IPC_BUFFER_SIZE);
	LOG_HEXDUMP_DBG(ipc_data, IPC_BUFFER_SIZE, "IPC data");
	k_work_submit(&thread_write_flash_id);

	process_ipc_data();
}
static struct ipc_ept_cfg ep_cfg = {
	.name = "ep0",
	.cb = {
		.bound    = ep_bound,
		.received = ep_recv,
	},
};

int main(void)
{
	// Initialize IPC
	int ret;
	const struct device *ipc1_instance;
	struct ipc_ept ep;
	ipc1_instance = DEVICE_DT_GET(DT_NODELABEL(ipc1));

	ret = ipc_service_open_instance(ipc1_instance);
	if ((ret < 0) && (ret != -EALREADY)) {
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc1_instance, &ep, &ep_cfg);
	if (ret < 0) {
		return ret;
	}

	k_sem_take(&bound_sem, K_FOREVER);

	LOG_INF("IPC bound");

	LOG_INF("Enabling BT");
	bt_enable(NULL);
	LOG_INF("BT enabled");

#if START_ADVERTISING
	ret = bt_le_adv_start(
			BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADVERTISING_INTERVAL, 1600, NULL),
			ad, ARRAY_SIZE(ad), NULL, 0);
	LOG_INF("BT advertising started");
#endif


	return 0;

}
