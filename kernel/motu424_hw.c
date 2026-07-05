// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * motu424_hw.c - the hardware abstraction layer.
 *
 * =====================================================================
 * THIS IS THE ONLY FILE THAT ENCODES REAL MOTU PCI-324/424 SEMANTICS.
 * =====================================================================
 *
 * The register map in motu424.h is a documented hypothesis. Every function
 * here is written so that, once the true register layout / DMA protocol is
 * confirmed on real hardware, correcting the constants and the bodies below is
 * sufficient to make the driver produce audio - no changes are needed in the
 * PCI or PCM layers.
 *
 * Confirmation workflow:
 *   1. Run tools/motu424-probe.c to dump BAR0 and observe which registers
 *      change while the vendor driver (or macOS) streams audio.
 *   2. Trace DMA descriptors / the ring format.
 *   3. Update the offsets/bits in motu424.h and the encoders here.
 */
#include <linux/delay.h>
#include <linux/io.h>

#include "motu424.h"

/* --- raw register accessors (little-endian MMIO) --- */
static inline u32 motu424_read(struct motu424 *chip, unsigned int reg)
{
	return ioread32(chip->mmio + reg);
}

static inline void motu424_write(struct motu424 *chip, unsigned int reg,
				 u32 val)
{
	iowrite32(val, chip->mmio + reg);
}

static inline void motu424_update(struct motu424 *chip, unsigned int reg,
				  u32 mask, u32 val)
{
	u32 v = motu424_read(chip, reg);

	v = (v & ~mask) | (val & mask);
	motu424_write(chip, reg, v);
}

/*
 * Bring the card to a known idle state: soft reset, all DMA engines and
 * interrupts disabled, interrupt sources cleared.
 */
int motu424_hw_init(struct motu424 *chip)
{
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);

	motu424_write(chip, MOTU424_REG_CONTROL, MOTU424_CTRL_RESET);
	/* Give the FPGA time to come out of reset. */
	udelay(50);
	motu424_write(chip, MOTU424_REG_CONTROL, 0);
	motu424_write(chip, MOTU424_REG_IRQ_MASK, 0);
	motu424_write(chip, MOTU424_REG_IRQ_STATUS, MOTU424_IRQ_MASK_ALL);

	spin_unlock_irqrestore(&chip->lock, flags);

	chip->rate = 48000;
	chip->channels = MOTU424_MAX_CHANNELS;
	return 0;
}

void motu424_hw_shutdown(struct motu424 *chip)
{
	unsigned long flags;

	if (!chip->mmio)
		return;

	spin_lock_irqsave(&chip->lock, flags);
	motu424_write(chip, MOTU424_REG_IRQ_MASK, 0);
	motu424_write(chip, MOTU424_REG_CONTROL, 0);
	motu424_write(chip, MOTU424_REG_IRQ_STATUS, MOTU424_IRQ_MASK_ALL);
	spin_unlock_irqrestore(&chip->lock, flags);
}

/*
 * Translate a sample rate into the card's clock-family encoding. The PCI-424
 * exposes a fixed frame whose channel count halves per doubling of the rate;
 * callers query chip->channels after a successful call.
 */
int motu424_hw_set_rate(struct motu424 *chip, unsigned int rate)
{
	unsigned long flags;
	u32 clk;

	switch (rate) {
	case 44100:
		clk = MOTU424_CLK_INTERNAL | MOTU424_CLK_RATE_1X |
		      MOTU424_CLK_BASE_44100;
		break;
	case 48000:
		clk = MOTU424_CLK_INTERNAL | MOTU424_CLK_RATE_1X;
		break;
	case 88200:
		clk = MOTU424_CLK_INTERNAL | MOTU424_CLK_RATE_2X |
		      MOTU424_CLK_BASE_44100;
		break;
	case 96000:
		clk = MOTU424_CLK_INTERNAL | MOTU424_CLK_RATE_2X;
		break;
	case 176400:
		clk = MOTU424_CLK_INTERNAL | MOTU424_CLK_RATE_4X |
		      MOTU424_CLK_BASE_44100;
		break;
	case 192000:
		clk = MOTU424_CLK_INTERNAL | MOTU424_CLK_RATE_4X;
		break;
	default:
		return -EINVAL;
	}

	spin_lock_irqsave(&chip->lock, flags);
	motu424_write(chip, MOTU424_REG_CLOCK, clk);
	spin_unlock_irqrestore(&chip->lock, flags);

	chip->rate = rate;
	return 0;
}

/*
 * Program a stream's DMA ring base, total size and period (IRQ) size into the
 * card. Called from the PCM prepare callback with the DMA buffer already
 * allocated by ALSA (snd_pcm_set_managed_buffer_all).
 */
