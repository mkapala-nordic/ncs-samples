#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/spi.h>

/*
 * *** User input ***
 */
#define START_ADVERTISING 1
#define ADVERTISING_INTERVAL 1600 // in 625 ms steps

#define SPI_READ_INTERVAL 100
#define FLASH_WRITE_INTERVAL 10 //in number of SPI transactions
#define SPI_BUFFER_SIZE 35
#define FLASH_BUFFER_SIZE SPI_BUFFER_SIZE*FLASH_WRITE_INTERVAL

/*
 * *** SPI settings ***
 */
#define SPI_MODE (SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_TRANSFER_LSB)
#define SPIM_OP	 (SPI_OP_MODE_MASTER | SPI_MODE)

struct spi_dt_spec spim = SPI_DT_SPEC_GET(DT_NODELABEL(dut_spi_dt), SPIM_OP, 0);

static uint8_t tx_buffer[SPI_BUFFER_SIZE];
static struct spi_buf tx_buf  = {
	.buf = tx_buffer,
	.len = SPI_BUFFER_SIZE,
};
static struct spi_buf_set tx_buffer_set  = {
	.buffers = &tx_buf,
	.count = 1,
};

/*
 * *** BT settings ***
 */
#define DEVICE_NAME             CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN         (sizeof(DEVICE_NAME) - 1)

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

/*
 * *** External flash settings ***
 *
 * Dummy data stored in flash_data[] is written to flash, 
 * to show the current consumption assiciated with different payloads.
 * This can be changed to include SPI accumulated/processed data instead.
 */
const struct device *flash_dev = DEVICE_DT_GET_ONE(jedec_spi_nor);
#define SPI_FLASH_TEST_REGION_OFFSET 0xFF000
static uint8_t flash_data[FLASH_BUFFER_SIZE];
static void write_ext_flash(struct k_work *work){
	if (!device_is_ready(flash_dev)) {
		printk("%s: device not ready.\n", flash_dev->name);
		return;
	}
	int rc = flash_write(flash_dev, SPI_FLASH_TEST_REGION_OFFSET, flash_data, FLASH_BUFFER_SIZE);
	if (rc != 0) {
		printf("Flash write failed! %d\n", rc);
	}
}
K_WORK_DEFINE(thread_ext_flash_id, write_ext_flash);

/*
 * *** SPI read and flash write thread ***
 */
static void spi_tranceive_task(void *arg1, void *arg2, void *arg3) {
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int spi_read_counter = 0;
	while (1) {
		spi_read_dt(&spim, &tx_buffer_set);//, &rx_buffer_set);
		tx_buffer[0] = spi_read_counter;
		if(spi_read_counter%10 == 9){
			k_work_submit(&thread_ext_flash_id);
		}
		spi_read_counter++;
		k_msleep(SPI_READ_INTERVAL);
	}
}
K_THREAD_DEFINE(thread_spi_read, 1024, spi_tranceive_task, NULL, NULL, NULL,
		K_PRIO_COOP(1), 0, -1);

int main(void)
{
	//initialize external flash
	int ret;
	if (!device_is_ready(flash_dev)) {
		printk("%s: device not ready.\n", flash_dev->name);
		return 0;
	}

	//initialize BT
	bt_enable(NULL);
#if START_ADVERTISING
	ret = bt_le_adv_start(
			BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, ADVERTISING_INTERVAL, ADVERTISING_INTERVAL, NULL),
			ad, ARRAY_SIZE(ad), NULL, 0);
#endif

        // start SPI read thread
	k_thread_start(thread_spi_read);
	return 0;
}
