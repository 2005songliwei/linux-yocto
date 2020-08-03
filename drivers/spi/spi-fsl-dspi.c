// SPDX-License-Identifier: GPL-2.0+
//
// Copyright 2013 Freescale Semiconductor, Inc.
// Copyright 2020 NXP
//
// Freescale DSPI driver
// This file contains a driver for the Freescale DSPI

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regmap.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-fsl-dspi.h>

#define DRIVER_NAME			"fsl-dspi"

#define TRAN_STATE_RX_VOID		0x01
#define TRAN_STATE_TX_VOID		0x02
#define TRAN_STATE_WORD_ODD_NUM	0x04

#ifdef CONFIG_M5441x
#define DSPI_FIFO_SIZE			16
#else
#define DSPI_FIFO_SIZE			4
#endif

#define DSPI_DMA_BUFSIZE(dspi)		(dspi->fifo_size * 1024)

/* Module Configuration Register (SPI_MCR) */
#define SPI_MCR			0x00
#define SPI_MCR_MASTER			BIT(31)
#define SPI_MCR_PCSIS(x)		((x) << 16)
#define SPI_MCR_CLR_TXF			BIT(11)
#define SPI_MCR_CLR_RXF			BIT(10)
#define SPI_MCR_XSPI			BIT(3)
#define SPI_MCR_DIS_TXF			BIT(13)
#define SPI_MCR_DIS_RXF			BIT(12)
#define SPI_MCR_HALT			BIT(0)

/* Transfer Count Register (SPI_TCR) */
#define SPI_TCR				0x08
#define SPI_TCR_GET_TCNT(x)		(((x) & GENMASK(31, 16)) >> 16)

/* Clock and Transfer Attribute Register (SPI_CTARn) - Master Mode */
#define SPI_CTAR(x)			(0x0c + (((x) & GENMASK(1, 0)) * 4))
#define SPI_CTAR_FMSZ(x)		(((x) << 27) & GENMASK(30, 27))
#define SPI_CTAR_CPOL(x)		((x) << 26)
#define SPI_CTAR_CPHA(x)		((x) << 25)
#define SPI_CTAR_LSBFE(x)		((x) << 24)
#define SPI_CTAR_PCSSCK(x)		(((x) << 22) & GENMASK(23, 22))
#define SPI_CTAR_PASC(x)		(((x) << 20) & GENMASK(21, 20))
#define SPI_CTAR_PDT(x)			(((x) << 18) & GENMASK(19, 18))
#define SPI_CTAR_PBR(x)			(((x) << 16) & GENMASK(17, 16))
#define SPI_CTAR_CSSCK(x)		(((x) << 12) & GENMASK(15, 12))
#define SPI_CTAR_ASC(x)			(((x) << 8) & GENMASK(11, 8))
#define SPI_CTAR_DT(x)			(((x) << 4) & GENMASK(7, 4))
#define SPI_CTAR_BR(x)			((x) & GENMASK(3, 0))
#define SPI_CTAR_SCALE_BITS		0xf

#define SPI_CTAR0_SLAVE			0x0c

/* Status Register (SPI_SR) */
#define SPI_SR				0x2c
#define SPI_SR_TCFQF			BIT(31)
#define SPI_SR_EOQF			BIT(28)
#define SPI_SR_TFUF			BIT(27)
#define SPI_SR_TFFF			BIT(25)
#define SPI_SR_TXRXS		BIT(30)
#define SPI_SR_CMDTCF			BIT(23)
#define SPI_SR_SPEF			BIT(21)
#define SPI_SR_RFOF			BIT(19)
#define SPI_SR_TFIWF			BIT(18)
#define SPI_SR_RFDF			BIT(17)
#define SPI_SR_CMDFFF			BIT(16)
#define SPI_SR_CLEAR			(SPI_SR_TCFQF | SPI_SR_EOQF | \
					SPI_SR_TFUF | SPI_SR_TFFF | \
					SPI_SR_CMDTCF | SPI_SR_SPEF | \
					SPI_SR_RFOF | SPI_SR_TFIWF | \
					SPI_SR_RFDF | SPI_SR_CMDFFF)

/* DMA/Interrupts Request Select and Enable Register (SPI_RSER) */
#define SPI_RSER_TFFFE			BIT(25)
#define SPI_RSER_TFFFD			BIT(24)
#define SPI_RSER_RFDFE			BIT(17)
#define SPI_RSER_RFDFD			BIT(16)

#define SPI_RSER			0x30
#define SPI_RSER_TCFQE			BIT(31)
#define SPI_RSER_EOQFE			BIT(28)

/* PUSH TX FIFO Register in Master Mode (SPI_PUSHR) */
#define SPI_PUSHR			0x34
#define SPI_PUSHR_CMD_CONT		BIT(15)
#define SPI_PUSHR_CMD_CTAS(x)		(((x) << 12 & GENMASK(14, 12)))
#define SPI_PUSHR_CMD_EOQ		BIT(11)
#define SPI_PUSHR_EOQ               (SPI_PUSHR_CMD_EOQ << 16)
#define SPI_PUSHR_CMD_CTCNT		BIT(10)
#define SPI_PUSHR_CTCNT		 (SPI_PUSHR_CMD_CTCNT << 16)
#define SPI_PUSHR_CMD_PCS(x, y)        ((BIT(x)) & (y))
#define SPI_PUSHR_PCS(x, y)    (SPI_PUSHR_CMD_PCS(x, y) << 16)
#define SPI_PUSHR_TXDATA(x)	((x) & 0x0000ffff)

#define SPI_PUSHR_SLAVE			0x34

/* POP RX FIFO Register (SPI_POPR) */
#define SPI_POPR			0x38
#define SPI_POPR_RXDATA_8(x)    ((x) & 0x000000ff)
#define SPI_POPR_RXDATA_16(x)   ((x) & 0x0000ffff)
#define SPI_POPR_RXDATA_32(x)   ((x) & 0xffffffff)

/* Transmit FIFO Registers (SPI_TXFRn) */
#define SPI_TXFR(x)		(0x3c + (((x) & 0xf) << 2))

/* Receive FIFO Registers (SPI_RXFRn) */
#define SPI_RXFR(x)		(0x7c + (((x) & 0xf) << 2))

/* Clock and Transfer Attribute Register Extended (SPI_CTAREn) */
#define SPI_CTARE(x)			(0x11c + (((x) & 0x3) * 4))
#define SPI_CTARE_FMSZE(x)		(((x) & 0x00000010) << 12)
#define SPI_CTARE_FMSZE_MASK	SPI_CTARE_FMSZE(0x10)
#define SPI_CTARE_DTCP(x)		((x) & 0x7ff)

