/* drivers/serial/msm_serial_hs.c
 *
 * MSM 7k/8k High speed uart driver
 *
 * Copyright (c) 2007-2008 QUALCOMM Incorporated.
 * Copyright (c) 2008 QUALCOMM USA, INC.
 * Copyright (c) 2008 Google Inc.
 * Modified: Nick Pelly <npelly@google.com>
 *
 * All source code in this file is licensed under the following license
 * except where indicated.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org
 */

/*
 * MSM 7k/8k High speed uart driver
 *
 * Has optional support for uart power management independent of linux
 * suspend/resume:
 *
 * RX wakeup.
 * UART wakeup can be triggered by RX activity (using a wakeup GPIO on the
 * UART RX pin). This should only be used if there is not a wakeup
 * GPIO on the UART CTS, and the first RX byte is known (for example, with the
 * Bluetooth Texas Instruments HCILL protocol), since the first RX byte will
 * always be lost. RTS will be asserted even while the UART is off in this mode
 * of operation. See msm_serial_hs_platform_data.rx_wakeup_irq.
 */

#include <linux/module.h>

#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <linux/wait.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

#include <asm/atomic.h>
#include <linux/irq.h>
#include <asm/system.h>

#include <mach/hardware.h>
#include <mach/dma.h>
#include <mach/msm_serial_hs.h>

#include "msm_serial_hs_hwreg.h"

#ifdef CONFIG_LOCOSTO_UART2DM_MUX
#include <linux/mux_0710.h>
#endif
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
extern u8 radio_state;
#include <linux/gpio.h>
#include <asm/mach-types.h>

#include "../../arch/arm/mach-msm/gpio_chip.h"
#include "../../arch/arm/mach-msm/board-oboea.h"
#endif

#define MODULE_NAME "[GSM_RADIO]" /* HTC version */

enum flush_reason {
	FLUSH_NONE,
	FLUSH_DATA_READY,
	FLUSH_DATA_INVALID,  /* values after this indicate invalid data */
	FLUSH_IGNORE = FLUSH_DATA_INVALID,
	FLUSH_STOP,
	FLUSH_SHUTDOWN,
};

enum msm_hs_clk_states_e {
	MSM_HS_CLK_PORT_OFF,     /* port not in use */
	MSM_HS_CLK_OFF,          /* clock disabled */
	MSM_HS_CLK_REQUEST_OFF,  /* disable after TX and RX flushed */
	MSM_HS_CLK_ON,           /* clock enabled */
};

/* Track the forced RXSTALE flush during clock off sequence.
 * These states are only valid during MSM_HS_CLK_REQUEST_OFF */
enum msm_hs_clk_req_off_state_e {
	CLK_REQ_OFF_START,
	CLK_REQ_OFF_RXSTALE_ISSUED,
	CLK_REQ_OFF_FLUSH_ISSUED,
	CLK_REQ_OFF_RXSTALE_FLUSHED,
};

struct msm_hs_tx {
	unsigned int tx_ready_int_en;  /* ok to dma more tx */
	unsigned int dma_in_flight;    /* tx dma in progress */
	struct msm_dmov_cmd xfer;
	dmov_box *command_ptr;
	u32 *command_ptr_ptr;
	dma_addr_t mapped_cmd_ptr;
	dma_addr_t mapped_cmd_ptr_ptr;
	int tx_count;
	dma_addr_t dma_base;
};

struct msm_hs_rx {
	enum flush_reason flush;
	struct msm_dmov_cmd xfer;
	dma_addr_t cmdptr_dmaaddr;
	dmov_box *command_ptr;
	u32 *command_ptr_ptr;
	dma_addr_t mapped_cmd_ptr;
	wait_queue_head_t wait;
	dma_addr_t rbuffer;
	unsigned char *buffer;
	struct dma_pool *pool;
	struct wake_lock wake_lock;
	struct work_struct tty_work;
};

/* optional RX GPIO IRQ low power wakeup */
struct msm_hs_rx_wakeup {
	int irq;  /* < 0 indicates low power wakeup disabled */
	unsigned char ignore;  /* bool */

	/* bool: inject char into rx tty on wakeup */
	unsigned char inject_rx;
	char rx_to_inject;
};

struct msm_hs_port {
	struct uart_port uport;
	unsigned long imr_reg;  /* shadow value of UARTDM_IMR */
	struct clk *clk;
	struct msm_hs_tx tx;
	struct msm_hs_rx rx;

	int dma_tx_channel;
	int dma_rx_channel;
	int dma_tx_crci;
	int dma_rx_crci;

	struct hrtimer clk_off_timer;  /* to poll TXEMT before clock off */
	ktime_t clk_off_delay;
	enum msm_hs_clk_states_e clk_state;
	enum msm_hs_clk_req_off_state_e clk_req_off_state;

	struct msm_hs_rx_wakeup rx_wakeup;
	/* optional callback to exit low power mode */
	void (*exit_lpm_cb)(struct uart_port *);

	struct wake_lock dma_wake_lock;  /* held while any DMA active */
};

#define MSM_UARTDM_BURST_SIZE 16   /* DM burst size (in bytes) */
#define UARTDM_TX_BUF_SIZE UART_XMIT_SIZE
#define UARTDM_RX_BUF_SIZE 512

#define UARTDM_NR 2

static struct msm_hs_port q_uart_port[UARTDM_NR];
static struct platform_driver msm_serial_hs_platform_driver;
static struct uart_driver msm_hs_driver;
static struct uart_ops msm_hs_ops;
static struct workqueue_struct *msm_hs_workqueue;

#define UARTDM_TO_MSM(uart_port) \
	container_of((uart_port), struct msm_hs_port, uport)

static
inline unsigned int use_low_power_rx_wakeup(struct msm_hs_port *msm_uport)
{
	return (msm_uport->rx_wakeup.irq >= 0);
}

static inline unsigned int msm_hs_read(struct uart_port *uport,
				       unsigned int offset)
{
	return ioread32(uport->membase + offset);
}

static inline void msm_hs_write(struct uart_port *uport, unsigned int offset,
				 unsigned int value)
{
	iowrite32(value, uport->membase + offset);
}

static void msm_hs_release_port(struct uart_port *port)
{
}

static int msm_hs_request_port(struct uart_port *port)
{
	return 0;
}

static int __devexit msm_hs_remove(struct platform_device *pdev)
{

	struct msm_hs_port *msm_uport;
	struct device *dev;

	if (pdev->id < 0 || pdev->id >= UARTDM_NR) {
		printk(KERN_ERR MODULE_NAME "Invalid plaform device ID = %d\n", pdev->id);
		return -EINVAL;
	}

	msm_uport = &q_uart_port[pdev->id];
	dev = msm_uport->uport.dev;

	dma_unmap_single(dev, msm_uport->rx.mapped_cmd_ptr, sizeof(dmov_box),
			 DMA_TO_DEVICE);
	dma_pool_free(msm_uport->rx.pool, msm_uport->rx.buffer,
		      msm_uport->rx.rbuffer);
	dma_pool_destroy(msm_uport->rx.pool);

	dma_unmap_single(dev, msm_uport->rx.cmdptr_dmaaddr, sizeof(u32 *),
			 DMA_TO_DEVICE);
	dma_unmap_single(dev, msm_uport->tx.mapped_cmd_ptr_ptr, sizeof(u32 *),
			 DMA_TO_DEVICE);
	dma_unmap_single(dev, msm_uport->tx.mapped_cmd_ptr, sizeof(dmov_box),
			 DMA_TO_DEVICE);

	wake_lock_destroy(&msm_uport->rx.wake_lock);
	wake_lock_destroy(&msm_uport->dma_wake_lock);

	uart_remove_one_port(&msm_hs_driver, &msm_uport->uport);
	clk_put(msm_uport->clk);

	/* Free the tx resources */
	kfree(msm_uport->tx.command_ptr);
	kfree(msm_uport->tx.command_ptr_ptr);

	/* Free the rx resources */
	kfree(msm_uport->rx.command_ptr);
	kfree(msm_uport->rx.command_ptr_ptr);

	iounmap(msm_uport->uport.membase);

	return 0;
}

static int msm_hs_init_clk_locked(struct uart_port *uport)
{
	int ret;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	wake_lock(&msm_uport->dma_wake_lock);
	ret = clk_enable(msm_uport->clk);
	if (ret) {
		printk(KERN_ERR MODULE_NAME "Error could not turn on UART clk\n");
		return ret;
	}

#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	gpio_configure(OBOEA_GPIO_GSM_PDA_STATUS, GPIOF_DRIVE_OUTPUT | GPIOF_OUTPUT_LOW);
	printk(KERN_INFO MODULE_NAME "%s set GPIO_PDA_STATUS low\n",__FUNCTION__);
#endif

	/* Set up the MREG/NREG/DREG/MNDREG */
	ret = clk_set_rate(msm_uport->clk, uport->uartclk);
	if (ret) {
		printk(KERN_WARNING MODULE_NAME "Error setting clock rate on UART\n");
		return ret;
	}

	msm_uport->clk_state = MSM_HS_CLK_ON;
	return 0;
}

/* Enable and Disable clocks  (Used for power management) */
static void msm_hs_pm(struct uart_port *uport, unsigned int state,
		      unsigned int oldstate)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	if (use_low_power_rx_wakeup(msm_uport) || msm_uport->exit_lpm_cb)
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	{
		printk(KERN_INFO MODULE_NAME "%s ignore linux PM, use msm_hs_request_clock API\n",__FUNCTION__);
		return;  /* ignore linux PM states, use msm_hs_request_clock API */
	}
#else
	    return;  /* ignore linux PM states, use msm_hs_request_clock API */
