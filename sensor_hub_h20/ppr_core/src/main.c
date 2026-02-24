/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/drivers/spi.h>

#include <zephyr/pm/pm.h>

#include <hal/nrf_vpr_csr.h>
#include <hal/nrf_gpio.h>
#include <nrfx_spim.h>
#include <dmm.h>

#include "sensor_data.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ppr_main, LOG_LEVEL_DBG);

#define READ_INTERVAL_MS 100

/** SPI settings */
#if defined(CONFIG_SPI)
#define SPI_MODE (SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_TRANSFER_LSB)
#define SPIM_OP	 (SPI_OP_MODE_MASTER | SPI_MODE)
struct spi_dt_spec spim = SPI_DT_SPEC_GET(DT_NODELABEL(dut_spi_dt), SPIM_OP);
#endif

#if defined(CONFIG_APP_USE_NRFX_DRIVERS)
// #define SPI_NODE DT_NODELABEL(spi130)
// #define SPI_PINCTRL_NODE DT_CHILD(DT_PINCTRL_0(SPI_NODE, 0), group1)
// #define SCK_PIN (DT_PROP_BY_IDX(SPI_PINCTRL_NODE, psels, 0) & 0x3F)
// #define MISO_PIN (DT_PROP_BY_IDX(SPI_PINCTRL_NODE, psels, 1) & 0x3F)
// #define MOSI_PIN (DT_PROP_BY_IDX(SPI_PINCTRL_NODE, psels, 2) & 0x3F)

static nrfx_spim_t spim_nrfx = NRFX_SPIM_INSTANCE(NRF_SPIM_INST_GET(130));
static nrfx_spim_config_t spim_nrfx_config = {
	.frequency = NRFX_MHZ_TO_HZ(8),
	.miso_pin = NRF_PIN_PORT_TO_PIN_NUMBER(0, 6),
	.mosi_pin = NRF_PIN_PORT_TO_PIN_NUMBER(0, 7),
	.sck_pin = NRF_PIN_PORT_TO_PIN_NUMBER(0, 0),
	// .ss_pin = NRF_PIN_PORT_TO_PIN_NUMBER(0, 10), // <-- some issues with this
	.ss_pin = NRF_SPIM_PIN_NOT_CONNECTED,
	.ss_active_high = false,
	.irq_priority = NRFX_SPIM_DEFAULT_CONFIG_IRQ_PRIORITY,
	.orc = 0x00,
	.mode = NRF_SPIM_MODE_0,
	.bit_order = NRF_SPIM_BIT_ORDER_LSB_FIRST,
	.miso_pull = NRF_GPIO_PIN_NOPULL,
	.rx_delay = 1,
	.skip_gpio_cfg = false,
	.skip_psel_cfg = false,
};

// static nrfx_spim_config_t spim_nrfx_config =
// 	NRFX_SPIM_DEFAULT_CONFIG(NRF_PIN_PORT_TO_PIN_NUMBER(0, 0),
// 				 NRF_PIN_PORT_TO_PIN_NUMBER(0, 7),
// 				 NRF_PIN_PORT_TO_PIN_NUMBER(0, 6),
// 				 NRF_SPIM_PIN_NOT_CONNECTED);
#endif

/* Mock data for SPI loopback transfer. */
static struct sensor_data tx_data;

/* Buffer for received data.
 * Will be set to the specific place in ipc_data[] array.
 */
static struct sensor_data_batch batch_data;

#if defined(CONFIG_SPI)
/* SPI transmit buffer for mock data. */
static struct spi_buf tx_buf  = {
	.buf = tx_data.data,
	.len = sizeof(tx_data.data),
};
static struct spi_buf_set tx_buffer_set  = {
	.buffers = &tx_buf,
	.count = 1,
};

/* SPI receive buffer for loopback transfer data. */
static struct spi_buf rx_buf;
static struct spi_buf_set rx_buffer_set  = {
	.buffers = &rx_buf,
	.count = 1,
};
#endif

#if defined(CONFIG_APP_USE_NRFX_DRIVERS)
#define USER_SPIM_XFER_BUF_SIZE 64
#define SPI130_NODELABEL DT_NODELABEL(spi130)
static void *mem_reg = DMM_DEV_TO_REG(SPI130_NODELABEL);
static uint8_t rx_buffer[USER_SPIM_XFER_BUF_SIZE] DMM_MEMORY_SECTION(SPI130_NODELABEL);
static uint8_t tx_buffer[USER_SPIM_XFER_BUF_SIZE] DMM_MEMORY_SECTION(SPI130_NODELABEL);
BUILD_ASSERT(sizeof(struct sensor_data) <= sizeof(tx_buffer));
BUILD_ASSERT(sizeof(struct sensor_data) <= sizeof(rx_buffer));