/* Status Register Extended */
#define SPI_SREX			0x13c

#define SPI_FRAME_BITS(bits)		SPI_CTAR_FMSZ((bits) - 1)
#define SPI_FRAME_EBITS(bits)		SPI_CTARE_FMSZE(((bits) - 1) >> 4)

/* Register offsets for regmap_pushr */
#define PUSHR_CMD			0x0
#define PUSHR_TX			0x2

#define DMA_COMPLETION_TIMEOUT		msecs_to_jiffies(3000)

enum frame_mode {
	FM_BYTES_1 = 0,
	FM_BYTES_2,
	FM_BYTES_4,
};

struct chip_data {
 	u32 mcr_val;
 	u32 ctar_val;
	u32 ctare_val;
 	u16 void_write_data;
};

enum dspi_trans_mode {
	DSPI_EOQ_MODE = 0,
	DSPI_TCFQ_MODE,
	DSPI_DMA_MODE,
};

struct fsl_dspi_devtype_data {
	enum dspi_trans_mode	trans_mode;
	u8			max_clock_factor;
	bool			xspi_mode;
};

static const struct fsl_dspi_devtype_data vf610_data = {
	.trans_mode		= DSPI_DMA_MODE,
	.max_clock_factor	= 2,
};

static const struct fsl_dspi_devtype_data ls1021a_v1_data = {
	.trans_mode		= DSPI_TCFQ_MODE,
	.max_clock_factor	= 8,
	.xspi_mode		= true,
};

static const struct fsl_dspi_devtype_data ls2085a_data = {
	.trans_mode		= DSPI_TCFQ_MODE,
	.max_clock_factor	= 8,
};

static const struct fsl_dspi_devtype_data s32_data = {
	.trans_mode = DSPI_EOQ_MODE,
	.max_clock_factor = 1,
};

static const struct fsl_dspi_devtype_data s32r45_data = {
	.trans_mode = DSPI_TCFQ_MODE,
	.max_clock_factor = 1,
};

static const struct fsl_dspi_devtype_data coldfire_data = {
	.trans_mode		= DSPI_EOQ_MODE,
	.max_clock_factor	= 8,
};

struct fsl_dspi_dma {
	/* Length of transfer in words of fifo_size */
	u32					curr_xfer_len;

	u32					*tx_dma_buf;
	struct dma_chan				*chan_tx;
	dma_addr_t				tx_dma_phys;
	struct completion			cmd_tx_complete;
	struct dma_async_tx_descriptor		*tx_desc;

	u32					*rx_dma_buf;
	struct dma_chan				*chan_rx;
	dma_addr_t				rx_dma_phys;
	struct completion			cmd_rx_complete;
	struct dma_async_tx_descriptor		*rx_desc;
};

struct fsl_dspi {
	struct spi_controller			*ctlr;
	struct platform_device			*pdev;

	struct regmap				*regmap;
	struct regmap				*regmap_pushr;
	void __iomem				*base;
	int					irq;
	struct clk				*clk;

	struct spi_transfer			*cur_transfer;
	struct spi_message			*cur_msg;
	struct chip_data			*cur_chip;
	size_t					len;
	void					*tx;
	void					*tx_end;
	void					*rx;
	void					*rx_end;
	char					dataflags;
	u16					void_write_data;
	u16					tx_cmd;
	u8					bits_per_word;
	u8					bytes_per_word;
	const struct fsl_dspi_devtype_data	*devtype_data;
	size_t			queue_size;
	size_t			fifo_size;
	u32			pcs_mask;
	bool			extended_mode;

	wait_queue_head_t			waitq;
	u32					waitflags;

	struct fsl_dspi_dma			*dma;
};

static u32 dspi_data_to_pushr(struct fsl_dspi *dspi, int tx_word);

static inline enum frame_mode get_frame_mode(struct fsl_dspi *dspi)
{
	unsigned int val;

	regmap_read(dspi->regmap, SPI_MCR, &val);
	if (val & SPI_MCR_XSPI) {
		regmap_read(dspi->regmap, SPI_CTARE(0), &val);
		if (val & SPI_CTARE_FMSZE_MASK)
			return FM_BYTES_4;
	}

	regmap_read(dspi->regmap, SPI_CTAR(0), &val);
	if ((val & SPI_CTAR_FMSZ(0xf)) == SPI_FRAME_BITS(8))
		return FM_BYTES_1;
	return FM_BYTES_2;
}

static inline int bytes_per_frame(enum frame_mode fm)
{
	return 1 << (int)fm;
}

static u32 dspi_pop_tx(struct fsl_dspi *dspi)
{
	u32 txdata = 0;

	if (dspi->tx) {
		if (dspi->bytes_per_word == 1)
			txdata = *(u8 *)dspi->tx;
		else if (dspi->bytes_per_word == 2)
			txdata = *(u16 *)dspi->tx;
		else  /* dspi->bytes_per_word == 4 */
			txdata = *(u32 *)dspi->tx;
		dspi->tx += dspi->bytes_per_word;
	}
	dspi->len -= dspi->bytes_per_word;
	return txdata;
}

static u32 dspi_pop_tx_pushr(struct fsl_dspi *dspi)
{
	u16 cmd = dspi->tx_cmd, data = dspi_pop_tx(dspi);

	if (spi_controller_is_slave(dspi->ctlr))
		return data;

	if (dspi->len > 0)
		cmd |= SPI_PUSHR_CMD_CONT;
	return cmd << 16 | data;
}

static void dspi_push_rx(struct fsl_dspi *dspi, u32 rxdata)
{
	if (!dspi->rx)
		return;

	/* Mask off undefined bits */
	rxdata &= (1 << dspi->bits_per_word) - 1;

	if (dspi->bytes_per_word == 1)
		*(u8 *)dspi->rx = rxdata;
	else if (dspi->bytes_per_word == 2)
		*(u16 *)dspi->rx = rxdata;
	else /* dspi->bytes_per_word == 4 */
		*(u32 *)dspi->rx = rxdata;
	dspi->rx += dspi->bytes_per_word;
}

static void dspi_tx_dma_callback(void *arg)
{
	struct fsl_dspi *dspi = arg;
	struct fsl_dspi_dma *dma = dspi->dma;

	complete(&dma->cmd_tx_complete);
}

