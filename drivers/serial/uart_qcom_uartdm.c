/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Qualcomm UARTDM serial driver, transmit path only.
 *
 * Register layout follows the BLSP ("PERIPH_BLK_BLSP") variant used on
 * MSM8953, which is what lk2nd's platform/msm_shared/uart_dm.c uses and what
 * the downstream Linux msm_serial_hs driver describes: the status/command
 * registers sit at 0xA0..0xB0 and the 4-byte-wide TX FIFO starts at 0x100.
 *
 * The controller is left exactly as the bootloader programmed it. That is not
 * laziness: the UART's clock comes from GCC, which lk2nd has already set up,
 * and the line is already running at 115200 8N1 by the time we are entered.
 * We only push characters through the FIFO, using the same
 * "number of characters to transmit" register the bootloader uses, so the
 * padding bytes of a partial word are never sent.
 */

#define DT_DRV_COMPAT qcom_msm_uartdm

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/sys_io.h>

/* UARTDM register offsets (BLSP variant) */
#define UARTDM_CSR		0x0A0
#define UARTDM_SR		0x0A4
#define UARTDM_CR		0x0A8
#define UARTDM_IMR		0x0B0
#define UARTDM_TF		0x100
#define UARTDM_RF		0x140
#define UARTDM_NO_CHARS_FOR_TX	0x040
#define UARTDM_RX_TOTAL_SNAP	0x050

#define SR_RXRDY	BIT(0)
#define SR_TXRDY	BIT(2)
#define SR_TXEMT	BIT(3)

#define CR_TX_ENABLE	BIT(2)

struct uart_qcom_config {
	mm_reg_t base;
};

static inline uint32_t rd(mm_reg_t base, uint32_t off)
{
	return sys_read32(base + off);
}

static inline void wr(mm_reg_t base, uint32_t off, uint32_t val)
{
	sys_write32(val, base + off);
}

static void uart_qcom_poll_out(const struct device *dev, unsigned char c)
{
	const struct uart_qcom_config *cfg = dev->config;

	/* Wait for room in the TX FIFO. */
	while ((rd(cfg->base, UARTDM_SR) & SR_TXRDY) == 0U) {
		/* spin */
	}

	/*
	 * Tell the controller how many characters this word carries, then push
	 * the word. Writing the count first is what makes the hardware ignore
	 * the unused bytes of the 32-bit FIFO word.
	 */
	wr(cfg->base, UARTDM_NO_CHARS_FOR_TX, 1U);
	wr(cfg->base, UARTDM_TF, (uint32_t)c);
}

static int uart_qcom_poll_in(const struct device *dev, unsigned char *c)
{
	const struct uart_qcom_config *cfg = dev->config;

	if ((rd(cfg->base, UARTDM_SR) & SR_RXRDY) == 0U) {
		return -1;
	}
	*c = (unsigned char)(rd(cfg->base, UARTDM_RF) & 0xFFU);

	return 0;
}

static int uart_qcom_err_check(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN

static int uart_qcom_fifo_fill(const struct device *dev, const uint8_t *tx_data, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		uart_qcom_poll_out(dev, tx_data[i]);
	}

	return size;
}

static int uart_qcom_fifo_read(const struct device *dev, uint8_t *rx_data, const int size)
{
	int i;
	unsigned char c;

	for (i = 0; i < size; i++) {
		if (uart_qcom_poll_in(dev, &c) != 0) {
			break;
		}
		rx_data[i] = c;
	}

	return i;
}

static void uart_qcom_irq_tx_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static void uart_qcom_irq_tx_disable(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static int uart_qcom_irq_tx_ready(const struct device *dev)
{
	const struct uart_qcom_config *cfg = dev->config;

	return (rd(cfg->base, UARTDM_SR) & SR_TXRDY) != 0U;
}

static void uart_qcom_irq_rx_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static void uart_qcom_irq_rx_disable(const struct device *dev)
{
	ARG_UNUSED(dev);
}

static int uart_qcom_irq_rx_ready(const struct device *dev)
{
	const struct uart_qcom_config *cfg = dev->config;

	return (rd(cfg->base, UARTDM_SR) & SR_RXRDY) != 0U;
}

static int uart_qcom_irq_is_pending(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 0;
}

static int uart_qcom_irq_update(const struct device *dev)
{
	ARG_UNUSED(dev);

	return 1;
}

static void uart_qcom_irq_callback_set(const struct device *dev, uart_irq_callback_user_data_t cb,
				       void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(user_data);
}

#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static DEVICE_API(uart, uart_qcom_api) = {
	.poll_in = uart_qcom_poll_in,
	.poll_out = uart_qcom_poll_out,
	.err_check = uart_qcom_err_check,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = uart_qcom_fifo_fill,
	.fifo_read = uart_qcom_fifo_read,
	.irq_tx_enable = uart_qcom_irq_tx_enable,
	.irq_tx_disable = uart_qcom_irq_tx_disable,
	.irq_tx_ready = uart_qcom_irq_tx_ready,
	.irq_rx_enable = uart_qcom_irq_rx_enable,
	.irq_rx_disable = uart_qcom_irq_rx_disable,
	.irq_rx_ready = uart_qcom_irq_rx_ready,
	.irq_err_enable = uart_qcom_irq_rx_enable,
	.irq_err_disable = uart_qcom_irq_rx_disable,
	.irq_is_pending = uart_qcom_irq_is_pending,
	.irq_update = uart_qcom_irq_update,
	.irq_callback_set = uart_qcom_irq_callback_set,
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */
};

static int uart_qcom_init(const struct device *dev)
{
	const struct uart_qcom_config *cfg = dev->config;

	/* Make sure the transmitter is enabled; harmless if it already is. */
	wr(cfg->base, UARTDM_CR, CR_TX_ENABLE);

	return 0;
}

#define UART_QCOM_DEVICE(n)                                                                        \
	static const struct uart_qcom_config uart_qcom_cfg_##n = {                                 \
		.base = DT_INST_REG_ADDR(n),                                                       \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(n, uart_qcom_init, NULL, NULL, &uart_qcom_cfg_##n, PRE_KERNEL_1,     \
			      CONFIG_SERIAL_INIT_PRIORITY, &uart_qcom_api);

DT_INST_FOREACH_STATUS_OKAY(UART_QCOM_DEVICE)