static nrfx_spim_xfer_desc_t xfer_buffer_set =
	NRFX_SPIM_XFER_TRX(tx_buffer, sizeof(tx_buffer),
			   rx_buffer, sizeof(rx_buffer));
#endif

/* IPC endpoint for communication with app core. */
static const struct device *ipc1_instance = DEVICE_DT_GET(DT_NODELABEL(ipc1));
static struct ipc_ept ep;
#if defined(CONFIG_MULTITHREADING)
static K_SEM_DEFINE(ep_bound_sem, 0, 1);
#else
static volatile bool ep_bound_flag = false;
#endif

static void ep_bound(void *priv)
{
#if defined(CONFIG_MULTITHREADING)
	k_sem_give(&ep_bound_sem);
#else
	ep_bound_flag = true;
#endif
}

static void ep_recv(const void *data, size_t len, void *priv)
{
	/* Left empty on purpose. */
}

static struct ipc_ept_cfg ep_cfg = {
	.name = "ep0",
	.cb = {
		.bound    = ep_bound,
		.received = ep_recv,
	},
};

static void ipc_send(void)
{
	int ret;
	while (true) {
		ret = ipc_service_send(&ep, &batch_data, sizeof(batch_data));
		if (ret == -ENOMEM) {
			/* No space in the buffer. Retry. */
			continue;
		} else if (ret < 0) {
			LOG_ERR("send_message failed with ret %d", ret);
		}
		break;
	}
}

#if defined(CONFIG_MULTITHREADING)
static void ipc_send_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	ipc_send();
}

static K_WORK_DEFINE(ipc_send_work, ipc_send_work_handler);
#endif

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

#if defined(CONFIG_MULTITHREADING)
	k_sem_take(&ep_bound_sem, K_FOREVER);
#else
	while (!ep_bound_flag) {
		k_msleep(10);
	}
#endif

	return 0;
}

#if !defined(CONFIG_MULTITHREADING) || !defined(CONFIG_PM)
static void hibernate_sleep_state_set(void)
{
	csr_write(VPRCSR_NORDIC_VPRNORDICSLEEPCTRL,
		VPRCSR_NORDIC_VPRNORDICSLEEPCTRL_SLEEPSTATE_HIBERNATE);
	nrf_barrier_w();
}

static void wait_sleep_state_set(void)
{
	csr_write(VPRCSR_NORDIC_VPRNORDICSLEEPCTRL,
		VPRCSR_NORDIC_VPRNORDICSLEEPCTRL_SLEEPSTATE_WAIT);
	nrf_barrier_w();
}

/** ASSUMPTION: Only this function is configuring the VPR sleep state. */
static void sleep(k_timeout_t timeout)
{
	static const k_timeout_t hibernate_min_residency = K_USEC(200);

	/* Setup the sleep state based on the minimum residency time of the power state. */
	if (timeout.ticks >= hibernate_min_residency.ticks) {
		hibernate_sleep_state_set();
	}

	k_sleep(timeout);

	/* Set default sleep state. */
	wait_sleep_state_set();
}
#endif

static char generate_mock_sample(void)
{
	static char val = 'Z';

	val = (val == 'Z') ? 'A' : val + 1;

	return val;
}