#endif

	switch (state) {
	case 0:
		clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		gpio_configure(OBOEA_GPIO_GSM_PDA_STATUS, GPIOF_DRIVE_OUTPUT | GPIOF_OUTPUT_LOW);
		printk(KERN_INFO MODULE_NAME "%s set GPIO_PDA_STATUS low\n",__FUNCTION__);
#endif
		break;
	case 3:
		clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		gpio_configure(OBOEA_GPIO_GSM_PDA_STATUS, GPIOF_DRIVE_OUTPUT | GPIOF_OUTPUT_HIGH);
		printk(KERN_INFO MODULE_NAME "%s set GPIO_PDA_STATUS high\n",__FUNCTION__);
#endif
		break;
	default:
		printk(KERN_ERR MODULE_NAME "msm_serial: Unknown PM state %d\n", state);
	}
}

/*
 * programs the UARTDM_CSR register with correct bit rates
 *
 * Interrupts should be disabled before we are called, as
 * we modify Set Baud rate
 * Set receive stale interrupt level, dependant on Bit Rate
 * Goal is to have around 8 ms before indicate stale.
 * roundup (((Bit Rate * .008) / 10) + 1
 */
static void msm_hs_set_bps_locked(struct uart_port *uport,
			       unsigned int bps)
{
	unsigned long rxstale;
	unsigned long data;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	printk(KERN_INFO MODULE_NAME "%s bps=%d\n", __FUNCTION__, bps);

	switch (bps) {
	case 300:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x00);
		rxstale = 1;
		break;
	case 600:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x11);
		rxstale = 1;
		break;
	case 1200:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x22);
		rxstale = 1;
		break;
	case 2400:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x33);
		rxstale = 1;
		break;
	case 4800:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x44);
		rxstale = 1;
		break;
	case 9600:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x55);
		rxstale = 2;
		break;
	case 14400:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x66);
		rxstale = 3;
		break;
	case 19200:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x77);
		rxstale = 4;
		break;
	case 28800:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x88);
		rxstale = 6;
		break;
	case 38400:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0x99);
		rxstale = 8;
		break;
	case 57600:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xaa);
		rxstale = 16;
		break;
	case 76800:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xbb);
		rxstale = 16;
		break;
	case 115200:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xcc);
		rxstale = 31;
		break;
	case 230400:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xee);
		rxstale = 31;
		break;
	case 460800:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xff);
		rxstale = 31;
		break;
	case 4000000:
	case 3686400:
	case 3200000:
	case 3500000:
	case 3000000:
	case 2500000:
	case 1500000:
	case 1152000:
	case 1000000:
	case 921600:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xff);
		rxstale = 31;
		break;
	default:
		msm_hs_write(uport, UARTDM_CSR_ADDR, 0xff);
		/* default to 9600 */
		bps = 9600;
		rxstale = 2;
		break;
	}
	/*
	 * uart baud rate depends on CSR and MND Values
	 * we are updating CSR before and then calling
	 * clk_set_rate which updates MND Values. Hence
	 * dsb requires here.
	 */
	dsb();
	if (bps > 460800)
		uport->uartclk = bps * 16;
	else
		uport->uartclk = 7372800;

	if (clk_set_rate(msm_uport->clk, uport->uartclk)) {
		printk(KERN_WARNING MODULE_NAME "Error setting clock rate on UART\n");
		return;
	}

	data = rxstale & UARTDM_IPR_STALE_LSB_BMSK;
	data |= UARTDM_IPR_STALE_TIMEOUT_MSB_BMSK & (rxstale << 2);

	msm_hs_write(uport, UARTDM_IPR_ADDR, data);
}

/*
 * termios :  new ktermios
 * oldtermios:  old ktermios previous setting
 *
 * Configure the serial port
 */
static void msm_hs_set_termios(struct uart_port *uport,
				   struct ktermios *termios,
				   struct ktermios *oldtermios)
{
	unsigned int bps;
	unsigned long data;
	unsigned long flags;
	unsigned int c_cflag = termios->c_cflag;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	spin_lock_irqsave(&uport->lock, flags);
	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	/* 300 is the minimum baud support by the driver  */
	bps = uart_get_baud_rate(uport, termios, oldtermios, 200, 4000000);

	/* Temporary remapping  200 BAUD to 3.2 mbps */
	if (bps == 200)
		bps = 3200000;

	msm_hs_set_bps_locked(uport, bps);

	data = msm_hs_read(uport, UARTDM_MR2_ADDR);
	data &= ~UARTDM_MR2_PARITY_MODE_BMSK;
	/* set parity */
	if (PARENB == (c_cflag & PARENB)) {
		if (PARODD == (c_cflag & PARODD))
			data |= ODD_PARITY;
		else if (CMSPAR == (c_cflag & CMSPAR))
			data |= SPACE_PARITY;
		else
			data |= EVEN_PARITY;
	}

	/* Set bits per char */
	data &= ~UARTDM_MR2_BITS_PER_CHAR_BMSK;

	switch (c_cflag & CSIZE) {
	case CS5:
		data |= FIVE_BPC;
		break;
	case CS6:
		data |= SIX_BPC;
		break;
	case CS7:
		data |= SEVEN_BPC;
		break;
	default:
		data |= EIGHT_BPC;
		break;
	}
	/* stop bits */
	if (c_cflag & CSTOPB) {
		data |= STOP_BIT_TWO;
	} else {
		/* otherwise 1 stop bit */
		data |= STOP_BIT_ONE;
	}
	data |= UARTDM_MR2_ERROR_MODE_BMSK;
	/* write parity/bits per char/stop bit configuration */
	msm_hs_write(uport, UARTDM_MR2_ADDR, data);

	/* Configure HW flow control */
	data = msm_hs_read(uport, UARTDM_MR1_ADDR);

	data &= ~(UARTDM_MR1_CTS_CTL_BMSK | UARTDM_MR1_RX_RDY_CTL_BMSK);

	if (c_cflag & CRTSCTS) {
		data |= UARTDM_MR1_CTS_CTL_BMSK;
		data |= UARTDM_MR1_RX_RDY_CTL_BMSK;
	}

	msm_hs_write(uport, UARTDM_MR1_ADDR, data);

	uport->ignore_status_mask = termios->c_iflag & INPCK;
	uport->ignore_status_mask |= termios->c_iflag & IGNPAR;
	uport->read_status_mask = (termios->c_cflag & CREAD);

	msm_hs_write(uport, UARTDM_IMR_ADDR, 0);

	/* Set Transmit software time out */
	uart_update_timeout(uport, c_cflag, bps);

	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_RX);
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_TX);

	if (msm_uport->rx.flush == FLUSH_NONE) {
		wake_lock(&msm_uport->rx.wake_lock);
		msm_uport->rx.flush = FLUSH_IGNORE;
		/*
		 * Before using dmov APIs make sure that
		 * previous writel are completed. Hence
		 * dsb requires here.
		 */
		dsb();
		msm_dmov_flush(msm_uport->dma_rx_channel);
	}

	msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);

	/* calling other hardware component here clk_disable API. */
	dsb();
	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
	spin_unlock_irqrestore(&uport->lock, flags);
}

/*
 *  Standard API, Transmitter
 *  Any character in the transmit shift register is sent
 */
static unsigned int msm_hs_tx_empty(struct uart_port *uport)
{
	unsigned int data;
	unsigned int ret = 0;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	data = msm_hs_read(uport, UARTDM_SR_ADDR);
	if (data & UARTDM_SR_TXEMT_BMSK)
		ret = TIOCSER_TEMT;

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif

	return ret;
}

/*
 *  Standard API, Stop transmitter.
 *  Any character in the transmit shift register is sent as
 *  well as the current data mover transfer .
 */
static void msm_hs_stop_tx_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	msm_uport->tx.tx_ready_int_en = 0;
}

/*
 *  Standard API, Stop receiver as soon as possible.
 *
 *  Function immediately terminates the operation of the
 *  channel receiver and any incoming characters are lost. None
 *  of the receiver status bits are affected by this command and
 *  characters that are already in the receive FIFO there.
 */
static void msm_hs_stop_rx_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);
	unsigned int data;

	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	/* disable dlink */
	data = msm_hs_read(uport, UARTDM_DMEN_ADDR);
	data &= ~UARTDM_RX_DM_EN_BMSK;
	msm_hs_write(uport, UARTDM_DMEN_ADDR, data);

	/* calling DMOV or CLOCK API. Hence dsb() */
	dsb();
	/* Disable the receiver */
	if (msm_uport->rx.flush == FLUSH_NONE) {
		wake_lock(&msm_uport->rx.wake_lock);
		msm_dmov_flush(msm_uport->dma_rx_channel);
	}
	if (msm_uport->rx.flush != FLUSH_SHUTDOWN)
		msm_uport->rx.flush = FLUSH_STOP;

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
}