static void dspi_rx_dma_callback(void *arg)
{
	struct fsl_dspi *dspi = arg;
	struct fsl_dspi_dma *dma = dspi->dma;
	int i;

	if (dspi->rx) {
		for (i = 0; i < dma->curr_xfer_len; i++)
			dspi_push_rx(dspi, dspi->dma->rx_dma_buf[i]);
	}

	complete(&dma->cmd_rx_complete);
}

static int dspi_next_xfer_dma_submit(struct fsl_dspi *dspi)
{
	struct device *dev = &dspi->pdev->dev;
	struct fsl_dspi_dma *dma = dspi->dma;
	int time_left;
	int i;

	for (i = 0; i < dma->curr_xfer_len; i++)
		dspi->dma->tx_dma_buf[i] = dspi_pop_tx_pushr(dspi);

	dma->tx_desc = dmaengine_prep_slave_single(dma->chan_tx,
					dma->tx_dma_phys,
					dma->curr_xfer_len *
					DMA_SLAVE_BUSWIDTH_4_BYTES,
					DMA_MEM_TO_DEV,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!dma->tx_desc) {
		dev_err(dev, "Not able to get desc for DMA xfer\n");
		return -EIO;
	}

	dma->tx_desc->callback = dspi_tx_dma_callback;
	dma->tx_desc->callback_param = dspi;
	if (dma_submit_error(dmaengine_submit(dma->tx_desc))) {
		dev_err(dev, "DMA submit failed\n");
		return -EINVAL;
	}

	dma->rx_desc = dmaengine_prep_slave_single(dma->chan_rx,
					dma->rx_dma_phys,
					dma->curr_xfer_len *
					DMA_SLAVE_BUSWIDTH_4_BYTES,
					DMA_DEV_TO_MEM,
					DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!dma->rx_desc) {
		dev_err(dev, "Not able to get desc for DMA xfer\n");
		return -EIO;
	}

	dma->rx_desc->callback = dspi_rx_dma_callback;
	dma->rx_desc->callback_param = dspi;
	if (dma_submit_error(dmaengine_submit(dma->rx_desc))) {
		dev_err(dev, "DMA submit failed\n");
		return -EINVAL;
	}

	reinit_completion(&dspi->dma->cmd_rx_complete);
	reinit_completion(&dspi->dma->cmd_tx_complete);

	dma_async_issue_pending(dma->chan_rx);
	dma_async_issue_pending(dma->chan_tx);

	if (spi_controller_is_slave(dspi->ctlr)) {
		wait_for_completion_interruptible(&dspi->dma->cmd_rx_complete);
		return 0;
	}

	time_left = wait_for_completion_timeout(&dspi->dma->cmd_tx_complete,
						DMA_COMPLETION_TIMEOUT);
	if (time_left == 0) {
		dev_err(dev, "DMA tx timeout\n");
		dmaengine_terminate_all(dma->chan_tx);
		dmaengine_terminate_all(dma->chan_rx);
		return -ETIMEDOUT;
	}

	time_left = wait_for_completion_timeout(&dspi->dma->cmd_rx_complete,
						DMA_COMPLETION_TIMEOUT);
	if (time_left == 0) {
		dev_err(dev, "DMA rx timeout\n");
		dmaengine_terminate_all(dma->chan_tx);
		dmaengine_terminate_all(dma->chan_rx);
		return -ETIMEDOUT;
	}

	return 0;
}

static int dspi_dma_xfer(struct fsl_dspi *dspi)
{
	struct spi_message *message = dspi->cur_msg;
	struct device *dev = &dspi->pdev->dev;
	struct fsl_dspi_dma *dma = dspi->dma;
	int curr_remaining_bytes;
	int bytes_per_buffer;
	int ret = 0;

	curr_remaining_bytes = dspi->len;
	bytes_per_buffer = DSPI_DMA_BUFSIZE(dspi) / DSPI_FIFO_SIZE;
	while (curr_remaining_bytes) {
		/* Check if current transfer fits the DMA buffer */
		dma->curr_xfer_len = curr_remaining_bytes
			/ dspi->bytes_per_word;
		if (dma->curr_xfer_len > bytes_per_buffer)
			dma->curr_xfer_len = bytes_per_buffer;

		ret = dspi_next_xfer_dma_submit(dspi);
		if (ret) {
			dev_err(dev, "DMA transfer failed\n");
			goto exit;

		} else {
			const int len =
				dma->curr_xfer_len * dspi->bytes_per_word;
			curr_remaining_bytes -= len;
			message->actual_length += len;
			if (curr_remaining_bytes < 0)
				curr_remaining_bytes = 0;
		}
	}

exit:
	return ret;
}

static int dspi_request_dma(struct fsl_dspi *dspi, phys_addr_t phy_addr)
{
	struct device *dev = &dspi->pdev->dev;
	struct dma_slave_config cfg;
	struct fsl_dspi_dma *dma;
	int ret;

	dma = devm_kzalloc(dev, sizeof(*dma), GFP_KERNEL);
	if (!dma)
		return -ENOMEM;

	dma->chan_rx = dma_request_slave_channel(dev, "rx");
	if (!dma->chan_rx) {
		dev_err(dev, "rx dma channel not available\n");
		ret = -ENODEV;
		return ret;
	}

	dma->chan_tx = dma_request_slave_channel(dev, "tx");
	if (!dma->chan_tx) {
		dev_err(dev, "tx dma channel not available\n");
		ret = -ENODEV;
		goto err_tx_channel;
	}

	dma->tx_dma_buf = dma_alloc_coherent(dev, DSPI_DMA_BUFSIZE(dspi),
					     &dma->tx_dma_phys, GFP_KERNEL);
	if (!dma->tx_dma_buf) {
		ret = -ENOMEM;
		goto err_tx_dma_buf;
	}

	dma->rx_dma_buf = dma_alloc_coherent(dev, DSPI_DMA_BUFSIZE(dspi),
					     &dma->rx_dma_phys, GFP_KERNEL);
	if (!dma->rx_dma_buf) {
		ret = -ENOMEM;
		goto err_rx_dma_buf;
	}

	cfg.src_addr = phy_addr + SPI_POPR;
	cfg.dst_addr = phy_addr + SPI_PUSHR;
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	cfg.src_maxburst = 1;
	cfg.dst_maxburst = 1;

	cfg.direction = DMA_DEV_TO_MEM;
	ret = dmaengine_slave_config(dma->chan_rx, &cfg);
	if (ret) {
		dev_err(dev, "can't configure rx dma channel\n");
		ret = -EINVAL;
		goto err_slave_config;
	}

	cfg.direction = DMA_MEM_TO_DEV;
	ret = dmaengine_slave_config(dma->chan_tx, &cfg);
	if (ret) {
		dev_err(dev, "can't configure tx dma channel\n");
		ret = -EINVAL;
		goto err_slave_config;
	}

	dspi->dma = dma;
	init_completion(&dma->cmd_tx_complete);
	init_completion(&dma->cmd_rx_complete);

	return 0;

err_slave_config:
	dma_free_coherent(dev, DSPI_DMA_BUFSIZE(dspi),
			dma->rx_dma_buf, dma->rx_dma_phys);
err_rx_dma_buf:
	dma_free_coherent(dev, DSPI_DMA_BUFSIZE(dspi),
			dma->tx_dma_buf, dma->tx_dma_phys);
err_tx_dma_buf:
	dma_release_channel(dma->chan_tx);
err_tx_channel:
	dma_release_channel(dma->chan_rx);

	devm_kfree(dev, dma);
	dspi->dma = NULL;

	return ret;
}