static void spi_xfer_thread_handler(void *arg1, void *arg2, void *arg3) {
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int spi_xfer_cnt = 0;
	int err;

	while (1) {
		/* Setup mock data for SPI loopback transfer. */
		tx_data.data[0] = generate_mock_sample();
		tx_data.data[1] = generate_mock_sample();
		tx_data.data[2] = generate_mock_sample();
		tx_data.data[3] = generate_mock_sample();
		tx_data.data[4] = generate_mock_sample();
		tx_data.data[5] = generate_mock_sample();
		tx_data.data[6] = generate_mock_sample();
		tx_data.data[7] = generate_mock_sample();
		tx_data.data[8] = generate_mock_sample();
		tx_data.data[9] = generate_mock_sample();
#if defined(CONFIG_APP_USE_NRFX_DRIVERS)
		memset(tx_buffer, 0, sizeof(tx_buffer));
		memcpy(tx_buffer, tx_data.data, sizeof(tx_data.data));
#endif

		/* Setup buffer for received data. */
#if !defined(CONFIG_APP_USE_NRFX_DRIVERS)
		rx_buf.buf = batch_data.data[spi_xfer_cnt].data;
		rx_buf.len = sizeof(batch_data.data[spi_xfer_cnt].data);
		memset(rx_buf.buf, 0, rx_buf.len);
#else
		memset(rx_buffer, 0, sizeof(rx_buffer));
#endif

#if !defined(CONFIG_APP_USE_NRFX_DRIVERS)
#if defined(CONFIG_MULTITHREADING) && defined(CONFIG_PM)
		/* CONFIG_PM will automatically power up the SPI bus device. */
#else
		/* Initialize SPI bus to power up the bus device. */
		device_init(spim.bus);
#endif
#else
		err = nrfx_spim_init(&spim_nrfx, &spim_nrfx_config, NULL, NULL);
		LOG_INF("nrfx_spim_init error: %d", err);
#endif

		/* Perform SPI loopback transfer. */
#if !defined(CONFIG_APP_USE_NRFX_DRIVERS)
		spi_transceive_dt(&spim, &tx_buffer_set, &rx_buffer_set);
#else
		xfer_buffer_set.p_tx_buffer = NULL;
		xfer_buffer_set.p_rx_buffer = NULL;
		xfer_buffer_set.tx_length = sizeof(tx_buffer);
		xfer_buffer_set.rx_length = sizeof(rx_buffer);

		err = dmm_buffer_out_prepare(mem_reg, tx_buffer, xfer_buffer_set.tx_length, (void **)&xfer_buffer_set.p_tx_buffer);
		LOG_INF("dmm_buffer_out_prepare error: %d", err);
		err = dmm_buffer_in_prepare(mem_reg, rx_buffer, xfer_buffer_set.rx_length, (void **)&xfer_buffer_set.p_rx_buffer);
		LOG_INF("dmm_buffer_in_prepare error: %d", err);

		LOG_HEXDUMP_INF(xfer_buffer_set.p_tx_buffer, xfer_buffer_set.tx_length, "TX buffer");

		err = nrfx_spim_xfer(&spim_nrfx, &xfer_buffer_set, 0);
		LOG_INF("nrfx_spim_xfer error: %d", err);

		LOG_HEXDUMP_INF(xfer_buffer_set.p_rx_buffer, xfer_buffer_set.rx_length, "RX buffer");
		memcpy(batch_data.data[spi_xfer_cnt].data, rx_buffer, sizeof(batch_data.data[spi_xfer_cnt].data));

		dmm_buffer_in_release(mem_reg, rx_buffer, sizeof(rx_buffer), (void **)&xfer_buffer_set.p_rx_buffer);
		dmm_buffer_out_release(mem_reg, (void **)&xfer_buffer_set.p_tx_buffer);
#endif

#if !defined(CONFIG_APP_USE_NRFX_DRIVERS)
#if defined(CONFIG_MULTITHREADING) && defined(CONFIG_PM)
		/* CONFIG_PM will automatically power down the SPI bus device. */
#else
		/* Deinitialize SPI bus to power down the bus device. */
		device_deinit(spim.bus);
#endif
#else
		nrfx_spim_uninit(&spim_nrfx);
#endif

		/* Send IPC data if we have reached the required number of samples. */
		if (spi_xfer_cnt == (SENSOR_DATA_BATCH_CNT - 1)) {
#if defined(CONFIG_MULTITHREADING)
			k_work_submit(&ipc_send_work);
#else
			ipc_send();
#endif
			spi_xfer_cnt = 0;
		} else {
			spi_xfer_cnt++;
		}

#if defined(CONFIG_MULTITHREADING) && defined(CONFIG_PM)
		/* Rely on default sleep implemenatation.
		 * When CONFIG_PM=y, it will automatically enter required VPR sleep mode
		 * in the idle() thread.
		 */
		k_sleep(K_MSEC(READ_INTERVAL_MS));
#else
		/* Use custom sleep implementation.
		 * As CONFIG_PM is not supported when CONFIG_MULTITHREADING=n,
		 * manually enter the required VPR sleep mode before calling k_sleep(). */
		sleep(K_MSEC(READ_INTERVAL_MS));
#endif
	}
}

#if defined(CONFIG_MULTITHREADING)
K_THREAD_DEFINE(thread_spi_xfer, 1024, spi_xfer_thread_handler, NULL, NULL, NULL,
		K_PRIO_COOP(1), 0, -1);
#endif

int main(void)
{
	int err;

	LOG_INF("PPR core started");

	/* Wait for app core to start. */
	k_busy_wait(1000000);

	err = ipc_init();
	if (err) {
		LOG_ERR("Failed to initialize IPC: %d", err);
		return err;
	}

#if defined(CONFIG_MULTITHREADING)
	k_thread_start(thread_spi_xfer);
#else
	spi_xfer_thread_handler(NULL, NULL, NULL);

	/* Should never reach here. */
	k_panic();
#endif
	return 0;
}