int motu424_hw_stream_prepare(struct motu424 *chip,
			      struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	bool playback = MOTU424_STREAM_IS_PLAYBACK(substream);
	struct motu424_stream *s = playback ? &chip->playback : &chip->capture;
	dma_addr_t addr = runtime->dma_addr;
	unsigned long flags;

	s->buffer_bytes = snd_pcm_lib_buffer_bytes(substream);
	s->period_bytes = snd_pcm_lib_period_bytes(substream);
	s->substream = substream;

	spin_lock_irqsave(&chip->lock, flags);
	if (playback) {
		motu424_write(chip, MOTU424_REG_DMA_PLAY, lower_32_bits(addr));
		motu424_write(chip, MOTU424_REG_PLAY_SIZE, s->buffer_bytes);
		motu424_write(chip, MOTU424_REG_PLAY_PERIOD, s->period_bytes);
	} else {
		motu424_write(chip, MOTU424_REG_DMA_REC, lower_32_bits(addr));
		motu424_write(chip, MOTU424_REG_REC_SIZE, s->buffer_bytes);
		motu424_write(chip, MOTU424_REG_REC_PERIOD, s->period_bytes);
	}
	spin_unlock_irqrestore(&chip->lock, flags);
	return 0;
}

/* Enable a DMA engine and unmask its period interrupt. Caller may hold lock? no
 * - this takes the lock itself and is called from the (atomic) trigger op. */
void motu424_hw_stream_start(struct motu424 *chip, bool playback)
{
	struct motu424_stream *s = playback ? &chip->playback : &chip->capture;
	u32 en_bit = playback ? MOTU424_CTRL_PLAY_EN : MOTU424_CTRL_REC_EN;
	u32 irq_bit = playback ? MOTU424_IRQ_PLAY : MOTU424_IRQ_REC;
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	motu424_update(chip, MOTU424_REG_IRQ_MASK, irq_bit, irq_bit);
	motu424_update(chip, MOTU424_REG_CONTROL,
		       en_bit | MOTU424_CTRL_IRQ_EN,
		       en_bit | MOTU424_CTRL_IRQ_EN);
	s->running = true;
	spin_unlock_irqrestore(&chip->lock, flags);
}

void motu424_hw_stream_stop(struct motu424 *chip, bool playback)
{
	struct motu424_stream *s = playback ? &chip->playback : &chip->capture;
	u32 en_bit = playback ? MOTU424_CTRL_PLAY_EN : MOTU424_CTRL_REC_EN;
	u32 irq_bit = playback ? MOTU424_IRQ_PLAY : MOTU424_IRQ_REC;
	unsigned long flags;

	spin_lock_irqsave(&chip->lock, flags);
	motu424_update(chip, MOTU424_REG_CONTROL, en_bit, 0);
	motu424_update(chip, MOTU424_REG_IRQ_MASK, irq_bit, 0);
	s->running = false;
	/* Drop the master IRQ enable once no engine is active. */
	if (!chip->playback.running && !chip->capture.running)
		motu424_update(chip, MOTU424_REG_CONTROL,
			       MOTU424_CTRL_IRQ_EN, 0);
	spin_unlock_irqrestore(&chip->lock, flags);
}

/*
 * Current DMA position in frames within the ring. Read the card's byte-position
 * register and convert using the active frame size.
 */
snd_pcm_uframes_t motu424_hw_stream_pointer(struct motu424 *chip, bool playback)
{
	struct motu424_stream *s = playback ? &chip->playback : &chip->capture;
	unsigned int reg = playback ? MOTU424_REG_DMA_PLAY_POS
				    : MOTU424_REG_DMA_REC_POS;
	u32 pos;

	if (!s->substream || !s->buffer_bytes)
		return 0;

	pos = motu424_read(chip, reg);
	if (pos >= s->buffer_bytes)
		pos %= s->buffer_bytes;

	return bytes_to_frames(s->substream->runtime, pos);
}

/*
 * Read and clear pending interrupt sources. Returns the MOTU424_IRQ_* bitmask
 * of engines that raised a period interrupt (0 if the IRQ was not ours).
 */
u32 motu424_hw_irq_ack(struct motu424 *chip)
{
	unsigned long flags;
	u32 pending;

	spin_lock_irqsave(&chip->lock, flags);
	pending = motu424_read(chip, MOTU424_REG_IRQ_STATUS) &
		  MOTU424_IRQ_MASK_ALL;
	if (pending)			/* write-1-to-clear */
		motu424_write(chip, MOTU424_REG_IRQ_STATUS, pending);
	spin_unlock_irqrestore(&chip->lock, flags);

	return pending;
}