static void dspi_release_dma(struct fsl_dspi *dspi)
{
	struct fsl_dspi_dma *dma = dspi->dma;
	struct device *dev = &dspi->pdev->dev;

	if (!dma)
		return;

	if (dma->chan_tx) {
		dma_unmap_single(dev, dma->tx_dma_phys,
					DSPI_DMA_BUFSIZE(dspi), DMA_TO_DEVICE);
		dma_release_channel(dma->chan_tx);
	}

	if (dma->chan_rx) {
		dma_unmap_single(dev, dma->rx_dma_phys,
				 DSPI_DMA_BUFSIZE(dspi), DMA_FROM_DEVICE);
		dma_release_channel(dma->chan_rx);
	}
}

static void hz_to_spi_baud(char *pbr, char *br, int speed_hz,
			   unsigned long clkrate)
{
	/* Valid baud rate pre-scaler values */
	int pbr_tbl[4] = {2, 3, 5, 7};
	int brs[16] = {	2,	4,	6,	8,
			16,	32,	64,	128,
			256,	512,	1024,	2048,
			4096,	8192,	16384,	32768 };
	int scale_needed, scale, minscale = INT_MAX;
	int i, j;

	scale_needed = clkrate / speed_hz;
	if (clkrate % speed_hz)
		scale_needed++;

	for (i = 0; i < ARRAY_SIZE(brs); i++)
		for (j = 0; j < ARRAY_SIZE(pbr_tbl); j++) {
			scale = brs[i] * pbr_tbl[j];
			if (scale >= scale_needed) {
				if (scale < minscale) {
					minscale = scale;
					*br = i;
					*pbr = j;
				}
				break;
			}
		}

	if (minscale == INT_MAX) {
		pr_warn("Can not find valid baud rate,speed_hz is %d,clkrate is %ld, we use the max prescaler value.\n",
			speed_hz, clkrate);
		*pbr = ARRAY_SIZE(pbr_tbl) - 1;
		*br =  ARRAY_SIZE(brs) - 1;
	}
}

static void ns_delay_scale(char *psc, char *sc, int delay_ns,
			   unsigned long clkrate)
{
	int scale_needed, scale, minscale = INT_MAX;
	int pscale_tbl[4] = {1, 3, 5, 7};
	u32 remainder;
	int i, j;

	scale_needed = div_u64_rem((u64)delay_ns * clkrate, NSEC_PER_SEC,
				   &remainder);
	if (remainder)
		scale_needed++;

	for (i = 0; i < ARRAY_SIZE(pscale_tbl); i++)
		for (j = 0; j <= SPI_CTAR_SCALE_BITS; j++) {
			scale = pscale_tbl[i] * (2 << j);
			if (scale >= scale_needed) {
				if (scale < minscale) {
					minscale = scale;
					*psc = i;
					*sc = j;
				}
				break;
			}
		}

	if (minscale == INT_MAX) {
		pr_warn("Cannot find correct scale values for %dns delay at clkrate %ld, using max prescaler value",
			delay_ns, clkrate);
		*psc = ARRAY_SIZE(pscale_tbl) - 1;
		*sc = SPI_CTAR_SCALE_BITS;
	}
}

static u32 dspi_data_to_pushr(struct fsl_dspi *dspi, int tx_word)
{
	u16 data, cmd;

	if (!(dspi->dataflags & TRAN_STATE_TX_VOID))
		data = tx_word ? *(u16 *)dspi->tx : *(u8 *)dspi->tx;
	else
		data = dspi->void_write_data;

	dspi->tx += tx_word + 1;
	dspi->len -= tx_word + 1;

	cmd = dspi->tx_cmd;
	if (dspi->len > 0)
		cmd |= SPI_PUSHR_CMD_CONT;

	return (cmd << 16) | SPI_PUSHR_TXDATA(data);
}



static void dspi_data_from_popr(struct fsl_dspi *dspi,
				enum frame_mode rx_frame_mode)
{
	u32 rxdata;

	regmap_read(dspi->regmap, SPI_POPR, &rxdata);

	switch (rx_frame_mode) {
	case FM_BYTES_4:
		if (!(dspi->dataflags & TRAN_STATE_RX_VOID))
			*(u32 *)dspi->rx = SPI_POPR_RXDATA_32(rxdata);
		break;
	case FM_BYTES_2:
		if (!(dspi->dataflags & TRAN_STATE_RX_VOID))
			*(u16 *)dspi->rx = SPI_POPR_RXDATA_16(rxdata);
		break;
	default:
		if (!(dspi->dataflags & TRAN_STATE_RX_VOID))
			*(u8 *)dspi->rx = SPI_POPR_RXDATA_8(rxdata);
		break;
	}

	dspi->rx += bytes_per_frame(rx_frame_mode);
}