/*  Transmit the next chunk of data */
static void msm_hs_submit_tx_locked(struct uart_port *uport)
{
	int left;
	int tx_count;
	dma_addr_t src_addr;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);
	struct msm_hs_tx *tx = &msm_uport->tx;
	struct circ_buf *tx_buf = &msm_uport->uport.state->xmit;

	int idx = 0;

	printk(KERN_INFO MODULE_NAME "%s +\n", __FUNCTION__);

	if (uart_circ_empty(tx_buf) || uport->state->port.tty->stopped) {
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		/*	Release PDA_INT_BB when TX_FIFO and THR_REG , xmit buffer are all empty */
		gpio_configure(OBOEA_GPIO_GSM_PDA_INT_BB, GPIOF_DRIVE_OUTPUT | GPIOF_OUTPUT_HIGH);
		printk(KERN_INFO MODULE_NAME "%s set GPIO_PDA_INT_BB high-\n",__FUNCTION__);
#endif
		msm_hs_stop_tx_locked(uport);
		return;
	}

	tx->dma_in_flight = 1;

	tx_count = uart_circ_chars_pending(tx_buf);

	if (UARTDM_TX_BUF_SIZE < tx_count)
		tx_count = UARTDM_TX_BUF_SIZE;

	left = UART_XMIT_SIZE - tx_buf->tail;

	if (tx_count > left)
		tx_count = left;

	printk("[GSM_RADIO] ");
	for (idx=0 ; idx<tx_count; idx++)
		printk("%x ", tx_buf->buf[idx]);
	printk("\n");

	src_addr = tx->dma_base + tx_buf->tail;
	dma_sync_single_for_device(uport->dev, src_addr, tx_count,
				   DMA_TO_DEVICE);

	tx->command_ptr->num_rows = (((tx_count + 15) >> 4) << 16) |
				     ((tx_count + 15) >> 4);
	tx->command_ptr->src_row_addr = src_addr;

	dma_sync_single_for_device(uport->dev, tx->mapped_cmd_ptr,
				   sizeof(dmov_box), DMA_TO_DEVICE);

	*tx->command_ptr_ptr = CMD_PTR_LP | DMOV_CMD_ADDR(tx->mapped_cmd_ptr);

	dma_sync_single_for_device(uport->dev, tx->mapped_cmd_ptr_ptr,
				   sizeof(u32 *), DMA_TO_DEVICE);

	/* Save tx_count to use in Callback */
	tx->tx_count = tx_count;
	msm_hs_write(uport, UARTDM_NCF_TX_ADDR, tx_count);

	/* Disable the tx_ready interrupt */
	msm_uport->imr_reg &= ~UARTDM_ISR_TX_READY_BMSK;
	msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
	/* Calling next DMOV API. Hence dsb() here. */
	dsb();

	msm_dmov_enqueue_cmd(msm_uport->dma_tx_channel, &tx->xfer);

	printk(KERN_INFO MODULE_NAME "%s -\n", __FUNCTION__);

}

/* Start to receive the next chunk of data */
static void msm_hs_start_rx_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	printk(KERN_INFO MODULE_NAME "%s +\n", __FUNCTION__);

	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_STALE_INT);
	msm_hs_write(uport, UARTDM_DMRX_ADDR, UARTDM_RX_BUF_SIZE);
	msm_hs_write(uport, UARTDM_CR_ADDR, STALE_EVENT_ENABLE);
	msm_uport->imr_reg |= UARTDM_ISR_RXLEV_BMSK;
	msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
	/* Calling next DMOV API. Hence dsb() here. */
	dsb();

	msm_uport->rx.flush = FLUSH_NONE;
	msm_dmov_enqueue_cmd(msm_uport->dma_rx_channel, &msm_uport->rx.xfer);

	/* might have finished RX and be ready to clock off */
	hrtimer_start(&msm_uport->clk_off_timer, msm_uport->clk_off_delay,
			HRTIMER_MODE_REL);

	printk(KERN_INFO MODULE_NAME "%s -\n", __FUNCTION__);

}

/* Enable the transmitter Interrupt */
static void msm_hs_start_tx_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	if (msm_uport->exit_lpm_cb)
		msm_uport->exit_lpm_cb(uport);

	if (msm_uport->tx.tx_ready_int_en == 0) {
		msm_uport->tx.tx_ready_int_en = 1;
		msm_hs_submit_tx_locked(uport);
	}

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
}

/*
 *  This routine is called when we are done with a DMA transfer
 *
 *  This routine is registered with Data mover when we set
 *  up a Data Mover transfer. It is called from Data mover ISR
 *  when the DMA transfer is done.
 */
static void msm_hs_dmov_tx_callback(struct msm_dmov_cmd *cmd_ptr,
					unsigned int result,
					struct msm_dmov_errdata *err)
{
	unsigned long flags;
	struct msm_hs_port *msm_uport;

	WARN_ON(result != 0x80000002);  /* DMA did not finish properly */
	msm_uport = container_of(cmd_ptr, struct msm_hs_port, tx.xfer);

	spin_lock_irqsave(&msm_uport->uport.lock, flags);
	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	msm_uport->imr_reg |= UARTDM_ISR_TX_READY_BMSK;
	msm_hs_write(&msm_uport->uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
	/* Calling clk API. Hence dsb() requires. */
	dsb();

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
	spin_unlock_irqrestore(&msm_uport->uport.lock, flags);
}

#ifdef CONFIG_LOCOSTO_UART2DM_MUX
// irene@06132011++ Mux Decode

#define FALSE 0
#define TRUE 1
#define MUX_MAX_DECODE_BUF_LEN 4096


MUX_DATA_TYPE gMuxsrcType = MUX_TYPE_NONE;

static int MuxParseData(struct tty_struct *tty, unsigned char *pRxBuffer, int rx_count)
{

	static int PayloadIdx = 0;
	static MUX_STATE_TYPE MuxNextState = MUX_STATE_CHECK_START;
	static int ShouldCheckFcs = FALSE;
	static int DleDetected = FALSE;
	//static int PayloadIdxTrace = 0;
	static char LenHighByte = 0;
	static int PayloadLen = 0;
	static unsigned char ucFCS=0xFF;
	static unsigned char MuxDecodeRxBuffer[MUX_MAX_DECODE_BUF_LEN];

	int index = 0;
	// remove++
	int idx = 0;
	// remove--

/*
	int dwRenewPktState = 0;
	int dwRenewPktPos = 0;
*/	printk(KERN_INFO "MuxParseData\n\r rx_count %d",rx_count);



	if(!pRxBuffer || !rx_count)
	{
		printk(KERN_INFO "MuxParseData, bad  pointer(0x%p) or length(%d)\r\n", pRxBuffer, rx_count);
		goto Error;
	}
	else
	{
		while(index<rx_count)
		{

			if(MUX_BYTE_DLE == pRxBuffer[index])
			{
				if(!DleDetected)
				{
					DleDetected = TRUE;
					index++;
					continue;
				}
			}
			if(MUX_BYTE_START == pRxBuffer[index] && !DleDetected)
			{
				PayloadIdx = 0;
				MuxNextState = MUX_STATE_CHECK_CH_ID;
				index++;
				continue;
			}

			// workaround to extract the packet if the last one is invalid and ended by MUX_BYTE_DLE
			/*
			if(MUX_BYTE_START == pRxBuffer[index] && 0 == dwRenewPktState)
			{
				dwRenewPktState = 1;
				dwRenewPktPos = index;
			}
			else if(1 == dwRenewPktState && MUX_BYTE_ATCMD == pRxBuffer[index] && index == dwRenewPktPos+1)
			{
				dwRenewPktState = 2;
			}
			else if(2 == dwRenewPktState && 0x80 == pRxBuffer[index] && index == dwRenewPktPos+2)
			{
				dwRenewPktState = 3;
			}
			else if(3 != dwRenewPktState)
			{
				dwRenewPktState = 0;
			}
			*/
			switch(MuxNextState)
			{
			case MUX_STATE_CHECK_START:
				printk(KERN_INFO "MUX_STATE_CHECK_START\n\r");
				if(MUX_BYTE_START == pRxBuffer[index++] && !DleDetected)
				{
					MuxNextState = MUX_STATE_CHECK_CH_ID;
				}
				DleDetected = FALSE;
				break;

			case MUX_STATE_CHECK_CH_ID:
				printk(KERN_INFO "MUX_STATE_CHECK_CH_ID\n\r");
				memset(MuxDecodeRxBuffer, 0, MUX_MAX_DECODE_BUF_LEN);
				PayloadIdx = 0;
				PayloadLen = 0;
				ucFCS = 0xFF;
				//PayloadIdxTrace = 0;

				if(MUX_BYTE_ATCMD == pRxBuffer[index])
				{
					MuxNextState = MUX_STATE_CHECK_LEN_HIGH_BYTE;
					gMuxsrcType = MUX_TYPE_ATCMD;
				}
				else if(MUX_BYTE_DATA == pRxBuffer[index])
				{
					MuxNextState = MUX_STATE_CHECK_LEN_HIGH_BYTE;
					gMuxsrcType = MUX_TYPE_DATA;
				}
				/*
				else if(MUX_BYTE_TRACE_MIN <= pRxBuffer[index] && MUX_BYTE_TRACE_MAX >= pRxBuffer[index])
				{
					MuxNextState = MUX_STATE_CHECK_LEN_HIGH_BYTE;
					gMuxsrcType = MUX_TYPE_TRACE;
					MuxDecodeRxBuffer[PayloadIdx++]=MUX_BYTE_START;
					MuxDecodeRxBuffer[PayloadIdx++]=pRxBuffer[index];
				}
				*/
				else
				{
					MuxNextState = MUX_STATE_CHECK_START;
					PayloadIdx = 0;
				}
				index++;
				break;

			case MUX_STATE_CHECK_LEN_HIGH_BYTE:
				printk(KERN_INFO "MUX_STATE_CHECK_LEN_HIGH_BYTE\n\r");
				LenHighByte = pRxBuffer[index] & 0x0f;
				ShouldCheckFcs = (pRxBuffer[index++] & 0x0080) ? TRUE : FALSE;
				MuxNextState = MUX_STATE_CHECK_LEN_LOW_BYTE;
				DleDetected = FALSE;
				break;

			case MUX_STATE_CHECK_LEN_LOW_BYTE:
				printk(KERN_INFO "MUX_STATE_CHECK_LEN_LOW_BYTE\n\r");
				PayloadLen = (LenHighByte << 8) | (pRxBuffer[index++]);
				DleDetected = FALSE;
				MuxNextState = (ShouldCheckFcs) ? MUX_STATE_CHECK_PAYLOAD_WITH_FCS : MUX_STATE_CHECK_PAYLOAD_NO_FCS;
				if(ShouldCheckFcs)
				{
					ucFCS=0xFF;
				}
				break;

			case MUX_STATE_CHECK_PAYLOAD_WITH_FCS:
				printk(KERN_INFO "MUX_STATE_CHECK_PAYLOAD_WITH_FCS\r\n");
				ucFCS=crctable[ucFCS^pRxBuffer[index]];
				MuxDecodeRxBuffer[PayloadIdx++] = pRxBuffer[index++];
				DleDetected = FALSE;
				if(PayloadIdx == PayloadLen)
				{
					MuxNextState = MUX_STATE_CHECK_FCS;
				}
				break;
/*
			case MUX_STATE_CHECK_PAYLOAD_NO_FCS:
				if(MUX_BYTE_START == pRxBuffer[index] || MUX_BYTE_DLE == pRxBuffer[index])
				{
					MuxDecodeRxBuffer[PayloadIdx++] = MUX_BYTE_DLE;
				}
				MuxDecodeRxBuffer[PayloadIdx++] = pRxBuffer[index];
				index++;
				PayloadIdxTrace++;
				DleDetected = FALSE;
				if(PayloadIdxTrace == PayloadLen)
				{
					if(MUX_TYPE_TRACE == gMuxsrcType)
					{
						MuxDecodeRxBuffer[PayloadIdx++] = MUX_BYTE_START;
					}
					index++;
					MuxNextState = MUX_STATE_CHECK_END;
				}
				break;
*/
			case MUX_STATE_CHECK_FCS:
				printk(KERN_INFO "MUX_STATE_CHECK_FCS\r\n");
				if(ShouldCheckFcs)
				{
					ucFCS=crctable[ucFCS^MuxDecodeRxBuffer[index]];
				}
				index++;
				DleDetected = FALSE;
				if(!ShouldCheckFcs || ucFCS == 0xCF)
				{
					MuxNextState = MUX_STATE_CHECK_END;
				}
				else
				{
					printk(KERN_INFO "MUX_STATE_CHECK_FCS is wrong, but ignore checking\r\n");
					MuxNextState = MUX_STATE_CHECK_END;
				}
				/* check sum is wrong
				else
				{
					MuxNextState = MUX_STATE_CHECK_START;
					PayloadIdx = 0;
					printk(KERN_INFO "MUX_STATE_CHECK_FCS\r\n");
				}
				*/
				break;

			case MUX_STATE_CHECK_END:
				printk(KERN_INFO "MUX_STATE_CHECK_END\r\n");
				// ToDo: Based on MUX_TYPE, choose tty
				if(MUX_BYTE_END == pRxBuffer[index])
				{
					printk("[GSM_DEMUX] ");
					for (idx=0 ; idx<PayloadIdx; idx++)
						printk("%c ", MuxDecodeRxBuffer[idx]);
					printk("\n");
					if(MUX_TYPE_ATCMD == gMuxsrcType ||	MUX_TYPE_DATA == gMuxsrcType)
					{
						tty_insert_flip_string(tty, MuxDecodeRxBuffer,  PayloadIdx);
					}
				}

				index++;
				gMuxsrcType = MUX_TYPE_NONE;
				//MuxNextState = MUX_STATE_CHECK_START;
				DleDetected = FALSE;
				break;

			default:
				printk(KERN_INFO"[MUX] error state\r\n");
				break;

			}
		}
	}

Error:
	return index;
}

//Todo: based on gMuxSrcType, choose TTY
static int Mux_insert_flip_char(struct tty_struct *tty,unsigned char ch, char flag){

	if(gMuxsrcType != MUX_TYPE_NONE){
		return tty_insert_flip_char(tty, ch, flag);
	}
	return 0;
}

// irene@06132011-- Mux Decode
#endif

/*
 * This routine is called when we are done with a DMA transfer or the
 * a flush has been sent to the data mover driver.
 *
 * This routine is registered with Data mover when we set up a Data Mover
 *  transfer. It is called from Data mover ISR when the DMA transfer is done.
 */
static void msm_hs_dmov_rx_callback(struct msm_dmov_cmd *cmd_ptr,
					unsigned int result,
					struct msm_dmov_errdata *err)
{
	int retval;
	int rx_count;
	unsigned long status;
	unsigned int error_f = 0;
	unsigned long flags;
	unsigned int flush;
	struct tty_struct *tty;
	struct uart_port *uport;
	struct msm_hs_port *msm_uport;

