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

#include "sensor_data.h"

#define READ_INTERVAL_MS 100

/** SPI settings */
#define SPI_MODE (SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_TRANSFER_LSB)
#define SPIM_OP	 (SPI_OP_MODE_MASTER | SPI_MODE)
struct spi_dt_spec spim = SPI_DT_SPEC_GET(DT_NODELABEL(dut_spi_dt), SPIM_OP);

/* Mock data for SPI loopback transfer. */
static struct sensor_data tx_data;

/* SPI transmit buffer for mock data. */
static struct spi_buf tx_buf  = {
	.buf = tx_data.data,
	.len = sizeof(tx_data.data),
};
static struct spi_buf_set tx_buffer_set  = {
	.buffers = &tx_buf,
	.count = 1,
};

/* Buffer for received data.
 * Will be set to the specific place in ipc_data[] array.
 */
static struct sensor_data_batch batch_data;

/* SPI receive buffer for loopback transfer data. */
static struct spi_buf rx_buf;
static struct spi_buf_set rx_buffer_set  = {
	.buffers = &rx_buf,
	.count = 1,
};

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
			printk("send_message failed with ret %d\n", ret);
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

	while (1) {
		/* Setup mock data for SPI loopback transfer. */
		tx_data.data[0] = generate_mock_sample();

		/* Setup buffer for received data. */
		rx_buf.buf = batch_data.data[spi_xfer_cnt].data;
		rx_buf.len = sizeof(batch_data.data[spi_xfer_cnt].data);
		memset(rx_buf.buf, 0, rx_buf.len);

#if defined(CONFIG_MULTITHREADING) && defined(CONFIG_PM)
		/* CONFIG_PM will automatically power up the SPI bus device. */
#else
		/* Initialize SPI bus to power up the bus device. */
		device_init(spim.bus);
#endif

		/* Perform SPI loopback transfer. */
		spi_transceive_dt(&spim, &tx_buffer_set, &rx_buffer_set);

#if defined(CONFIG_MULTITHREADING) && defined(CONFIG_PM)
		/* CONFIG_PM will automatically power down the SPI bus device. */
#else
		/* Deinitialize SPI bus to power down the bus device. */
		device_deinit(spim.bus);
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

	/* Wait for app core to start. */
	k_busy_wait(1000000);

	err = ipc_init();
	if (err) {
		printk("Failed to initialize IPC: %d\n", err);
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
