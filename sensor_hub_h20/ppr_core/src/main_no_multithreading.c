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
#define SPI_READ_INTERVAL_MS 350
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

static uint8_t ipc_buffer[IPC_BUFFER_SIZE];
static uint8_t idx = 0;

static void ipc_send(void)
{
	int ret;
	while (true) {
		ipc_buffer[0] = idx;
		idx = (idx + 1) % 256;
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

// TIMER TO ENSURE PROPER DELAY
static volatile bool idle_timer_expired = false;
static void idle_timer_handler(struct k_timer *timer)
{
	idle_timer_expired = true;
}
K_TIMER_DEFINE(idle_timer, idle_timer_handler, NULL);


#include <hal/nrf_vpr_csr.h>

// from: /ncs/nrf/tests/benchmarks/multicore/common/power_off.c
static void pm_enter_hibernate(void)
{
	csr_write(VPRCSR_NORDIC_VPRNORDICSLEEPCTRL,
		  VPRCSR_NORDIC_VPRNORDICSLEEPCTRL_SLEEPSTATE_HIBERNATE);
	nrf_barrier_w();
	arch_cpu_idle();
}

// from: /ncs/nrf/tests/benchmarks/multicore/common/power_off.c
static void pm_exit_hibernate(void)
{
	csr_write(VPRCSR_NORDIC_VPRNORDICSLEEPCTRL,
		  VPRCSR_NORDIC_VPRNORDICSLEEPCTRL_SLEEPSTATE_WAIT);

	/* unlock interrupts after sleep */
	irq_unlock(MSTATUS_IEN);
}

// DOES NOT HANDLE DEVICES IN ANY CAPACITY, JUST VPR CORE
static void idle_custom_pm(void)
{
	pm_enter_hibernate();
	/* Should sleep here */
	pm_exit_hibernate();
}

// REQUIRES modifications to pm.c/device_runtime.c to remove all thread-related code
static void idle_hacked_pm(void)
{
	(void) arch_irq_lock();
	_kernel.idle = z_get_next_timeout_expiry();
	if (k_is_pre_kernel() || !pm_system_suspend(_kernel.idle)) {
		k_cpu_idle();
	}
}

static void idle(void)
{
	if (IS_ENABLED(CONFIG_APP_MANUAL_VPR_PM_CUSTOM)) {
		idle_custom_pm();
	} else if (IS_ENABLED(CONFIG_APP_MANUAL_VPR_PM_HACKED_PM)) {
		idle_hacked_pm();
	}
}

static void wait_in_idle_ms(uint32_t sleep_ms)
{
	idle_timer_expired = false;
	k_timer_start(&idle_timer, K_MSEC(sleep_ms), K_NO_WAIT);

	while(!idle_timer_expired) {
		idle();
	}
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

	// SPI read and IPC send loop (single-threaded)
	int spi_read_counter = 0;
	while (1) {
		spi_read_dt(&spim, &tx_buffer_set);
		if (spi_read_counter % IPC_SEND_INTERVAL == IPC_SEND_INTERVAL - 1) {
			ipc_send();
		}
		spi_read_counter++;

		if ((IS_ENABLED(CONFIG_APP_MANUAL_VPR_PM_CUSTOM) ||
		     IS_ENABLED(CONFIG_APP_MANUAL_VPR_PM_HACKED_PM))) {
			wait_in_idle_ms(SPI_READ_INTERVAL_MS);
		} else {
			k_msleep(SPI_READ_INTERVAL_MS);
		}
	}

	return 0;
}