	int idx = 0;

	msm_uport = container_of(cmd_ptr, struct msm_hs_port, rx.xfer);
	uport = &msm_uport->uport;

	printk(KERN_INFO MODULE_NAME "%s +\n", __FUNCTION__);

	spin_lock_irqsave(&uport->lock, flags);
	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	tty = uport->state->port.tty;

	msm_hs_write(uport, UARTDM_CR_ADDR, STALE_EVENT_DISABLE);

	status = msm_hs_read(uport, UARTDM_SR_ADDR);
	printk(KERN_INFO MODULE_NAME "%s msm_hs_read UARTDM_SR_ADDR(0x%lx)=0x%lx\n", __FUNCTION__, (unsigned long)uport->membase+UARTDM_SR_ADDR, status);

	/* overflow is not connect to data in a FIFO */
	if (unlikely((status & UARTDM_SR_OVERRUN_BMSK) &&
		     (uport->read_status_mask & CREAD))) {
#ifdef 	CONFIG_LOCOSTO_UART2DM_MUX
		Mux_insert_flip_char(tty, 0, TTY_OVERRUN);
#else
		tty_insert_flip_char(tty, 0, TTY_OVERRUN);
#endif
		uport->icount.buf_overrun++;
		error_f = 1;
	}

	if (!(uport->ignore_status_mask & INPCK))
		status = status & ~(UARTDM_SR_PAR_FRAME_BMSK);

	if (unlikely(status & UARTDM_SR_PAR_FRAME_BMSK)) {
		/* Can not tell difference between parity & frame error */
		uport->icount.parity++;
		error_f = 1;
		if (uport->ignore_status_mask & IGNPAR)
#ifdef 	CONFIG_LOCOSTO_UART2DM_MUX
			Mux_insert_flip_char(tty, 0, TTY_PARITY);
#else
			tty_insert_flip_char(tty, 0, TTY_PARITY);
#endif
	}

	if (error_f)
		msm_hs_write(uport, UARTDM_CR_ADDR, RESET_ERROR_STATUS);

	if (msm_uport->clk_req_off_state == CLK_REQ_OFF_FLUSH_ISSUED)
		msm_uport->clk_req_off_state = CLK_REQ_OFF_RXSTALE_FLUSHED;

	flush = msm_uport->rx.flush;
	if (flush == FLUSH_IGNORE)
		msm_hs_start_rx_locked(uport);
	if (flush == FLUSH_STOP)
		msm_uport->rx.flush = FLUSH_SHUTDOWN;
	if (flush >= FLUSH_DATA_INVALID)
		goto out;

	rx_count = msm_hs_read(uport, UARTDM_RX_TOTAL_SNAP_ADDR);
	printk(KERN_INFO MODULE_NAME "%s msm_hs_read UARTDM_RX_TOTAL_SNAP_ADDR(0x%lx)=0x%x\n", __FUNCTION__, (unsigned long)uport->membase+UARTDM_RX_TOTAL_SNAP_ADDR, rx_count);
	/*
	 * Complete device writel before invalidating Rx buffer
	 * and using the same.
	 */
	dsb();

	printk("[GSM_RADIO] ");
	for (idx=0 ; idx<rx_count; idx++)
		printk("%x ", msm_uport->rx.buffer[idx]);
	printk("\n");

	if (0 != (uport->read_status_mask & CREAD)) {
#ifdef CONFIG_LOCOSTO_UART2DM_MUX
	    retval = MuxParseData(tty, msm_uport->rx.buffer,
						rx_count);
#else
		retval = tty_insert_flip_string(tty, msm_uport->rx.buffer,
						rx_count);
#endif
		BUG_ON(retval != rx_count);
	}

	msm_hs_start_rx_locked(uport);

out:
	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
	/* release wakelock in 500ms, not immediately, because higher layers
	 * don't always take wakelocks when they should */
	wake_lock_timeout(&msm_uport->rx.wake_lock, HZ / 2);
	spin_unlock_irqrestore(&uport->lock, flags);

	if (flush < FLUSH_DATA_INVALID)
		queue_work(msm_hs_workqueue, &msm_uport->rx.tty_work);

	printk(KERN_INFO MODULE_NAME "%s -\n", __FUNCTION__);

}

static void msm_hs_tty_flip_buffer_work(struct work_struct *work)
{
	struct msm_hs_port *msm_uport =
			container_of(work, struct msm_hs_port, rx.tty_work);
	struct tty_struct *tty = msm_uport->uport.state->port.tty;

	tty_flip_buffer_push(tty);
}

/*
 *  Standard API, Current states of modem control inputs
 *
 * Since CTS can be handled entirely by HARDWARE we always
 * indicate clear to send and count on the TX FIFO to block when
 * it fills up.
 *
 * - TIOCM_DCD
 * - TIOCM_CTS
 * - TIOCM_DSR
 * - TIOCM_RI
 *  (Unsupported) DCD and DSR will return them high. RI will return low.
 */
static unsigned int msm_hs_get_mctrl_locked(struct uart_port *uport)
{
	return TIOCM_DSR | TIOCM_CAR | TIOCM_CTS;
}

/*
 * True enables UART auto RFR, which indicates we are ready for data if the RX
 * buffer is not full. False disables auto RFR, and deasserts RFR to indicate
 * we are not ready for data. Must be called with UART clock on.
 */
static void set_rfr_locked(struct uart_port *uport, int auto_rfr)
{
	unsigned int data;

	data = msm_hs_read(uport, UARTDM_MR1_ADDR);

	if (auto_rfr) {
		/* enable auto ready-for-receiving */
		data |= UARTDM_MR1_RX_RDY_CTL_BMSK;
		msm_hs_write(uport, UARTDM_MR1_ADDR, data);
	} else {
		/* disable auto ready-for-receiving */
		data &= ~UARTDM_MR1_RX_RDY_CTL_BMSK;
		msm_hs_write(uport, UARTDM_MR1_ADDR, data);
		/* RFR is active low, set high */
		msm_hs_write(uport, UARTDM_CR_ADDR, RFR_HIGH);
	}
	/* Calling CLOCK API. Hence dsb() requires. */
	dsb();
}

/*
 *  Standard API, used to set or clear RFR
 */
static void msm_hs_set_mctrl_locked(struct uart_port *uport,
				    unsigned int mctrl)
{
	unsigned int auto_rfr;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	auto_rfr = TIOCM_RTS & mctrl ? 1 : 0;
	set_rfr_locked(uport, auto_rfr);

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
}

/* Standard API, Enable modem status (CTS) interrupt  */
static void msm_hs_enable_ms_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	/* Enable DELTA_CTS Interrupt */
	msm_uport->imr_reg |= UARTDM_ISR_DELTA_CTS_BMSK;
	msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
	/* Calling CLOCK API. Hence dsb() requires here. */
	dsb();

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif

}

