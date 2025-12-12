#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <string.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/drivers/spi.h>

/*
 * *** User input ***
 */
#define SPI_BUFFER_SIZE 35
#define SPI_READ_INTERVAL_MS 100
#define IPC_SEND_INTERVAL 10     // in number of SPI transactions


#define IPC_BUFFER_SIZE SPI_BUFFER_SIZE*IPC_SEND_INTERVAL //Can not exceed CONFIG_PBUF_RX_READ_BUF_SIZE


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
 * *** IPC settings ***
 *
 * IPC sends ipc_buffer[] dummy data. This is to show the current consumpton 
 * associated with different payloads.
 * SPI buffers can be expanded to contain all transactions,
 * and can be send directly over IPC instead of the ipc_buffer.
 */
const struct device *ipc1_instance;
struct ipc_ept ep;
static K_SEM_DEFINE(bound_sem, 0, 1);

static void ep_bound(void *priv) {
	k_sem_give(&bound_sem);
}
static void ep_recv(const void *data, size_t len, void *priv) { }

static struct ipc_ept_cfg ep_cfg = {
	.name = "ep0",
	.cb = {
		.bound    = ep_bound,
		.received = ep_recv,
	},
};

static uint8_t ipc_buffer[IPC_BUFFER_SIZE];
static void ipc_send_task(struct k_work *work){
	int ret;
	while (true) {
		ret = ipc_service_send(&ep, ipc_buffer, IPC_BUFFER_SIZE);
		if (ret == -ENOMEM) {
			/* No space in the buffer. Retry. */
			continue;
		} else if (ret < 0) {
			printk("send_message failed with ret %d\n", ret);
		}
		break;
	}
}
K_WORK_DEFINE(work_ipc_id, ipc_send_task);

/*
 * *** SPI read and IPC send thread ***
 */
static void spi_tranceive_task(void *arg1, void *arg2, void *arg3) {
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	int spi_read_counter = 0;
	while (1) {
		spi_read_dt(&spim, &tx_buffer_set);
		if(spi_read_counter%IPC_SEND_INTERVAL == IPC_SEND_INTERVAL-1){
			k_work_submit(&work_ipc_id);
		}
		spi_read_counter++;
		k_msleep(SPI_READ_INTERVAL_MS);
	}
}
K_THREAD_DEFINE(thread_spi_read, 1024, spi_tranceive_task, NULL, NULL, NULL,
		K_PRIO_COOP(1), 0, -1);

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

	k_sem_take(&bound_sem, K_FOREVER);

	// Start SPI read thread
	k_thread_start(thread_spi_read);
	return 0;
}