static int dspi_eoq_write(struct fsl_dspi *dspi)
{
	int first = 1;
	size_t initial_len = dspi->len;
	unsigned int fifo_entries_used = 0;
	unsigned int fifo_entries_per_frm = 0;
	unsigned int tx_frames_count = 0;
	u16 xfer_cmd = dspi->tx_cmd;
	u32 dspi_pushr = 0;
	enum frame_mode tx_frame_mode = get_frame_mode(dspi);

	fifo_entries_per_frm = (tx_frame_mode == FM_BYTES_4) ? 2 : 1;

	while (dspi->len &&
	       dspi->fifo_size - fifo_entries_used >= fifo_entries_per_frm) {

		dspi->tx_cmd = xfer_cmd;
		switch (tx_frame_mode) {
		case FM_BYTES_4:
			fifo_entries_used++;
			/* Fall through and prepare the register to push the
			 * least significant 16 bits only. We'll push the other
			 * 16 bits after we have written to the CMD-FIFO.
			 */
		case FM_BYTES_2:
			dspi_pushr = dspi_data_to_pushr(dspi, 1);
			break;

		default:
			dspi_pushr = dspi_data_to_pushr(dspi, 0);
			break;
		}

		fifo_entries_used++;
		tx_frames_count++;

		if (dspi->len == 0 ||
		    dspi->fifo_size - fifo_entries_used <
		    fifo_entries_per_frm) {

			/* last transfer in the transfer */
			dspi_pushr |= SPI_PUSHR_EOQ;
			dspi->queue_size = tx_frames_count;

		} else if ((tx_frame_mode == FM_BYTES_2 && dspi->len == 1) ||
			   (tx_frame_mode == FM_BYTES_4 && dspi->len < 4)) {
			dspi_pushr |= SPI_PUSHR_EOQ;
			dspi->queue_size = tx_frames_count;
		}

		if (first) {
			first = 0;
			dspi_pushr |= SPI_PUSHR_CTCNT; /* clear counter */
		}

		regmap_write(dspi->regmap, SPI_PUSHR, dspi_pushr);

		if (tx_frame_mode == FM_BYTES_4) {

			/* regmap does not seem to support 16-bit write access
			 * to 32-bit registers.
			 * This currently applies only to S32V234 SPI, which is
			 * known to be little-endian.
			 */

			dspi_pushr = dspi_data_to_pushr(dspi, 1);
			/* Only write the TXDATA part of the register */
			writew(SPI_PUSHR_TXDATA(dspi_pushr),
			       dspi->base + SPI_PUSHR);
		}
	}

	return initial_len - dspi->len;
}

static int dspi_eoq_read(struct fsl_dspi *dspi)
{
	enum frame_mode rx_frame_mode = get_frame_mode(dspi);
	unsigned int rx_bytes_count = 0;
	unsigned int rx_frames_count = 0;

	while (dspi->rx < dspi->rx_end &&
	       rx_frames_count < dspi->queue_size) {
		dspi_data_from_popr(dspi, rx_frame_mode);
		rx_bytes_count += bytes_per_frame(rx_frame_mode);
		rx_frames_count++;
	}

	return rx_bytes_count;
}

static void fifo_write(struct fsl_dspi *dspi)
{
	regmap_write(dspi->regmap, SPI_PUSHR, dspi_pop_tx_pushr(dspi));
}

static void cmd_fifo_write(struct fsl_dspi *dspi)
{
	u16 cmd = dspi->tx_cmd;

	if (dspi->len > 0)
		cmd |= SPI_PUSHR_CMD_CONT;
	regmap_write(dspi->regmap_pushr, PUSHR_CMD, cmd);
}

static void tx_fifo_write(struct fsl_dspi *dspi, u16 txdata)
{
	regmap_write(dspi->regmap_pushr, PUSHR_TX, txdata);
}

static void dspi_tcfq_write(struct fsl_dspi *dspi)
{
	/* Clear transfer count */
	dspi->tx_cmd |= SPI_PUSHR_CMD_CTCNT;

	if (dspi->devtype_data->xspi_mode && dspi->bits_per_word > 16) {
		/* Write the CMD FIFO entry first, and then the two
		 * corresponding TX FIFO entries.
		 */
		u32 data = dspi_pop_tx(dspi);

		cmd_fifo_write(dspi);
		tx_fifo_write(dspi, data & 0xFFFF);
		tx_fifo_write(dspi, data >> 16);
	} else {
		/* Write one entry to both TX FIFO and CMD FIFO
		 * simultaneously.
		 */
		fifo_write(dspi);
	}
}

static u32 fifo_read(struct fsl_dspi *dspi)
{
	u32 rxdata = 0;

	regmap_read(dspi->regmap, SPI_POPR, &rxdata);
	return rxdata;
}

static void dspi_tcfq_read(struct fsl_dspi *dspi)
{
	dspi_push_rx(dspi, fifo_read(dspi));
}
static int dspi_rxtx(struct fsl_dspi *dspi)
{
	struct spi_message *msg = dspi->cur_msg;
	enum dspi_trans_mode trans_mode;
	u16 spi_tcnt;
	u32 spi_tcr;

	/* Get transfer counter (in number of SPI transfers). It was
	 * reset to 0 when transfer(s) were started.
	 */
	regmap_read(dspi->regmap, SPI_TCR, &spi_tcr);
	spi_tcnt = SPI_TCR_GET_TCNT(spi_tcr);
	/* Update total number of bytes that were transferred */
	msg->actual_length += spi_tcnt * dspi->bytes_per_word;

	trans_mode = dspi->devtype_data->trans_mode;
	if (trans_mode == DSPI_EOQ_MODE)
		dspi_eoq_read(dspi);
	else if (trans_mode == DSPI_TCFQ_MODE)
		dspi_tcfq_read(dspi);

	if (!dspi->len)
		/* Success! */
		return 0;

	if (trans_mode == DSPI_EOQ_MODE)
		dspi_eoq_write(dspi);
	else if (trans_mode == DSPI_TCFQ_MODE)
		dspi_tcfq_write(dspi);

	return -EINPROGRESS;
}

static int dspi_poll(struct fsl_dspi *dspi)
{
	int tries = 1000;
	u32 spi_sr;

	do {
		regmap_read(dspi->regmap, SPI_SR, &spi_sr);
		regmap_write(dspi->regmap, SPI_SR, spi_sr);

		if (spi_sr & (SPI_SR_EOQF | SPI_SR_TCFQF))
			break;
	} while (--tries);

	if (!tries)
		return -ETIMEDOUT;

	return dspi_rxtx(dspi);
}

static irqreturn_t dspi_interrupt(int irq, void *dev_id)
{
	struct fsl_dspi *dspi = (struct fsl_dspi *)dev_id;
	u32 spi_sr;

	regmap_read(dspi->regmap, SPI_SR, &spi_sr);
	regmap_write(dspi->regmap, SPI_SR, spi_sr);

	if (!(spi_sr & (SPI_SR_EOQF | SPI_SR_TCFQF)))
		return IRQ_NONE;

	if (dspi_rxtx(dspi) == 0) {
		dspi->waitflags = 1;
		wake_up_interruptible(&dspi->waitq);
	}

	return IRQ_HANDLED;
}

