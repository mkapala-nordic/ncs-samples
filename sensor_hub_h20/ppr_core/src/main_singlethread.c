#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/drivers/spi.h>

#include <zephyr/pm/pm.h>

/*
 * *** User input ***
 */
#define SPI_BUFFER_SIZE 35
#define SPI_READ_INTERVAL_MS 100
#define IPC_SEND_INTERVAL 10     // in number of SPI transactions

/** Represents a SPI transaction. */
struct spi_data {
	uint8_t data[SPI_BUFFER_SIZE];
};

/*
 * *** SPI settings ***
 */
#define SPI_MODE (SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_TRANSFER_LSB)
#define SPIM_OP	 (SPI_OP_MODE_MASTER | SPI_MODE)
struct spi_dt_spec spim = SPI_DT_SPEC_GET(DT_NODELABEL(dut_spi_dt), SPIM_OP, 0);

/* Mock data for SPI loopback transfer. */
static struct spi_data tx_data;
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
static struct spi_data batch_data[IPC_SEND_INTERVAL];
static struct spi_buf rx_buf;
static struct spi_buf_set rx_buffer_set  = {
	.buffers = &rx_buf,
	.count = 1,
};

/*
 * *** IPC settings ***
 *
 * IPC sends ipc_buffer[] dummy data. This is to show the current consumpton
 * associated with different payloads.
 * SPI buffers can be expanded to contain all transactions,
 * and can be send directly over IPC instead of the ipc_buffer.
 */
const struct device *ipc1_instance;
struct ipc_ept ep;
static bool ep_bound_flag = false;

static void ep_bound(void *priv) {
	ep_bound_flag = true;
}
static void ep_recv(const void *data, size_t len, void *priv) { }

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
		ret = ipc_service_send(&ep, batch_data, sizeof(batch_data));
		if (ret == -ENOMEM) {
			/* No space in the buffer. Retry. */
			continue;
		} else if (ret < 0) {
			printk("send_message failed with ret %d\n", ret);
		}
		break;
	}
}

#include <hal/nrf_vpr_csr.h>

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

static char next_alpha_char(void)
{
	static char val = 'Z';

	val = (val == 'Z') ? 'A' : val + 1;

	return val;
}

int main(void)
{
	// Wait for app core to start
	k_busy_wait(1000000);

	// Initialize IPC
	int ret;
	ipc1_instance = DEVICE_DT_GET(DT_NODELABEL(ipc1));

	ret = ipc_service_open_instance(ipc1_instance);
	if ((ret < 0) && (ret != -EALREADY)) {
		return 0;
	}

	ret = ipc_service_register_endpoint(ipc1_instance, &ep, &ep_cfg);
	if (ret < 0) {
		return 0;
	}

	while (!ep_bound_flag) {
		k_msleep(10);
	}


	int spi_xfer_cnt = 0;

	/* SPI read and IPC send loop (single-threaded). */
	while (1) {
		/* Setup mock data for SPI loopback transfer. */
		tx_data.data[0] = next_alpha_char();

		/* Setup buffer for received data. */
		rx_buf.buf = batch_data[spi_xfer_cnt].data;
		rx_buf.len = sizeof(batch_data[spi_xfer_cnt].data);
		memset(rx_buf.buf, 0, rx_buf.len);

		/* Initialize SPI bus to power up the bus device. */
		device_init(spim.bus);

		/* Perform SPI loopback transfer. */
		spi_transceive_dt(&spim, &tx_buffer_set, &rx_buffer_set);

		/* Deinitialize SPI bus to power down the bus device. */
		device_deinit(spim.bus);

		/* Send IPC data if we have reached the required number of samples. */
		if (spi_xfer_cnt == (IPC_SEND_INTERVAL - 1)) {
			ipc_send();
			spi_xfer_cnt = 0;
		} else {
			spi_xfer_cnt++;
		}

		sleep(K_MSEC(SPI_READ_INTERVAL_MS));
	}

	return 0;
}