/*
 *  Standard API, Break Signal
 *
 * Control the transmission of a break signal. ctl eq 0 => break
 * signal terminate ctl ne 0 => start break signal
 */
static void msm_hs_break_ctl(struct uart_port *uport, int ctl)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif
	msm_hs_write(uport, UARTDM_CR_ADDR, ctl ? START_BREAK : STOP_BREAK);
	/* Calling CLOCK API. Hence dsb() requires here. */
	dsb();
	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
}

static void msm_hs_config_port(struct uart_port *uport, int cfg_flags)
{
	unsigned long flags;

	spin_lock_irqsave(&uport->lock, flags);
	if (cfg_flags & UART_CONFIG_TYPE) {
		uport->type = PORT_MSM;
		msm_hs_request_port(uport);
	}
	spin_unlock_irqrestore(&uport->lock, flags);
}

/*  Handle CTS changes (Called from interrupt handler) */
static void msm_hs_handle_delta_cts(struct uart_port *uport)
{
	unsigned long flags;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	spin_lock_irqsave(&uport->lock, flags);
	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	/* clear interrupt */
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_CTS);
	/* Calling CLOCK API. Hence dsb() requires here. */
	dsb();
	uport->icount.cts++;

	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
	spin_unlock_irqrestore(&uport->lock, flags);

	/* clear the IOCTL TIOCMIWAIT if called */
	wake_up_interruptible(&uport->state->port.delta_msr_wait);
}

/* check if the TX path is flushed, and if so clock off
 * returns 0 did not clock off, need to retry (still sending final byte)
 *        -1 did not clock off, do not retry
 *         1 if we clocked off
 */
static int msm_hs_check_clock_off_locked(struct uart_port *uport)
{
	unsigned long sr_status;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);
	struct circ_buf *tx_buf = &uport->state->xmit;

	/* Cancel if tx tty buffer is not empty, dma is in flight,
	 * or tx fifo is not empty, or rx fifo is not empty */
	if (msm_uport->clk_state != MSM_HS_CLK_REQUEST_OFF ||
		!uart_circ_empty(tx_buf) || msm_uport->tx.dma_in_flight ||
		(msm_uport->imr_reg & UARTDM_ISR_TXLEV_BMSK) ||
		!(msm_uport->imr_reg & UARTDM_ISR_RXLEV_BMSK)) {
		return -1;
	}

	/* Make sure the uart is finished with the last byte */
	sr_status = msm_hs_read(uport, UARTDM_SR_ADDR);
	if (!(sr_status & UARTDM_SR_TXEMT_BMSK))
		return 0;  /* retry */

#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	/*
		BB_INT_PDA	= 0 : PDA can enter sleep
					= 1 : PDA cannot enter sleep
	*/
	if (gpio_get_value(OBOEA_GPIO_GSM_BB_INT_PDA)) {
		msm_uport->clk_state = MSM_HS_CLK_ON;
		return -1;	/* bb_int_pda is high, PDA cannot sleep, no need to retry */
	} else {

		if (sr_status & UARTDM_SR_RXRDY_BMSK)
			return 0;  /* some data in rx fifo, so retry */

		/* bb has nothing to send to host, so we disable rfr */
		{
			unsigned int data;
			data = msm_hs_read(uport, UARTDM_MR1_ADDR);

			if (data & UARTDM_MR1_RX_RDY_CTL_BMSK) {
				/*disable auto ready-for-receiving */
				data &= ~UARTDM_MR1_RX_RDY_CTL_BMSK;
				msm_hs_write(uport, UARTDM_MR1_ADDR, data);

				/* set RFR_N to high */
				msm_hs_write(uport, UARTDM_CR_ADDR, RFR_HIGH);
				printk(KERN_INFO MODULE_NAME"- DIS RFR -\n");
			}
		}
	}
#endif

	/* Make sure forced RXSTALE flush complete */
	switch (msm_uport->clk_req_off_state) {
	case CLK_REQ_OFF_START:
		msm_uport->clk_req_off_state = CLK_REQ_OFF_RXSTALE_ISSUED;
		msm_hs_write(uport, UARTDM_CR_ADDR, FORCE_STALE_EVENT);
		/*
		 * Before returning make sure that device writel completed.
		 * Hence dsb() requires here.
		 */
		dsb();
		return 0;  /* RXSTALE flush not complete - retry */
	case CLK_REQ_OFF_RXSTALE_ISSUED:
	case CLK_REQ_OFF_FLUSH_ISSUED:
		return 0;  /* RXSTALE flush not complete - retry */
	case CLK_REQ_OFF_RXSTALE_FLUSHED:
		break;  /* continue */
	}

	if (msm_uport->rx.flush != FLUSH_SHUTDOWN) {
		if (msm_uport->rx.flush == FLUSH_NONE)
			msm_hs_stop_rx_locked(uport);
		return 0;  /* come back later to really clock off */
	}

	/* we really want to clock off */
	clk_disable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	if (radio_state){
		gpio_configure(OBOEA_GPIO_GSM_PDA_STATUS, GPIOF_DRIVE_OUTPUT | GPIOF_OUTPUT_HIGH);
		printk(KERN_INFO MODULE_NAME "%s set GPIO_PDA_STATUS high\n",__FUNCTION__);
	}
	else{
		printk(KERN_INFO MODULE_NAME "%s NOT set GPIO_PDA_STATUS high due to radio_off\n",__FUNCTION__);
	}
#endif
	msm_uport->clk_state = MSM_HS_CLK_OFF;
	wake_unlock(&msm_uport->dma_wake_lock);
	if (use_low_power_rx_wakeup(msm_uport)) {
		msm_uport->rx_wakeup.ignore = 1;
		enable_irq(msm_uport->rx_wakeup.irq);
		printk(KERN_INFO MODULE_NAME "%s enable_irq rx_wakeup.irq(%d)\n",__FUNCTION__, msm_uport->rx_wakeup.irq);
	}
	return 1;
}

static enum hrtimer_restart msm_hs_clk_off_retry(struct hrtimer *timer)
{
	unsigned long flags;
	int ret = HRTIMER_NORESTART;
	struct msm_hs_port *msm_uport = container_of(timer, struct msm_hs_port,
						     clk_off_timer);
	struct uart_port *uport = &msm_uport->uport;

	spin_lock_irqsave(&uport->lock, flags);

	if (!msm_hs_check_clock_off_locked(uport)) {
		hrtimer_forward_now(timer, msm_uport->clk_off_delay);
		ret = HRTIMER_RESTART;
	}

	spin_unlock_irqrestore(&uport->lock, flags);

	return ret;
}