static int dspi_transfer_one_message(struct spi_controller *ctlr,
				     struct spi_message *message)
{
	struct fsl_dspi *dspi = spi_controller_get_devdata(ctlr);
	struct spi_device *spi = message->spi;
	enum dspi_trans_mode trans_mode;
	struct spi_transfer *transfer;
	int status = 0;
	unsigned int val;

	message->actual_length = 0;

	list_for_each_entry(transfer, &message->transfers, transfer_list) {
		dspi->cur_transfer = transfer;
		dspi->cur_msg = message;
		dspi->cur_chip = spi_get_ctldata(spi);
		/* Prepare command word for CMD FIFO */
		dspi->tx_cmd = SPI_PUSHR_CMD_CTAS(0) |
			       SPI_PUSHR_CMD_PCS(spi->chip_select,
						 dspi->pcs_mask);
		if (list_is_last(&dspi->cur_transfer->transfer_list,
				 &dspi->cur_msg->transfers)) {
			/* Leave PCS activated after last transfer when
			 * cs_change is set.
			 */
			if (transfer->cs_change)
				dspi->tx_cmd |= SPI_PUSHR_CMD_CONT;
		} else {
			/* Keep PCS active between transfers in same message
			 * when cs_change is not set, and de-activate PCS
			 * between transfers in the same message when
			 * cs_change is set.
			 */
			if (!transfer->cs_change)
				dspi->tx_cmd |= SPI_PUSHR_CMD_CONT;
		}

		dspi->void_write_data = dspi->cur_chip->void_write_data;

		dspi->dataflags = 0;
		dspi->tx = (void *)transfer->tx_buf;
		dspi->tx_end = dspi->tx + transfer->len;
		dspi->rx = transfer->rx_buf;
		dspi->rx_end = dspi->rx + transfer->len;
		dspi->len = transfer->len;

		if (!dspi->rx)
			dspi->dataflags |= TRAN_STATE_RX_VOID;

		if (!dspi->tx)
			dspi->dataflags |= TRAN_STATE_TX_VOID;

		/* Put DSPI in stopped mode. */
		regmap_update_bits(dspi->regmap, SPI_MCR,
				SPI_MCR_HALT, SPI_MCR_HALT);
		while (regmap_read(dspi->regmap, SPI_SR, &val) >= 0 &&
				val & SPI_SR_TXRXS)
			;

		regmap_write(dspi->regmap, SPI_CTAR(0),
				dspi->cur_chip->ctar_val);

		if (dspi->cur_chip->mcr_val & SPI_MCR_XSPI)
			regmap_write(dspi->regmap, SPI_CTARE(0),
				     dspi->cur_chip->ctare_val);

		trans_mode = dspi->devtype_data->trans_mode;
		switch (trans_mode) {
		case DSPI_EOQ_MODE:
			regmap_write(dspi->regmap, SPI_RSER, SPI_RSER_EOQFE);
			regmap_write(dspi->regmap, SPI_MCR,
				     dspi->cur_chip->mcr_val |
				     SPI_MCR_CLR_TXF | SPI_MCR_CLR_RXF);
			dspi_eoq_write(dspi);
			break;
		case DSPI_TCFQ_MODE:
			regmap_write(dspi->regmap, SPI_RSER, SPI_RSER_TCFQE);
			regmap_write(dspi->regmap, SPI_MCR,
				     dspi->cur_chip->mcr_val |
				     SPI_MCR_CLR_TXF | SPI_MCR_CLR_RXF);
			dspi_tcfq_write(dspi);
			break;
		case DSPI_DMA_MODE:
			regmap_write(dspi->regmap, SPI_RSER,
				     SPI_RSER_TFFFE | SPI_RSER_TFFFD |
				     SPI_RSER_RFDFE | SPI_RSER_RFDFD);
			regmap_write(dspi->regmap, SPI_MCR,
				     dspi->cur_chip->mcr_val |
				     SPI_MCR_CLR_TXF | SPI_MCR_CLR_RXF);
			status = dspi_dma_xfer(dspi);
			break;
		default:
			dev_err(&dspi->pdev->dev, "unsupported trans_mode %u\n",
				trans_mode);
			status = -EINVAL;
			goto out;
		}

		if (!dspi->irq) {
			do {
				status = dspi_poll(dspi);
			} while (status == -EINPROGRESS);
		} else if (trans_mode != DSPI_DMA_MODE) {
			status = wait_event_interruptible(dspi->waitq,
							  dspi->waitflags);
			dspi->waitflags = 0;
		}
		if (status)
			dev_err(&dspi->pdev->dev,
				"Waiting for transfer to complete failed!\n");

		if (transfer->delay_usecs)
			udelay(transfer->delay_usecs);
	}

out:
	message->status = status;
	spi_finalize_current_message(ctlr);

	return status;
}

static int dspi_setup(struct spi_device *spi)
{
	struct fsl_dspi *dspi = spi_controller_get_devdata(spi->controller);
	unsigned char br = 0, pbr = 0, pcssck = 0, cssck = 0;
	u32 cs_sck_delay = 0, sck_cs_delay = 0;
	struct fsl_dspi_platform_data *pdata;
	unsigned char pasc = 0, asc = 0, fmsz = 0;
	struct chip_data *chip;
	unsigned long clkrate;

	if ((spi->bits_per_word >= 4 && spi->bits_per_word <= 16) ||
	    (dspi->extended_mode && spi->bits_per_word <= 32)) {
		fmsz = spi->bits_per_word - 1;
	} else {
		pr_err("Invalid wordsize\n");
		return -ENODEV;
	}

	/* Only alloc on first setup */
	chip = spi_get_ctldata(spi);
	if (chip == NULL) {
		chip = kzalloc(sizeof(struct chip_data), GFP_KERNEL);
		if (!chip)
			return -ENOMEM;
	}

	pdata = dev_get_platdata(&dspi->pdev->dev);

	if (!pdata) {
		of_property_read_u32(spi->dev.of_node, "fsl,spi-cs-sck-delay",
				     &cs_sck_delay);

		of_property_read_u32(spi->dev.of_node, "fsl,spi-sck-cs-delay",
				     &sck_cs_delay);
	} else {
		cs_sck_delay = pdata->cs_sck_delay;
		sck_cs_delay = pdata->sck_cs_delay;
	}

	chip->mcr_val = SPI_MCR_MASTER | SPI_MCR_PCSIS(dspi->pcs_mask) |
 		SPI_MCR_CLR_TXF | SPI_MCR_CLR_RXF;
	chip->void_write_data = 0;

	clkrate = clk_get_rate(dspi->clk);
	hz_to_spi_baud(&pbr, &br, spi->max_speed_hz, clkrate);

	/* Set PCS to SCK delay scale values */
	ns_delay_scale(&pcssck, &cssck, cs_sck_delay, clkrate);

	/* Set After SCK delay scale values */
	ns_delay_scale(&pasc, &asc, sck_cs_delay, clkrate);

	chip->ctar_val = SPI_CTAR_FMSZ(fmsz)
		| SPI_CTAR_CPOL(spi->mode & SPI_CPOL ? 1 : 0)
		| SPI_CTAR_CPHA(spi->mode & SPI_CPHA ? 1 : 0);

	if (!spi_controller_is_slave(dspi->ctlr)) {
		chip->ctar_val |= SPI_CTAR_PCSSCK(pcssck) |
				  SPI_CTAR_CSSCK(cssck) |
				  SPI_CTAR_PASC(pasc) |
				  SPI_CTAR_ASC(asc) |
				  SPI_CTAR_PBR(pbr) |
				  SPI_CTAR_BR(br);

		chip->ctar_val |= SPI_CTAR_LSBFE(spi->mode &
						 SPI_LSB_FIRST ? 1 : 0);
	}

	spi_set_ctldata(spi, chip);

	if (dspi->extended_mode && fmsz >= 16) {
		chip->mcr_val |= SPI_MCR_XSPI;

		/* Support for multiple data frames with a single command frame
		 * not yet implemented: SPI_CTAREn[DTCP] is left to the default
		 * value, 1.
		 */
		chip->ctare_val = SPI_CTARE_FMSZE(fmsz) | SPI_CTARE_DTCP(1);
	}

	return 0;
}