static irqreturn_t msm_hs_isr(int irq, void *dev)
{
	unsigned long flags;
	unsigned long isr_status;
	struct msm_hs_port *msm_uport = (struct msm_hs_port *)dev;
	struct uart_port *uport = &msm_uport->uport;
	struct circ_buf *tx_buf = &uport->state->xmit;
	struct msm_hs_tx *tx = &msm_uport->tx;
	struct msm_hs_rx *rx = &msm_uport->rx;

	printk(KERN_INFO MODULE_NAME "%s +\n", __FUNCTION__);
	printk(KERN_INFO MODULE_NAME "irq=%d\n", irq);

	spin_lock_irqsave(&uport->lock, flags);

	isr_status = msm_hs_read(uport, UARTDM_MISR_ADDR);
	printk(KERN_INFO MODULE_NAME "%s msm_hs_read UARTDM_MISR_ADDR(0x%lx)=0x%lx\n", __FUNCTION__, (unsigned long)uport->membase+UARTDM_MISR_ADDR, isr_status);

	/* Uart RX starting */
	if (isr_status & UARTDM_ISR_RXLEV_BMSK) {
		wake_lock(&rx->wake_lock);  /* hold wakelock while rx dma */
		msm_uport->imr_reg &= ~UARTDM_ISR_RXLEV_BMSK;
		msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
	}
	/* Stale rx interrupt */
	if (isr_status & UARTDM_ISR_RXSTALE_BMSK) {
		msm_hs_write(uport, UARTDM_CR_ADDR, STALE_EVENT_DISABLE);
		msm_hs_write(uport, UARTDM_CR_ADDR, RESET_STALE_INT);
		/*
		 * Complete device write before calling DMOV API. Hence
		 * dsb() requires here.
		 */
		dsb();

		if (msm_uport->clk_req_off_state == CLK_REQ_OFF_RXSTALE_ISSUED)
			msm_uport->clk_req_off_state =
					CLK_REQ_OFF_FLUSH_ISSUED;
		if (rx->flush == FLUSH_NONE) {
			rx->flush = FLUSH_DATA_READY;
			msm_dmov_flush(msm_uport->dma_rx_channel);
		}
	}
	/* tx ready interrupt */
	if (isr_status & UARTDM_ISR_TX_READY_BMSK) {
		/* Clear  TX Ready */
		msm_hs_write(uport, UARTDM_CR_ADDR, CLEAR_TX_READY);

		if (msm_uport->clk_state == MSM_HS_CLK_REQUEST_OFF) {
			msm_uport->imr_reg |= UARTDM_ISR_TXLEV_BMSK;
			msm_hs_write(uport, UARTDM_IMR_ADDR,
				     msm_uport->imr_reg);
		}
		/*
		 * Complete both writes before starting new TX.
		 * Hence dsb() requires here.
		 */
		dsb();
		/* Complete DMA TX transactions and submit new transactions */
		tx_buf->tail = (tx_buf->tail + tx->tx_count) & ~UART_XMIT_SIZE;

		tx->dma_in_flight = 0;

		uport->icount.tx += tx->tx_count;
		if (tx->tx_ready_int_en)
			msm_hs_submit_tx_locked(uport);

		if (uart_circ_chars_pending(tx_buf) < WAKEUP_CHARS)
			uart_write_wakeup(uport);
	}
	if (isr_status & UARTDM_ISR_TXLEV_BMSK) {
		/* TX FIFO is empty */
		msm_uport->imr_reg &= ~UARTDM_ISR_TXLEV_BMSK;
		msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
		/*
		 * Complete device write before starting clock_off request.
		 * Hence dsb() requires here.
		 */
		dsb();
		if (!msm_hs_check_clock_off_locked(uport))
			hrtimer_start(&msm_uport->clk_off_timer,
				      msm_uport->clk_off_delay,
				      HRTIMER_MODE_REL);
	}

	/* Change in CTS interrupt */
	if (isr_status & UARTDM_ISR_DELTA_CTS_BMSK)
		msm_hs_handle_delta_cts(uport);

	spin_unlock_irqrestore(&uport->lock, flags);

	printk(KERN_INFO MODULE_NAME"%s -\n", __FUNCTION__);

	return IRQ_HANDLED;
}

void msm_hs_uart2_request_clock_off_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	printk(KERN_INFO MODULE_NAME "%s +\n", __FUNCTION__);

	if (msm_uport->clk_state == MSM_HS_CLK_ON) {
		msm_uport->clk_state = MSM_HS_CLK_REQUEST_OFF;
		msm_uport->clk_req_off_state = CLK_REQ_OFF_START;
		if (!use_low_power_rx_wakeup(msm_uport))
			set_rfr_locked(uport, 0);
		msm_uport->imr_reg |= UARTDM_ISR_TXLEV_BMSK;
		msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
		/*
		 * Complete device write before retuning back.
		 * Hence dsb() requires here.
		 */
		dsb();
	}
	printk(KERN_INFO MODULE_NAME "%s -\n", __FUNCTION__);

}
EXPORT_SYMBOL(msm_hs_uart2_request_clock_off_locked);

/* request to turn off uart clock once pending TX is flushed */
void msm_hs_uart2_request_clock_off(struct uart_port *uport)
{
	unsigned long flags;

	printk(KERN_INFO MODULE_NAME "%s +\n", __FUNCTION__);

	spin_lock_irqsave(&uport->lock, flags);
	msm_hs_uart2_request_clock_off_locked(uport);
	spin_unlock_irqrestore(&uport->lock, flags);

	printk(KERN_INFO MODULE_NAME "%s -\n", __FUNCTION__);

}
EXPORT_SYMBOL(msm_hs_uart2_request_clock_off);

void msm_hs_uart2_request_clock_on_locked(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);
	unsigned int data;

	printk(KERN_INFO MODULE_NAME "%s clk_state=%d +\n", __FUNCTION__, msm_uport->clk_state);

	switch (msm_uport->clk_state) {
	case MSM_HS_CLK_OFF:
		wake_lock(&msm_uport->dma_wake_lock);
		clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		gpio_configure(OBOEA_GPIO_GSM_PDA_STATUS, GPIOF_DRIVE_OUTPUT | GPIOF_OUTPUT_LOW);
		printk(KERN_INFO MODULE_NAME "%s set GPIO_PDA_STATUS low\n",__FUNCTION__);
#endif
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		printk(KERN_INFO MODULE_NAME "%s NOT disable_irq rx_wakeup.irq(%d)\n",__FUNCTION__, msm_uport->rx_wakeup.irq);
#else
		disable_irq_nosync(msm_uport->rx_wakeup.irq);
#endif
		/* fall-through */
	case MSM_HS_CLK_REQUEST_OFF:
		if (msm_uport->rx.flush == FLUSH_STOP ||
				msm_uport->rx.flush == FLUSH_SHUTDOWN) {
			msm_hs_write(uport, UARTDM_CR_ADDR, RESET_RX);
			data = msm_hs_read(uport, UARTDM_DMEN_ADDR);
			data |= UARTDM_RX_DM_EN_BMSK;
			msm_hs_write(uport, UARTDM_DMEN_ADDR, data);
			/*complete above write. hence dsb() here. */
			dsb();
		}
		hrtimer_try_to_cancel(&msm_uport->clk_off_timer);
		if (msm_uport->rx.flush == FLUSH_SHUTDOWN)
			msm_hs_start_rx_locked(uport);
		if (!use_low_power_rx_wakeup(msm_uport))
			set_rfr_locked(uport, 1);
		if (msm_uport->rx.flush == FLUSH_STOP)
			msm_uport->rx.flush = FLUSH_IGNORE;
		msm_uport->clk_state = MSM_HS_CLK_ON;
		break;
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	case MSM_HS_CLK_ON:
	case MSM_HS_CLK_PORT_OFF:
		{
			unsigned int data;

			/* If disabled, enable auto ready-for-receiving again */
			data = msm_hs_read(uport, UARTDM_MR1_ADDR);
			if (!(data & UARTDM_MR1_RX_RDY_CTL_BMSK)) {
				data |= UARTDM_MR1_RX_RDY_CTL_BMSK;
				msm_hs_write(uport, UARTDM_MR1_ADDR, data);
				printk(KERN_INFO MODULE_NAME "- EN RFR -\n");
			}
		}
		break;
#else
	case MSM_HS_CLK_ON: break;
	case MSM_HS_CLK_PORT_OFF: break;
#endif
	}

	printk(KERN_INFO MODULE_NAME "%s clk_state=%d -\n", __FUNCTION__, msm_uport->clk_state);

}
EXPORT_SYMBOL(msm_hs_uart2_request_clock_on_locked);

void msm_hs_uart2_request_clock_on(struct uart_port *uport)
{
	unsigned long flags;
	spin_lock_irqsave(&uport->lock, flags);
	msm_hs_uart2_request_clock_on_locked(uport);
	spin_unlock_irqrestore(&uport->lock, flags);
}
EXPORT_SYMBOL(msm_hs_uart2_request_clock_on);

static irqreturn_t msm_hs_rx_wakeup_isr(int irq, void *dev)
{
	unsigned int wakeup = 0;
	unsigned long flags;
	struct msm_hs_port *msm_uport = (struct msm_hs_port *)dev;
	struct uart_port *uport = &msm_uport->uport;
	struct tty_struct *tty = NULL;

	printk(KERN_INFO MODULE_NAME "%s clk_state=%d +\n", __FUNCTION__, msm_uport->clk_state);

	printk(KERN_INFO MODULE_NAME "irq=%d\n", irq);

	spin_lock_irqsave(&uport->lock, flags);
	if (msm_uport->clk_state == MSM_HS_CLK_OFF) {
		/* ignore the first irq - it is a pending irq that occured
		 * before enable_irq() */
		if (msm_uport->rx_wakeup.ignore)
			msm_uport->rx_wakeup.ignore = 0;
		else
			wakeup = 1;
	}

	if (wakeup) {
		/* the uart was clocked off during an rx, wake up and
		 * optionally inject char into tty rx */
		msm_hs_uart2_request_clock_on_locked(uport);
		if (msm_uport->rx_wakeup.inject_rx) {
			tty = uport->state->port.tty;
#ifdef CONFIG_LOCOSTO_UART2DM_MUX
			Mux_insert_flip_char(tty,
					     msm_uport->rx_wakeup.rx_to_inject,
					     TTY_NORMAL);
#else
			tty_insert_flip_char(tty,
					     msm_uport->rx_wakeup.rx_to_inject,
					     TTY_NORMAL);
#endif
			queue_work(msm_hs_workqueue, &msm_uport->rx.tty_work);
		}
	}

	spin_unlock_irqrestore(&uport->lock, flags);

	printk(KERN_INFO MODULE_NAME "%s clk_state=%d -\n", __FUNCTION__, msm_uport->clk_state);

	return IRQ_HANDLED;
}

static const char *msm_hs_type(struct uart_port *port)
{
	return ("MSM HS UART");
}

#define UART2DM_NS_REG_OFFSET 0x012c
static unsigned int uart_clk_port_base;

static inline void msm_write_clk(unsigned int val, unsigned int off)
{
	__raw_writel(val, uart_clk_port_base + off);
}

static inline unsigned int msm_read_clk(unsigned int off)
{
	return __raw_readl(uart_clk_port_base + off);
}