static void dspi_cleanup(struct spi_device *spi)
{
	struct chip_data *chip = spi_get_ctldata((struct spi_device *)spi);

	dev_dbg(&spi->dev, "spi_device %u.%u cleanup\n",
		spi->controller->bus_num, spi->chip_select);

	kfree(chip);
}

static const struct of_device_id fsl_dspi_dt_ids[] = {
	{ .compatible = "fsl,vf610-dspi", .data = &vf610_data, },
	{ .compatible = "fsl,ls1021a-v1.0-dspi", .data = &ls1021a_v1_data, },
	{ .compatible = "fsl,ls2085a-dspi", .data = &ls2085a_data, },
	{ .compatible = "fsl,s32v234-dspi", .data = &s32_data, },
	{ .compatible = "fsl,s32gen1-dspi", .data = &s32_data, },
	{ .compatible = "fsl,s32r45x-dspi", .data = &s32r45_data, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, fsl_dspi_dt_ids);

#ifdef CONFIG_PM_SLEEP
static int dspi_suspend(struct device *dev)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct fsl_dspi *dspi = spi_controller_get_devdata(ctlr);

	if (dspi->irq)
		disable_irq(dspi->irq);
	spi_controller_suspend(ctlr);
	clk_disable_unprepare(dspi->clk);

	pinctrl_pm_select_sleep_state(dev);

	return 0;
}

static int dspi_resume(struct device *dev)
{
	struct spi_controller *ctlr = dev_get_drvdata(dev);
	struct fsl_dspi *dspi = spi_controller_get_devdata(ctlr);
	int ret;

	pinctrl_pm_select_default_state(dev);

	ret = clk_prepare_enable(dspi->clk);
	if (ret)
		return ret;
	spi_controller_resume(ctlr);
	if (dspi->irq)
		enable_irq(dspi->irq);

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static SIMPLE_DEV_PM_OPS(dspi_pm, dspi_suspend, dspi_resume);

static const struct regmap_range dspi_volatile_ranges[] = {
	regmap_reg_range(SPI_MCR, SPI_TCR),
	regmap_reg_range(SPI_SR, SPI_SR),
	regmap_reg_range(SPI_PUSHR, SPI_RXFR(0x83)),
};

static const struct regmap_access_table dspi_volatile_table = {
	.yes_ranges	= dspi_volatile_ranges,
	.n_yes_ranges	= ARRAY_SIZE(dspi_volatile_ranges),
};

static struct regmap_config dspi_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.volatile_table	= &dspi_volatile_table,
};

static const struct regmap_range dspi_xspi_volatile_ranges[] = {
	regmap_reg_range(SPI_MCR, SPI_TCR),
	regmap_reg_range(SPI_SR, SPI_SR),
	regmap_reg_range(SPI_PUSHR, SPI_RXFR(0x83)),
	regmap_reg_range(SPI_SREX, SPI_SREX),
};

static const struct regmap_access_table dspi_xspi_volatile_table = {
	.yes_ranges	= dspi_xspi_volatile_ranges,
	.n_yes_ranges	= ARRAY_SIZE(dspi_xspi_volatile_ranges),
};

static const struct regmap_config dspi_xspi_regmap_config[] = {
	{
		.reg_bits	= 32,
		.val_bits	= 32,
		.reg_stride	= 4,
		.max_register	= 0x13c,
		.volatile_table	= &dspi_xspi_volatile_table,
	},
	{
		.name		= "pushr",
		.reg_bits	= 16,
		.val_bits	= 16,
		.reg_stride	= 2,
		.max_register	= 0x2,
	},
};

static void dspi_init(struct fsl_dspi *dspi)
{
	unsigned int mcr = SPI_MCR_PCSIS(0x3F);

	if (dspi->devtype_data->xspi_mode)
		mcr |= SPI_MCR_XSPI;
	if (!spi_controller_is_slave(dspi->ctlr))
		mcr |= SPI_MCR_MASTER;

	regmap_write(dspi->regmap, SPI_MCR, mcr);
	regmap_write(dspi->regmap, SPI_SR, SPI_SR_CLEAR);
}

static int dspi_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct fsl_dspi_platform_data *pdata;
	struct spi_controller *ctlr;
	int ret, cs_num, bus_num;
	struct fsl_dspi *dspi;
	struct resource *res;
	u32 val;

	ctlr = spi_alloc_master(&pdev->dev, sizeof(struct fsl_dspi));
	if (!ctlr)
		return -ENOMEM;

	dspi = spi_controller_get_devdata(ctlr);
	dspi->pdev = pdev;
	dspi->ctlr = ctlr;

	ctlr->setup = dspi_setup;
	ctlr->transfer_one_message = dspi_transfer_one_message;
	ctlr->dev.of_node = pdev->dev.of_node;

	ctlr->cleanup = dspi_cleanup;
	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_LSB_FIRST;
	ctlr->bits_per_word_mask = SPI_BPW_RANGE_MASK(4, 32);

	ret = of_property_read_u32(np, "spi-num-chipselects", &cs_num);
	if (ret < 0) {
		dev_err(&pdev->dev, "can't get spi-num-chipselects\n");
		goto out_ctlr_put;
	}
	ctlr->num_chipselect = cs_num;
	dspi->pcs_mask = (1 << cs_num) - 1;
	pdata = dev_get_platdata(&pdev->dev);
	if (pdata) {
		ctlr->num_chipselect = pdata->cs_num;
		ctlr->bus_num = pdata->bus_num;

		dspi->devtype_data = &coldfire_data;
	} else {

		ret = of_property_read_u32(np, "spi-num-chipselects", &cs_num);
		if (ret < 0) {
			dev_err(&pdev->dev, "can't get spi-num-chipselects\n");
			goto out_ctlr_put;
		}
		ctlr->num_chipselect = cs_num;

		ret = of_property_read_u32(np, "bus-num", &bus_num);
		if (ret < 0) {
			dev_err(&pdev->dev, "can't get bus-num\n");
			goto out_ctlr_put;
		}
		ctlr->bus_num = bus_num;

		if (of_property_read_bool(np, "spi-slave"))
			ctlr->slave = true;

		dspi->devtype_data = of_device_get_match_data(&pdev->dev);
		if (!dspi->devtype_data) {
			dev_err(&pdev->dev, "can't get devtype_data\n");
			ret = -EFAULT;
			goto out_ctlr_put;
		}
	}

	ret = of_property_read_u32(np, "spi-fifo-size", &val);
	if (ret < 0)
		dspi->fifo_size = DSPI_FIFO_SIZE;
	else
		dspi->fifo_size = val;

	dspi->extended_mode = of_property_read_bool(np, "spi-extended-mode");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	dspi->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(dspi->base)) {
		ret = PTR_ERR(dspi->base);
		goto out_ctlr_put;
	}

	if (dspi->extended_mode)
		dspi_regmap_config.max_register = SPI_SREX;
	else
		dspi_regmap_config.max_register = SPI_RXFR(dspi->fifo_size - 1);
	dspi->regmap = devm_regmap_init_mmio_clk(&pdev->dev, NULL, dspi->base,
						&dspi_regmap_config);
	if (IS_ERR(dspi->regmap)) {
		dev_err(&pdev->dev, "failed to init regmap: %ld\n",
				PTR_ERR(dspi->regmap));
		ret = PTR_ERR(dspi->regmap);
		goto out_ctlr_put;
	}

	if (dspi->devtype_data->xspi_mode) {
		dspi->regmap_pushr = devm_regmap_init_mmio(
			&pdev->dev, dspi->base + SPI_PUSHR,
			&dspi_xspi_regmap_config[1]);
		if (IS_ERR(dspi->regmap_pushr)) {
			dev_err(&pdev->dev,
				"failed to init pushr regmap: %ld\n",
				PTR_ERR(dspi->regmap_pushr));
			ret = PTR_ERR(dspi->regmap_pushr);
			goto out_ctlr_put;
		}
	}

	dspi->clk = devm_clk_get(&pdev->dev, "dspi");
	if (IS_ERR(dspi->clk)) {
		ret = PTR_ERR(dspi->clk);
		dev_err(&pdev->dev, "unable to get clock\n");
		goto out_ctlr_put;
	}
	ret = clk_prepare_enable(dspi->clk);
	if (ret)
		goto out_ctlr_put;

	dspi_init(dspi);

	dspi->irq = platform_get_irq(pdev, 0);
	if (dspi->irq <= 0) {
		dev_info(&pdev->dev,
			 "can't get platform irq, using poll mode\n");
		dspi->irq = 0;
		goto poll_mode;
	}

	ret = request_threaded_irq(dspi->irq, dspi_interrupt, NULL,
				   IRQF_SHARED, pdev->name, dspi);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to attach DSPI interrupt\n");
		goto out_clk_put;
	}

	init_waitqueue_head(&dspi->waitq);

poll_mode:
	if (dspi->devtype_data->trans_mode == DSPI_DMA_MODE) {
		ret = dspi_request_dma(dspi, res->start);
		if (ret < 0) {
			dev_err(&pdev->dev, "can't get dma channels\n");
			goto out_free_irq;
		}
	}

	ctlr->max_speed_hz =
		clk_get_rate(dspi->clk) / dspi->devtype_data->max_clock_factor;

	platform_set_drvdata(pdev, ctlr);

	ret = spi_register_controller(ctlr);
	if (ret != 0) {
		dev_err(&pdev->dev, "Problem registering DSPI ctlr\n");
		goto out_free_irq;
	}

	return ret;

out_free_irq:
	if (dspi->irq)
		free_irq(dspi->irq, dspi);
out_clk_put:
	clk_disable_unprepare(dspi->clk);
out_ctlr_put:
	spi_controller_put(ctlr);

	return ret;
}

static int dspi_remove(struct platform_device *pdev)
{
	struct spi_controller *ctlr = platform_get_drvdata(pdev);
	struct fsl_dspi *dspi = spi_controller_get_devdata(ctlr);

	/* Disconnect from the SPI framework */
	spi_unregister_controller(dspi->ctlr);

	/* Disable RX and TX */
	regmap_update_bits(dspi->regmap, SPI_MCR,
			   SPI_MCR_DIS_TXF | SPI_MCR_DIS_RXF,
			   SPI_MCR_DIS_TXF | SPI_MCR_DIS_RXF);

	/* Stop Running */
	regmap_update_bits(dspi->regmap, SPI_MCR, SPI_MCR_HALT, SPI_MCR_HALT);

	dspi_release_dma(dspi);
	if (dspi->irq)
		free_irq(dspi->irq, dspi);
	clk_disable_unprepare(dspi->clk);

	return 0;
}

static void dspi_shutdown(struct platform_device *pdev)
{
	dspi_remove(pdev);
}

static struct platform_driver fsl_dspi_driver = {
	.driver.name		= DRIVER_NAME,
	.driver.of_match_table	= fsl_dspi_dt_ids,
	.driver.owner		= THIS_MODULE,
	.driver.pm		= &dspi_pm,
	.probe			= dspi_probe,
	.remove			= dspi_remove,
	.shutdown		= dspi_shutdown,
};
module_platform_driver(fsl_dspi_driver);

MODULE_DESCRIPTION("Freescale DSPI Controller Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