/* Called when port is opened */
static int msm_hs_startup(struct uart_port *uport)
{
	int ret;
	int rfr_level;
	unsigned long flags;
	unsigned int data;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);
	struct circ_buf *tx_buf = &uport->state->xmit;
	struct msm_hs_tx *tx = &msm_uport->tx;
	struct msm_hs_rx *rx = &msm_uport->rx;
	unsigned int ns_reg;

	printk(KERN_INFO MODULE_NAME "%s\n", __FUNCTION__);

	rfr_level = uport->fifosize;
	if (rfr_level > 16)
		rfr_level -= 16;

	tx->dma_base = dma_map_single(uport->dev, tx_buf->buf, UART_XMIT_SIZE,
				      DMA_TO_DEVICE);

	/* do not let tty layer execute RX in global workqueue, use a
	 * dedicated workqueue managed by this driver */
	uport->state->port.tty->low_latency = 1;

	uart_clk_port_base = (unsigned int) MSM_CLK_CTL_BASE ;

	ns_reg = msm_read_clk(UART2DM_NS_REG_OFFSET);
	printk(KERN_INFO MODULE_NAME "%s : UART2DM_NS_REG=0x%x  +\n", __FUNCTION__, ns_reg);

	/* turn on uart clk */
	ret = msm_hs_init_clk_locked(uport);

	ns_reg = msm_read_clk(UART2DM_NS_REG_OFFSET);
	printk(KERN_INFO MODULE_NAME "%s : UART2DM_NS_REG=0x%x  -\n", __FUNCTION__, ns_reg);

	if (unlikely(ret))
		return ret;

	/* Set auto RFR Level */
	data = msm_hs_read(uport, UARTDM_MR1_ADDR);
	data &= ~UARTDM_MR1_AUTO_RFR_LEVEL1_BMSK;
	data &= ~UARTDM_MR1_AUTO_RFR_LEVEL0_BMSK;
	data |= (UARTDM_MR1_AUTO_RFR_LEVEL1_BMSK & (rfr_level << 2));
	data |= (UARTDM_MR1_AUTO_RFR_LEVEL0_BMSK & rfr_level);
	msm_hs_write(uport, UARTDM_MR1_ADDR, data);

	/* Make sure RXSTALE count is non-zero */
	data = msm_hs_read(uport, UARTDM_IPR_ADDR);
	if (!data) {
		data |= 0x1f & UARTDM_IPR_STALE_LSB_BMSK;
		msm_hs_write(uport, UARTDM_IPR_ADDR, data);
	}

	/* Enable Data Mover Mode */
	data = UARTDM_TX_DM_EN_BMSK | UARTDM_RX_DM_EN_BMSK;
	msm_hs_write(uport, UARTDM_DMEN_ADDR, data);

	/* Reset TX */
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_TX);
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_RX);
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_ERROR_STATUS);
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_BREAK_INT);
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_STALE_INT);
	msm_hs_write(uport, UARTDM_CR_ADDR, RESET_CTS);
	msm_hs_write(uport, UARTDM_CR_ADDR, RFR_LOW);
	/* Turn on Uart Receiver */
	msm_hs_write(uport, UARTDM_CR_ADDR, UARTDM_CR_RX_EN_BMSK);

	/* Turn on Uart Transmitter */
	msm_hs_write(uport, UARTDM_CR_ADDR, UARTDM_CR_TX_EN_BMSK);

	/* Initialize the tx */
	tx->tx_ready_int_en = 0;
	tx->dma_in_flight = 0;

	tx->xfer.complete_func = msm_hs_dmov_tx_callback;
	tx->xfer.exec_func = NULL;

	tx->command_ptr->cmd = CMD_LC |
	    CMD_DST_CRCI(msm_uport->dma_tx_crci) | CMD_MODE_BOX;

	tx->command_ptr->src_dst_len = (MSM_UARTDM_BURST_SIZE << 16)
					   | (MSM_UARTDM_BURST_SIZE);

	tx->command_ptr->row_offset = (MSM_UARTDM_BURST_SIZE << 16);

	tx->command_ptr->dst_row_addr =
	    msm_uport->uport.mapbase + UARTDM_TF_ADDR;


	/* Turn on Uart Receive */
	rx->xfer.complete_func = msm_hs_dmov_rx_callback;
	rx->xfer.exec_func = NULL;

	rx->command_ptr->cmd = CMD_LC |
	    CMD_SRC_CRCI(msm_uport->dma_rx_crci) | CMD_MODE_BOX;

	rx->command_ptr->src_dst_len = (MSM_UARTDM_BURST_SIZE << 16)
					   | (MSM_UARTDM_BURST_SIZE);
	rx->command_ptr->row_offset =  MSM_UARTDM_BURST_SIZE;
	rx->command_ptr->src_row_addr = uport->mapbase + UARTDM_RF_ADDR;


	msm_uport->imr_reg |= UARTDM_ISR_RXSTALE_BMSK;
	/* Enable reading the current CTS, no harm even if CTS is ignored */
	msm_uport->imr_reg |= UARTDM_ISR_CURRENT_CTS_BMSK;

	msm_hs_write(uport, UARTDM_TFWR_ADDR, 0);  /* TXLEV on empty TX fifo */
	/*
	 * Complete all device write related configuration before
	 * queuing RX request. Hence dsb() requires here.
	 */
	dsb();

	ret = request_irq(uport->irq, msm_hs_isr, IRQF_TRIGGER_HIGH,
			  "msm_hs_uart_locosto", msm_uport);
	printk(KERN_INFO MODULE_NAME "irq(msm_hs_uart_locosto)=%d\n", uport->irq);

	if (unlikely(ret))
		return ret;
	if (use_low_power_rx_wakeup(msm_uport)) {
		/* move from startup  **/
		if (unlikely(set_irq_wake(msm_uport->rx_wakeup.irq, 1)))
			return -ENXIO;
		ret = request_irq(msm_uport->rx_wakeup.irq,
				  msm_hs_rx_wakeup_isr,
				  IRQF_TRIGGER_RISING,
				  "msm_hs_rx_wakeup_locosto", msm_uport);
		printk(KERN_INFO MODULE_NAME "irq(msm_hs_rx_wakeup_locosto)=%d\n", msm_uport->rx_wakeup.irq);

		if (unlikely(ret))
			return ret;

#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		printk(KERN_INFO MODULE_NAME "%s NOT disable_irq rx_wakeup.irq(%d)\n",__FUNCTION__, msm_uport->rx_wakeup.irq);
#else
		disable_irq(msm_uport->rx_wakeup.irq);
#endif
	}

	spin_lock_irqsave(&uport->lock, flags);

	msm_hs_write(uport, UARTDM_RFWR_ADDR, 0);
	msm_hs_start_rx_locked(uport);

	spin_unlock_irqrestore(&uport->lock, flags);

	return 0;
}

/* Initialize tx and rx data structures */
static int uartdm_init_port(struct uart_port *uport)
{
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);
	struct msm_hs_tx *tx = &msm_uport->tx;
	struct msm_hs_rx *rx = &msm_uport->rx;

	/* Allocate the command pointer. Needs to be 64 bit aligned */
	tx->command_ptr = kmalloc(sizeof(dmov_box), GFP_KERNEL | __GFP_DMA);

	tx->command_ptr_ptr = kmalloc(sizeof(u32), GFP_KERNEL | __GFP_DMA);

	if (!tx->command_ptr || !tx->command_ptr_ptr)
		return -ENOMEM;

	tx->mapped_cmd_ptr = dma_map_single(uport->dev, tx->command_ptr,
					    sizeof(dmov_box), DMA_TO_DEVICE);
	tx->mapped_cmd_ptr_ptr = dma_map_single(uport->dev,
						tx->command_ptr_ptr,
						sizeof(u32), DMA_TO_DEVICE);
	tx->xfer.cmdptr = DMOV_CMD_ADDR(tx->mapped_cmd_ptr_ptr);

	init_waitqueue_head(&rx->wait);
	wake_lock_init(&rx->wake_lock, WAKE_LOCK_SUSPEND, "msm_serial_hs_rx_locosto");
	wake_lock_init(&msm_uport->dma_wake_lock, WAKE_LOCK_SUSPEND,
			"msm_serial_hs_dma_locosto");

	rx->pool = dma_pool_create("rx_buffer_pool", uport->dev,
				   UARTDM_RX_BUF_SIZE, 16, 0);

	if (!rx->pool)
		return -ENOMEM;

	rx->buffer = dma_pool_alloc(rx->pool, GFP_KERNEL, &rx->rbuffer);

	/* Allocate the command pointer. Needs to be 64 bit aligned */
	rx->command_ptr = kmalloc(sizeof(dmov_box), GFP_KERNEL | __GFP_DMA);

	rx->command_ptr_ptr = kmalloc(sizeof(u32), GFP_KERNEL | __GFP_DMA);

	if (!rx->command_ptr || !rx->command_ptr_ptr ||
	    !rx->buffer)
		return -ENOMEM;

	rx->command_ptr->num_rows = ((UARTDM_RX_BUF_SIZE >> 4) << 16) |
					 (UARTDM_RX_BUF_SIZE >> 4);

	rx->command_ptr->dst_row_addr = rx->rbuffer;

	rx->mapped_cmd_ptr = dma_map_single(uport->dev, rx->command_ptr,
					    sizeof(dmov_box), DMA_TO_DEVICE);

	*rx->command_ptr_ptr = CMD_PTR_LP | DMOV_CMD_ADDR(rx->mapped_cmd_ptr);

	rx->cmdptr_dmaaddr = dma_map_single(uport->dev, rx->command_ptr_ptr,
					    sizeof(u32), DMA_TO_DEVICE);
	rx->xfer.cmdptr = DMOV_CMD_ADDR(rx->cmdptr_dmaaddr);

	INIT_WORK(&rx->tty_work, msm_hs_tty_flip_buffer_work);

	return 0;
}

static int msm_hs_probe(struct platform_device *pdev)
{
	int ret;
	struct uart_port *uport;
	struct msm_hs_port *msm_uport;
	struct resource *resource;
	struct msm_serial_hs_platform_data *pdata = pdev->dev.platform_data;

	/* for debug */
	printk(KERN_INFO MODULE_NAME "Locosto\n");

	if (pdev->id < 0 || pdev->id >= UARTDM_NR) {
		printk(KERN_ERR MODULE_NAME "Invalid plaform device ID = %d\n", pdev->id);
		return -EINVAL;
	}

	msm_uport = &q_uart_port[pdev->id];
	uport = &msm_uport->uport;

	uport->dev = &pdev->dev;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (unlikely(!resource))
		return -ENXIO;
	uport->mapbase = resource->start;  /* virtual address */

	uport->membase = ioremap(uport->mapbase, PAGE_SIZE);
	if (unlikely(!uport->membase))
		return -ENOMEM;

	uport->irq = platform_get_irq(pdev, 0);

	printk(KERN_INFO MODULE_NAME "%s mapbase=0x%lx membase=0x%lx irq=%d\n", __FUNCTION__,
		(unsigned long)uport->mapbase, (unsigned long)uport->membase, uport->irq);

	/*
	if (unlikely(uport->irq < 0))
		return -ENXIO;
	*/
	if (unlikely(set_irq_wake(uport->irq, 1)))
		return -ENXIO;

	if (pdata == NULL || pdata->rx_wakeup_irq < 0)
		msm_uport->rx_wakeup.irq = -1;
	else {
		msm_uport->rx_wakeup.irq = pdata->rx_wakeup_irq;
		msm_uport->rx_wakeup.ignore = 1;
		msm_uport->rx_wakeup.inject_rx = pdata->inject_rx_on_wakeup;
		msm_uport->rx_wakeup.rx_to_inject = pdata->rx_to_inject;

		if (unlikely(msm_uport->rx_wakeup.irq < 0))
			return -ENXIO;
		/* move this to startup
		 * if (unlikely(set_irq_wake(msm_uport->rx_wakeup.irq, 1)))
		 * return -ENXIO;
		 */
	}

	if (pdata == NULL)
		msm_uport->exit_lpm_cb = NULL;
	else
		msm_uport->exit_lpm_cb = pdata->exit_lpm_cb;

	resource = platform_get_resource_byname(pdev, IORESOURCE_DMA,
						"uartdm_channels");
	if (unlikely(!resource))
		return -ENXIO;
	msm_uport->dma_tx_channel = resource->start;
	msm_uport->dma_rx_channel = resource->end;

	printk(KERN_INFO MODULE_NAME "dma_tx_channel=%d\n", msm_uport->dma_tx_channel);
	printk(KERN_INFO MODULE_NAME "dma_rx_channel=%d\n", msm_uport->dma_rx_channel);

	resource = platform_get_resource_byname(pdev, IORESOURCE_DMA,
						"uartdm_crci");
	if (unlikely(!resource))
		return -ENXIO;
	msm_uport->dma_tx_crci = resource->start;
	msm_uport->dma_rx_crci = resource->end;

	printk(KERN_INFO MODULE_NAME "dma_tx_crci=%d\n", msm_uport->dma_tx_crci);
	printk(KERN_INFO MODULE_NAME "dma_rx_crci=%d\n", msm_uport->dma_rx_crci);

	uport->iotype = UPIO_MEM;
	uport->fifosize = 64;
	uport->ops = &msm_hs_ops;
	uport->flags = UPF_BOOT_AUTOCONF;
	uport->uartclk = 7372800;
	msm_uport->imr_reg = 0x0;
	msm_uport->clk = clk_get(&pdev->dev, "uartdm_clk");
	printk(KERN_INFO MODULE_NAME "pdev->name=%s\n", pdev->name);

	if (IS_ERR(msm_uport->clk))
		return PTR_ERR(msm_uport->clk);

	ret = uartdm_init_port(uport);
	if (unlikely(ret))
		return ret;

	/* configure the CR Protection to Enable */
	msm_hs_write(uport, UARTDM_CR_ADDR, CR_PROTECTION_EN);
	/*
	 * Enable Command register protection before going ahead as this hw
	 * configuration makes sure that issued cmd to CR register gets complete
	 * before next issued cmd start. Hence dsb() requires here.
	 */
	dsb();

	msm_uport->clk_state = MSM_HS_CLK_PORT_OFF;
	hrtimer_init(&msm_uport->clk_off_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	msm_uport->clk_off_timer.function = msm_hs_clk_off_retry;
	msm_uport->clk_off_delay = ktime_set(0, 1000000);  /* 1ms */

	uport->line = pdev->id;
	return uart_add_one_port(&msm_hs_driver, uport);
}

static int __init msm_serial_hs_init(void)
{
	int ret;
	int i;

	/* Init all UARTS as non-configured */
	for (i = 0; i < UARTDM_NR; i++)
		q_uart_port[i].uport.type = PORT_UNKNOWN;

	msm_hs_workqueue = create_singlethread_workqueue("msm_serial_hs_locosto");

	ret = uart_register_driver(&msm_hs_driver);
	if (unlikely(ret)) {
		printk(KERN_ERR MODULE_NAME "%s failed to load\n", __func__);
		return ret;
	}
	ret = platform_driver_register(&msm_serial_hs_platform_driver);
	if (ret) {
		printk(KERN_ERR MODULE_NAME "%s failed to load\n", __func__);
		uart_unregister_driver(&msm_hs_driver);
		return ret;
	}

	printk(KERN_INFO MODULE_NAME "msm_serial_hs module loaded\n");
	return ret;
}

/*
 *  Called by the upper layer when port is closed.
 *     - Disables the port
 *     - Unhook the ISR
 */
static void msm_hs_shutdown(struct uart_port *uport)
{
	unsigned long flags;
	struct msm_hs_port *msm_uport = UARTDM_TO_MSM(uport);

	BUG_ON(msm_uport->rx.flush < FLUSH_STOP);

	spin_lock_irqsave(&uport->lock, flags);
	clk_enable(msm_uport->clk);
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_enable\n",__FUNCTION__);
#endif

	/* Disable the transmitter */
	msm_hs_write(uport, UARTDM_CR_ADDR, UARTDM_CR_TX_DISABLE_BMSK);
	/* Disable the receiver */
	msm_hs_write(uport, UARTDM_CR_ADDR, UARTDM_CR_RX_DISABLE_BMSK);

	/* disable irq wakeup when shutdown **/
	if (use_low_power_rx_wakeup(msm_uport))
		if (unlikely(set_irq_wake(msm_uport->rx_wakeup.irq, 0)))
			return;

	/* Free the interrupt */
	free_irq(uport->irq, msm_uport);
	if (use_low_power_rx_wakeup(msm_uport))
		free_irq(msm_uport->rx_wakeup.irq, msm_uport);

	msm_uport->imr_reg = 0;
	msm_hs_write(uport, UARTDM_IMR_ADDR, msm_uport->imr_reg);
	/*
	 * Complete all device write before actually disabling uartclk.
	 * Hence dsb() requires here.
	 */
	dsb();

	wait_event(msm_uport->rx.wait, msm_uport->rx.flush == FLUSH_SHUTDOWN);

	clk_disable(msm_uport->clk);  /* to balance local clk_enable() */
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
	printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
	if (msm_uport->clk_state != MSM_HS_CLK_OFF) {
		wake_unlock(&msm_uport->dma_wake_lock);
		clk_disable(msm_uport->clk);  /* to balance clk_state */
#ifdef CONFIG_LOCOSTO_UART2DM_HANDSHAKE
		printk(KERN_INFO MODULE_NAME "%s clk_disable\n",__FUNCTION__);
#endif
	}
	msm_uport->clk_state = MSM_HS_CLK_PORT_OFF;

	dma_unmap_single(uport->dev, msm_uport->tx.dma_base,
			 UART_XMIT_SIZE, DMA_TO_DEVICE);

	spin_unlock_irqrestore(&uport->lock, flags);

	if (cancel_work_sync(&msm_uport->rx.tty_work))
		msm_hs_tty_flip_buffer_work(&msm_uport->rx.tty_work);

	/* make sure wake_lock is released */
	wake_lock_timeout(&msm_uport->rx.wake_lock, HZ / 10);
}

static void __exit msm_serial_hs_exit(void)
{
	printk(KERN_INFO MODULE_NAME "msm_serial_hs module removed\n");
	platform_driver_unregister(&msm_serial_hs_platform_driver);
	uart_unregister_driver(&msm_hs_driver);
	destroy_workqueue(msm_hs_workqueue);
}

static struct platform_driver msm_serial_hs_platform_driver = {
	.probe = msm_hs_probe,
	.remove = msm_hs_remove,
	.driver = {
		   .name = "msm_serial_hs_locosto",
		   },
};

static struct uart_driver msm_hs_driver = {
	.owner = THIS_MODULE,
	.driver_name = "msm_serial_hs_locosto",
	.dev_name = "ttyHS",
	.nr = UARTDM_NR,
	.cons = 0,
};

static struct uart_ops msm_hs_ops = {
	.tx_empty = msm_hs_tx_empty,
	.set_mctrl = msm_hs_set_mctrl_locked,
	.get_mctrl = msm_hs_get_mctrl_locked,
	.stop_tx = msm_hs_stop_tx_locked,
	.start_tx = msm_hs_start_tx_locked,
	.stop_rx = msm_hs_stop_rx_locked,
	.enable_ms = msm_hs_enable_ms_locked,
	.break_ctl = msm_hs_break_ctl,
	.startup = msm_hs_startup,
	.shutdown = msm_hs_shutdown,
	.set_termios = msm_hs_set_termios,
	.pm = msm_hs_pm,
	.type = msm_hs_type,
	.config_port = msm_hs_config_port,
	.release_port = msm_hs_release_port,
	.request_port = msm_hs_request_port,
};

module_init(msm_serial_hs_init);
module_exit(msm_serial_hs_exit);
MODULE_DESCRIPTION("High Speed UART Driver for the MSM chipset");
MODULE_VERSION("1.2");
MODULE_LICENSE("GPL v2");
