// SPDX-License-Identifier: GPL-2.0
/*
 * ALSA PCM playback driver for MCP49x1 DACs connected by GPIO bit-bang.
 *
 * Luckfox Pico / RV1103 wiring used by this driver:
 *   GPIO52 -> MCP49x1 CS
 *   GPIO42 -> MCP49x1 SCK
 *   GPIO43 -> MCP49x1 SDI
 *
 * This is intentionally a simple PCM driver, not ASoC and not IIO.
 * It accepts normal ALSA PCM data and converts it in-kernel to the
 * DAC value required by MCP4901/MCP4911/MCP4921.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/math64.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/control.h>

#define DRV_NAME "snd_mcp49x1_gpio"

#define DEFAULT_GPIO_CS   52
#define DEFAULT_GPIO_SCK  42
#define DEFAULT_GPIO_SDI  43

#define MCP49X1_CMD_ACTIVE_BASE 0x3000u
#define MCP49X1_CMD_SHUTDOWN    0x2000u

static int index = SNDRV_DEFAULT_IDX1;
static char *id = SNDRV_DEFAULT_STR1;
static bool enable = true;

module_param(index, int, 0444);
MODULE_PARM_DESC(index, "ALSA card index");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ALSA card ID");
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable, "Enable this sound card");

static int gpio_cs = DEFAULT_GPIO_CS;
static int gpio_sck = DEFAULT_GPIO_SCK;
static int gpio_sdi = DEFAULT_GPIO_SDI;
module_param(gpio_cs, int, 0444);
MODULE_PARM_DESC(gpio_cs, "GPIO number for MCP49x1 CS");
module_param(gpio_sck, int, 0444);
MODULE_PARM_DESC(gpio_sck, "GPIO number for MCP49x1 SCK");
module_param(gpio_sdi, int, 0444);
MODULE_PARM_DESC(gpio_sdi, "GPIO number for MCP49x1 SDI");

static int gain_percent = 150;
module_param(gain_percent, int, 0644);
MODULE_PARM_DESC(gain_percent, "Software gain before DAC conversion, percent");

static bool limiter_enable = true;
module_param(limiter_enable, bool, 0644);
MODULE_PARM_DESC(limiter_enable, "Enable soft limiter before DAC conversion");

static int fade_ms = 24;
module_param(fade_ms, int, 0644);
MODULE_PARM_DESC(fade_ms, "Playback fade-in/fade-out time in ms");

static bool highpass_enable = true;
module_param(highpass_enable, bool, 0644);
MODULE_PARM_DESC(highpass_enable, "Enable simple DC-block/high-pass filter");

static int highpass_q15 = 30000;
module_param(highpass_q15, int, 0644);
MODULE_PARM_DESC(highpass_q15, "High-pass feedback coefficient in Q15; lower cuts more bass, typical 30000..32400");

static bool psycho_bass_enable = true;
module_param(psycho_bass_enable, bool, 0644);
MODULE_PARM_DESC(psycho_bass_enable, "Enable psychoacoustic bass enhancement for small speakers, default 1");

static int psycho_bass_level = 60;
module_param(psycho_bass_level, int, 0644);
MODULE_PARM_DESC(psycho_bass_level, "Psychoacoustic bass amount, percent, typical 30..80, default 60");

static int psycho_bass_shift = 5;
module_param(psycho_bass_shift, int, 0644);
MODULE_PARM_DESC(psycho_bass_shift, "Low-frequency tracking shift for psycho bass; lower reaches higher bass, typical 4..7, default 5");

static int dac_bits = 8;
module_param(dac_bits, int, 0444);
MODULE_PARM_DESC(dac_bits, "DAC resolution: 8 for MCP4901, 10 for MCP4911, 12 for MCP4921; default 8");

/* Stable rates for this GPIO/DAC path. Higher rates were intentionally removed. */
static const unsigned int mcp4901_supported_rates[] = {
	8000, 11025, 16000, 22050,
};

static const struct snd_pcm_hw_constraint_list mcp4901_rate_constraint = {
	.count = ARRAY_SIZE(mcp4901_supported_rates),
	.list = mcp4901_supported_rates,
	.mask = 0,
};

static bool mcp4901_is_supported_rate(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mcp4901_supported_rates); i++) {
		if (mcp4901_supported_rates[i] == rate)
			return true;
	}

	return false;
}

/* Runtime state for one ALSA card instance. */
struct mcp4901_chip {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;
	struct hrtimer timer;
	ktime_t period;
	spinlock_t lock;

	bool running;
	bool shutdown_pending;
	snd_pcm_uframes_t hw_ptr;
	snd_pcm_uframes_t period_pos;
	snd_pcm_uframes_t played_frames;

	unsigned int rate;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t fade_frames;

	s32 hp_prev_x;
	s32 hp_prev_y;
	s32 bass_lp;
	u16 last_code;
	bool dac_active;
	unsigned int dac_bits_cached;
	unsigned int dac_shift_cached;
	unsigned int dac_convert_shift_cached;
	u16 dac_center_cached;
	u16 dac_max_cached;
	unsigned int master_volume;


	void *pcm_area;
	unsigned int pcm_channels;
	snd_pcm_format_t pcm_format;
	bool hp_enabled_cached;
	bool limiter_enabled_cached;
	bool psycho_bass_enabled_cached;
	s32 hp_q15_cached;
	s32 gain_q10_cached;
	s32 psycho_level_q10_cached;
	s32 psycho_shift_cached;


	s32 (*read_sample_cached)(struct mcp4901_chip *chip, snd_pcm_uframes_t frame);
	u16 (*process_sample_cached)(struct mcp4901_chip *chip, s32 x);
};

static struct platform_device *mcp4901_pdev;

/* Rockchip GPIO v2 registers used by the MMIO playback backend. */
#define RK_GPIO_V2_DR_L      0x0000
#define RK_GPIO_V2_DR_H      0x0004
#define RK_GPIO_V2_VER_ID    0x0078

#define GPIO1_BIT_SCK        10
#define GPIO1_BIT_SDI        11
#define GPIO1_BIT_CS         20

#define GPIO1_L_MASK_SCK_SDI (BIT(GPIO1_BIT_SCK + 16) | BIT(GPIO1_BIT_SDI + 16))
#define GPIO1_H_MASK_CS      BIT((GPIO1_BIT_CS - 16) + 16)

static unsigned long mmio_gpio1_base = 0xff530000UL;
module_param(mmio_gpio1_base, ulong, 0444);
MODULE_PARM_DESC(mmio_gpio1_base, "Physical base address of GPIO1 controller; default 0xff530000");

static void __iomem *mmio_gpio1;
static bool mmio_active;

static __always_inline void mmio_hold(void)
{
	cpu_relax();
}

static __always_inline void mmio_write_sck_sdi(int sck, int sdi)
{
	u32 data = 0;

	if (sck)
		data |= BIT(GPIO1_BIT_SCK);
	if (sdi)
		data |= BIT(GPIO1_BIT_SDI);

	writel_relaxed(GPIO1_L_MASK_SCK_SDI | data, mmio_gpio1 + RK_GPIO_V2_DR_L);
}

static __always_inline void mmio_write_cs(int value)
{
	u32 data = value ? BIT(GPIO1_BIT_CS - 16) : 0;

	writel_relaxed(GPIO1_H_MASK_CS | data, mmio_gpio1 + RK_GPIO_V2_DR_H);
}

/* Map GPIO1 and verify that it looks like the expected Rockchip GPIO block. */
static int init_mmio_backend(void)
{
	u32 id;

	mmio_active = false;

	if (gpio_cs != DEFAULT_GPIO_CS || gpio_sck != DEFAULT_GPIO_SCK || gpio_sdi != DEFAULT_GPIO_SDI) {
		pr_err(DRV_NAME ": MMIO-only version supports only default GPIOs CS=52 SCK=42 SDI=43\n");
		return -EINVAL;
	}

	mmio_gpio1 = ioremap(mmio_gpio1_base, 0x100);
	if (!mmio_gpio1) {
		pr_err(DRV_NAME ": ioremap GPIO1 base 0x%lx failed\n", mmio_gpio1_base);
		return -ENOMEM;
	}

	id = readl_relaxed(mmio_gpio1 + RK_GPIO_V2_VER_ID);
	if (id != 0x01000c2b && id != 0x0101157c && id != 0x0101157b) {
		pr_err(DRV_NAME ": GPIO1 MMIO version 0x%08x is not expected Rockchip GPIO v2/v2.1\n", id);
		iounmap(mmio_gpio1);
		mmio_gpio1 = NULL;
		return -ENODEV;
	}

	mmio_active = true;
	pr_info(DRV_NAME ": MMIO backend enabled: GPIO1 base=0x%lx ver=0x%08x\n",
		mmio_gpio1_base, id);
	return 0;
}

static void release_mmio_backend(void)
{
	mmio_active = false;
	if (mmio_gpio1) {
		iounmap(mmio_gpio1);
		mmio_gpio1 = NULL;
	}
}

static const struct snd_pcm_hardware mcp4901_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_U8 | SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_11025 |
		 SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050,
	.rate_min = 8000,
	.rate_max = 22050,
	.channels_min = 1,
	.channels_max = 2,
	.buffer_bytes_max = 64 * 1024,
	.period_bytes_min = 64,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 1024,
};

static int validate_module_params(void)
{
	if (gpio_cs != DEFAULT_GPIO_CS || gpio_sck != DEFAULT_GPIO_SCK || gpio_sdi != DEFAULT_GPIO_SDI) {
		pr_err(DRV_NAME ": MMIO-only version supports only default GPIOs CS=52 SCK=42 SDI=43\n");
		return -EINVAL;
	}

	if (dac_bits != 8 && dac_bits != 10 && dac_bits != 12) {
		pr_err(DRV_NAME ": dac_bits must be 8, 10 or 12\n");
		return -EINVAL;
	}

	return 0;
}

static inline void gpio_quiet_state(void)
{
	if (likely(mmio_active)) {
		mmio_write_cs(1);
		mmio_write_sck_sdi(0, 0);
		return;
	}

	
	gpio_set_value(gpio_cs, 1);
	gpio_set_value(gpio_sck, 0);
	gpio_set_value(gpio_sdi, 0);
}

/* Send one 16-bit MCP49x1 command word using direct GPIO1 MMIO writes. */
static void mcp4901_send16_mmio(u16 word)
{
	int i;
	int last_sdi = -1;

	mmio_write_cs(0);
	mmio_hold();

	for (i = 15; i >= 0; i--) {
		int bit = (word >> i) & 1;

		if (bit != last_sdi) {
			mmio_write_sck_sdi(0, bit);
			last_sdi = bit;
			mmio_hold();
		}

		mmio_write_sck_sdi(1, bit);
		mmio_hold();
		mmio_write_sck_sdi(0, bit);
		mmio_hold();
	}

	mmio_write_cs(1);
	mmio_hold();
}

static inline void mcp4901_send16(u16 word)
{
	mcp4901_send16_mmio(word);
}

static inline void mcp4901_write_code(struct mcp4901_chip *chip, u16 code)
{
	unsigned int shift = chip ? chip->dac_shift_cached : 4;
	u16 max = chip ? chip->dac_max_cached : 0x00ff;

	if (code > max)
		code = max;

	mcp4901_send16(MCP49X1_CMD_ACTIVE_BASE | (code << shift));
}

/* Leave the analog output quiet after playback or module removal. */
static void mcp4901_mute_shutdown(struct mcp4901_chip *chip)
{
	int i;
	u16 start = chip ? chip->last_code : 128;
	u16 center = chip ? chip->dac_center_cached : 128;

	

	if (chip && !chip->dac_active) {
		gpio_quiet_state();
		return;
	}

	
	for (i = 1; i <= 32; i++) {
		u16 v = start + (((int)center - (int)start) * i) / 32;
		mcp4901_write_code(chip, v);
	}

	for (i = 0; i < 6; i++)
		mcp4901_send16(MCP49X1_CMD_SHUTDOWN);

	gpio_quiet_state();
	if (chip) {
		chip->last_code = center;
		chip->dac_active = false;
	}
}

/* Small-speaker processing: soft limiting, DC blocking and psychoacoustic bass. */
static inline s32 soft_limit_s16(s32 x)
{
	const s32 limit = 32767;
	const s32 knee = 24576;
	s32 sign = 1;
	s32 excess;
	s32 headroom;

	if (x < 0) {
		sign = -1;
		x = -x;
	}

	if (x <= knee)
		return sign * x;

	excess = x - knee;
	headroom = limit - knee;

	
	x = knee + div_s64((s64)excess * headroom, excess + headroom);
	if (x > limit)
		x = limit;

	return sign * x;
}

static inline s32 dc_block(struct mcp4901_chip *chip, s32 x)
{
	s32 y;

	

	y = x - chip->hp_prev_x + ((chip->hp_prev_y * chip->hp_q15_cached) >> 15);

	chip->hp_prev_x = x;
	chip->hp_prev_y = y;

	if (y > 32767)
		y = 32767;
	else if (y < -32768)
		y = -32768;

	return y;
}

static inline s32 read_sample_u8_mono(struct mcp4901_chip *chip,
                                       snd_pcm_uframes_t frame)
{
	return (((s32)((u8 *)chip->pcm_area)[frame]) - 128) << 8;
}

static inline s32 read_sample_u8_stereo(struct mcp4901_chip *chip,
                                        snd_pcm_uframes_t frame)
{
	u8 *p = (u8 *)chip->pcm_area + frame * 2;
	return ((((s32)p[0] + (s32)p[1]) >> 1) - 128) << 8;
}

static inline s32 read_sample_s16_mono(struct mcp4901_chip *chip,
                                       snd_pcm_uframes_t frame)
{
	return ((s16 *)chip->pcm_area)[frame];
}

static inline s32 read_sample_s16_stereo(struct mcp4901_chip *chip,
                                         snd_pcm_uframes_t frame)
{
	s16 *p = (s16 *)chip->pcm_area + frame * 2;
	return ((s32)p[0] + (s32)p[1]) >> 1;
}

static inline s32 read_sample_generic(struct mcp4901_chip *chip,
                                      snd_pcm_uframes_t frame)
{
	unsigned int channels = chip->pcm_channels;

	if (likely(chip->pcm_format == SNDRV_PCM_FORMAT_U8)) {
		u8 *p = (u8 *)chip->pcm_area + frame * channels;
		s32 v;

		if (likely(channels == 1))
			v = p[0];
		else
			v = ((s32)p[0] + (s32)p[1]) >> 1;

		return (v - 128) << 8;
	}

	if (chip->pcm_format == SNDRV_PCM_FORMAT_S16_LE) {
		s16 *p = (s16 *)chip->pcm_area + frame * channels;
		s32 v;

		if (likely(channels == 1))
			v = p[0];
		else
			v = ((s32)p[0] + (s32)p[1]) >> 1;

		return v;
	}

	return 0;
}

static inline s32 psycho_bass_process(struct mcp4901_chip *chip, s32 x)
{
	s32 low;
	s32 abs_low;
	s32 harmonic;
	s32 add;

	if (likely(!chip->psycho_bass_enabled_cached))
		return x;

	

	chip->bass_lp += (x - chip->bass_lp) >> chip->psycho_shift_cached;
	low = chip->bass_lp;

	abs_low = low < 0 ? -low : low;
	if (abs_low > 32767)
		abs_low = 32767;

	harmonic = (low * abs_low) >> 15;
	add = (harmonic * chip->psycho_level_q10_cached) >> 10;
	x += add;

	if (x > 65535)
		x = 65535;
	else if (x < -65536)
		x = -65536;

	return x;
}

static inline s32 psycho_bass_process_always(struct mcp4901_chip *chip, s32 x)
{
	s32 low;
	s32 abs_low;
	s32 harmonic;
	s32 add;

	chip->bass_lp += (x - chip->bass_lp) >> chip->psycho_shift_cached;
	low = chip->bass_lp;

	abs_low = low < 0 ? -low : low;
	if (abs_low > 32767)
		abs_low = 32767;

	harmonic = (low * abs_low) >> 15;
	add = (harmonic * chip->psycho_level_q10_cached) >> 10;
	x += add;

	if (x > 65535)
		x = 65535;
	else if (x < -65536)
		x = -65536;

	return x;
}

static inline u16 process_to_dac_full(struct mcp4901_chip *chip, s32 x)
{
	s32 y;

	x = psycho_bass_process_always(chip, x);
	x = dc_block(chip, x);
	x = (x * chip->gain_q10_cached) >> 10;

	if (unlikely(chip->fade_frames && chip->played_frames < chip->fade_frames))
		x = div_s64((s64)x * chip->played_frames, chip->fade_frames);

	x = soft_limit_s16(x);

	y = (x + 32768) >> chip->dac_convert_shift_cached;
	if (y < 0)
		y = 0;
	else if (y > chip->dac_max_cached)
		y = chip->dac_max_cached;

	return (u16)y;
}

static inline u16 process_to_dac(struct mcp4901_chip *chip, s32 x)
{
	s32 y;

	x = psycho_bass_process(chip, x);

	if (likely(chip->hp_enabled_cached))
		x = dc_block(chip, x);

	
	x = (x * chip->gain_q10_cached) >> 10;

	if (unlikely(chip->fade_frames && chip->played_frames < chip->fade_frames))
		x = div_s64((s64)x * chip->played_frames, chip->fade_frames);

	if (likely(chip->limiter_enabled_cached))
		x = soft_limit_s16(x);
	else if (x > 32767)
		x = 32767;
	else if (x < -32768)
		x = -32768;

	y = (x + 32768) >> chip->dac_convert_shift_cached;
	if (y < 0)
		y = 0;
	else if (y > chip->dac_max_cached)
		y = chip->dac_max_cached;

	return (u16)y;
}

/* The hrtimer is the actual PCM clock: one callback advances one ALSA frame. */
static enum hrtimer_restart mcp4901_timer_fn(struct hrtimer *timer)
{
	struct mcp4901_chip *chip;
	struct snd_pcm_substream *elapsed_substream = NULL;
	struct snd_pcm_substream *substream;
	unsigned long flags;
	u16 code;
	bool do_restart = false;
	bool do_shutdown = false;

	chip = container_of(timer, struct mcp4901_chip, timer);
	code = chip->dac_center_cached;

	spin_lock_irqsave(&chip->lock, flags);

	if (chip->running && chip->substream) {
		substream = chip->substream;
		if (chip->pcm_area && chip->buffer_size && chip->period_size) {
			s32 s = chip->read_sample_cached(chip, chip->hw_ptr);

			code = chip->process_sample_cached(chip, s);
			chip->last_code = code;

			chip->played_frames++;
			chip->hw_ptr++;
			if (chip->hw_ptr >= chip->buffer_size)
				chip->hw_ptr = 0;

			chip->period_pos++;
			if (chip->period_pos >= chip->period_size) {
				chip->period_pos = 0;
				elapsed_substream = substream;
			}

			do_restart = true;
		}
	} else if (chip->shutdown_pending) {
		chip->shutdown_pending = false;
		do_shutdown = true;
	}

	spin_unlock_irqrestore(&chip->lock, flags);

	if (!do_restart) {
		if (do_shutdown)
			mcp4901_mute_shutdown(chip);
		return HRTIMER_NORESTART;
	}

	mcp4901_write_code(chip, code);
	chip->dac_active = true;

	if (elapsed_substream)
		snd_pcm_period_elapsed(elapsed_substream);

	hrtimer_forward_now(timer, chip->period);
	return HRTIMER_RESTART;
}

static void mcp4901_stop_playback(struct mcp4901_chip *chip, bool shutdown, bool may_block)
{
	unsigned long flags;
	int ret;
	bool do_shutdown_now = false;

	if (!chip)
		return;

	spin_lock_irqsave(&chip->lock, flags);
	chip->running = false;
	chip->substream = NULL;
	chip->shutdown_pending = shutdown;
	spin_unlock_irqrestore(&chip->lock, flags);

	if (may_block) {
		hrtimer_cancel(&chip->timer);
		if (shutdown)
			do_shutdown_now = true;
	} else {
		ret = hrtimer_try_to_cancel(&chip->timer);
		if (ret >= 0 && shutdown)
			do_shutdown_now = true;
		
	}

	if (do_shutdown_now) {
		spin_lock_irqsave(&chip->lock, flags);
		chip->shutdown_pending = false;
		spin_unlock_irqrestore(&chip->lock, flags);
		mcp4901_mute_shutdown(chip);
	}
}

/* ALSA PCM callbacks. */
static int mcp4901_pcm_open(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	runtime->hw = mcp4901_pcm_hw;

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &mcp4901_rate_constraint);
	if (ret < 0)
		return ret;

	return 0;
}

static int mcp4901_pcm_close(struct snd_pcm_substream *substream)
{
	struct mcp4901_chip *chip = snd_pcm_substream_chip(substream);

	mcp4901_stop_playback(chip, true, true);
	return 0;
}

static int mcp4901_pcm_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	size_t size = params_buffer_bytes(params);
	unsigned int rate = params_rate(params);

	

	if (!mcp4901_is_supported_rate(rate)) {
		pr_err(DRV_NAME ": unsupported sample rate %u Hz\n", rate);
		return -EINVAL;
	}

	if (runtime->dma_area) {
		vfree(runtime->dma_area);
		runtime->dma_area = NULL;
	}

	runtime->dma_area = vzalloc(size);
	if (!runtime->dma_area)
		return -ENOMEM;

	runtime->dma_bytes = size;
	runtime->dma_addr = 0;

	return 0;
}

static void mcp4901_update_cached_params(struct mcp4901_chip *chip,
					       struct snd_pcm_runtime *runtime)
{
	s32 gain = gain_percent;
	s32 vol = chip->master_volume;
	s32 hp = highpass_q15;
	s32 psycho_level = psycho_bass_level;
	s32 psycho_shift = psycho_bass_shift;

	if (gain < 0)
		gain = 0;
	else if (gain > 800)
		gain = 800;

	if (vol < 0)
		vol = 0;
	else if (vol > 100)
		vol = 100;

	if (hp < 0)
		hp = 0;
	else if (hp > 32760)
		hp = 32760;

	if (psycho_level < 0)
		psycho_level = 0;
	else if (psycho_level > 200)
		psycho_level = 200;

	if (psycho_shift < 2)
		psycho_shift = 2;
	else if (psycho_shift > 10)
		psycho_shift = 10;

	chip->pcm_area = runtime->dma_area;
	chip->pcm_channels = runtime->channels;
	chip->pcm_format = runtime->format;
	chip->hp_enabled_cached = highpass_enable;
	chip->limiter_enabled_cached = limiter_enable;
	chip->psycho_bass_enabled_cached = psycho_bass_enable;
	chip->hp_q15_cached = hp;
	chip->gain_q10_cached = (gain * vol * 1024) / 10000;
	chip->psycho_level_q10_cached = (psycho_level * 1024) / 100;
	chip->psycho_shift_cached = psycho_shift;
	chip->dac_bits_cached = dac_bits;
	chip->dac_shift_cached = 12 - dac_bits;
	chip->dac_convert_shift_cached = 16 - dac_bits;
	chip->dac_max_cached = (1u << dac_bits) - 1;
	chip->dac_center_cached = 1u << (dac_bits - 1);

	if (runtime->format == SNDRV_PCM_FORMAT_U8 && runtime->channels == 1)
		chip->read_sample_cached = read_sample_u8_mono;
	else if (runtime->format == SNDRV_PCM_FORMAT_U8 && runtime->channels == 2)
		chip->read_sample_cached = read_sample_u8_stereo;
	else if (runtime->format == SNDRV_PCM_FORMAT_S16_LE && runtime->channels == 1)
		chip->read_sample_cached = read_sample_s16_mono;
	else if (runtime->format == SNDRV_PCM_FORMAT_S16_LE && runtime->channels == 2)
		chip->read_sample_cached = read_sample_s16_stereo;
	else
		chip->read_sample_cached = read_sample_generic;

	if (chip->hp_enabled_cached && chip->limiter_enabled_cached && chip->psycho_bass_enabled_cached)
		chip->process_sample_cached = process_to_dac_full;
	else
		chip->process_sample_cached = process_to_dac;
}

static int mcp4901_pcm_hw_free(struct snd_pcm_substream *substream)
{
	struct mcp4901_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;

	mcp4901_stop_playback(chip, true, true);

	if (runtime->dma_area) {
		vfree(runtime->dma_area);
		runtime->dma_area = NULL;
	}
	runtime->dma_bytes = 0;
	runtime->dma_addr = 0;
	spin_lock_irqsave(&chip->lock, flags);
	chip->pcm_area = NULL;
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int mcp4901_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct mcp4901_chip *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	unsigned long flags;
	u64 ns;

	if (!runtime->rate)
		return -EINVAL;

	ns = div_u64(1000000000ULL, runtime->rate);

	spin_lock_irqsave(&chip->lock, flags);
	chip->substream = substream;
	chip->rate = runtime->rate;
	chip->buffer_size = runtime->buffer_size;
	chip->period_size = runtime->period_size;
	chip->hw_ptr = 0;
	chip->period_pos = 0;
	chip->played_frames = 0;
	chip->shutdown_pending = false;
	chip->fade_frames = (runtime->rate * max(fade_ms, 0)) / 1000;
	mcp4901_update_cached_params(chip, runtime);
	chip->period = ns_to_ktime(ns);
	chip->hp_prev_x = 0;
	chip->hp_prev_y = 0;
	chip->bass_lp = 0;
	chip->last_code = chip->dac_center_cached;
	
	spin_unlock_irqrestore(&chip->lock, flags);

	return 0;
}

static int mcp4901_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct mcp4901_chip *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&chip->lock, flags);
		chip->running = true;
		chip->shutdown_pending = false;
		chip->substream = substream;
		spin_unlock_irqrestore(&chip->lock, flags);

		hrtimer_start(&chip->timer, chip->period, HRTIMER_MODE_REL);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		mcp4901_stop_playback(chip, true, false);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static snd_pcm_uframes_t mcp4901_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct mcp4901_chip *chip = snd_pcm_substream_chip(substream);
	unsigned long flags;
	snd_pcm_uframes_t pos;

	spin_lock_irqsave(&chip->lock, flags);
	pos = chip->hw_ptr;
	spin_unlock_irqrestore(&chip->lock, flags);

	return pos;
}

static const struct snd_pcm_ops mcp4901_pcm_ops = {
	.open = mcp4901_pcm_open,
	.close = mcp4901_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = mcp4901_pcm_hw_params,
	.hw_free = mcp4901_pcm_hw_free,
	.prepare = mcp4901_pcm_prepare,
	.trigger = mcp4901_pcm_trigger,
	.pointer = mcp4901_pcm_pointer,
};

static int request_one_gpio(unsigned int gpio, const char *name, int value)
{
	int ret;

	ret = gpio_request(gpio, name);
	if (ret) {
		pr_err(DRV_NAME ": gpio_request(%u/%s) failed: %d\n", gpio, name, ret);
		return ret;
	}

	ret = gpio_direction_output(gpio, value);
	if (ret) {
		pr_err(DRV_NAME ": gpio_direction_output(%u/%s) failed: %d\n", gpio, name, ret);
		gpio_free(gpio);
		return ret;
	}

	return 0;
}

static void free_gpios(void)
{
	gpio_quiet_state();
	gpio_free(gpio_sdi);
	gpio_free(gpio_sck);
	gpio_free(gpio_cs);
}

/* GPIO API is still used for ownership and direction setup. Playback uses MMIO. */
static int request_gpios(void)
{
	int ret;

	ret = request_one_gpio(gpio_cs, "mcp4901-cs", 1);
	if (ret)
		return ret;

	ret = request_one_gpio(gpio_sck, "mcp4901-sck", 0);
	if (ret)
		goto err_sck;

	ret = request_one_gpio(gpio_sdi, "mcp4901-sdi", 0);
	if (ret)
		goto err_sdi;

	gpio_quiet_state();
	return 0;

err_sdi:
	gpio_free(gpio_sck);
err_sck:
	gpio_free(gpio_cs);
	return ret;
}

static int mcp4901_volume_info(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 100;
	return 0;
}

static int mcp4901_volume_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct mcp4901_chip *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	unsigned int vol;

	spin_lock_irqsave(&chip->lock, flags);
	vol = chip->master_volume;
	spin_unlock_irqrestore(&chip->lock, flags);

	ucontrol->value.integer.value[0] = vol;
	return 0;
}

static int mcp4901_volume_put(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	struct mcp4901_chip *chip = snd_kcontrol_chip(kcontrol);
	unsigned long flags;
	unsigned int old, vol;

	vol = ucontrol->value.integer.value[0];
	if (vol > 100)
		vol = 100;

	spin_lock_irqsave(&chip->lock, flags);
	old = chip->master_volume;
	chip->master_volume = vol;
	{
		s32 gain = gain_percent;
		if (gain < 0)
			gain = 0;
		else if (gain > 800)
			gain = 800;
		chip->gain_q10_cached = (gain * vol * 1024) / 10000;
	}
	spin_unlock_irqrestore(&chip->lock, flags);

	return old != vol;
}

/* Standard ALSA mixer control for system/user volume. */
static const struct snd_kcontrol_new mcp4901_volume_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Master Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = mcp4901_volume_info,
	.get = mcp4901_volume_get,
	.put = mcp4901_volume_put,
};

/* Create the ALSA card and one playback-only PCM device. */
static int create_card(struct device *dev, struct mcp4901_chip *chip)
{
	struct snd_card *card;
	struct snd_pcm *pcm;
	int ret;

	ret = snd_card_new(dev, index, id, THIS_MODULE, 0, &card);
	if (ret < 0)
		return ret;

	chip->card = card;
	card->private_data = chip;

	strscpy(card->driver, "MCP49x1GPIO", sizeof(card->driver));
	strscpy(card->shortname, "MCP49x1 GPIO Audio", sizeof(card->shortname));
	strscpy(card->longname, "MCP49x1 GPIO bit-bang PCM", sizeof(card->longname));

	ret = snd_pcm_new(card, "MCP49x1 PCM", 0, 1, 0, &pcm);
	if (ret < 0)
		goto err_card;

	chip->pcm = pcm;
	pcm->private_data = chip;
	strscpy(pcm->name, "MCP49x1 GPIO PCM", sizeof(pcm->name));

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &mcp4901_pcm_ops);

	ret = snd_ctl_add(card, snd_ctl_new1(&mcp4901_volume_control, chip));
	if (ret < 0)
		goto err_card;

	ret = snd_card_register(card);
	if (ret < 0)
		goto err_card;

	return 0;

err_card:
	snd_card_free(card);
	chip->card = NULL;
	return ret;
}

static int mcp4901_probe(struct platform_device *pdev)
{
	struct mcp4901_chip *chip;
	int ret;

	if (!enable)
		return -ENODEV;

	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->master_volume = 100;
	chip->read_sample_cached = read_sample_generic;
	chip->process_sample_cached = process_to_dac;
	chip->dac_bits_cached = dac_bits;
	chip->dac_shift_cached = 12 - dac_bits;
	chip->dac_convert_shift_cached = 16 - dac_bits;
	chip->dac_max_cached = (1u << dac_bits) - 1;
	chip->dac_center_cached = 1u << (dac_bits - 1);
	chip->last_code = chip->dac_center_cached;
	chip->dac_active = false;
	spin_lock_init(&chip->lock);
	hrtimer_init(&chip->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	chip->timer.function = mcp4901_timer_fn;

	platform_set_drvdata(pdev, chip);
	ret = request_gpios();
	if (ret)
		goto err_free;

	ret = init_mmio_backend();
	if (ret)
		goto err_gpio;

	
	gpio_quiet_state();

	ret = create_card(&pdev->dev, chip);
	if (ret)
		goto err_mmio;

	pr_info(DRV_NAME ": registered ALSA PCM, backend=mmio-only, GPIO CS=%d SCK=%d SDI=%d, rates=8000/11025/16000/22050, gain=%d%% limiter=%d highpass=%d q15=%d psycho_bass=%d level=%d shift=%d master_volume=%u%% dac_bits=%d\n",
		gpio_cs, gpio_sck, gpio_sdi, gain_percent, limiter_enable,
		highpass_enable, highpass_q15, psycho_bass_enable, psycho_bass_level,
		psycho_bass_shift, chip->master_volume, dac_bits);
	return 0;

err_mmio:
	release_mmio_backend();
err_gpio:
	free_gpios();
err_free:
	platform_set_drvdata(pdev, NULL);
	kfree(chip);
	return ret;
}

static int mcp4901_remove(struct platform_device *pdev)
{
	struct mcp4901_chip *chip = platform_get_drvdata(pdev);

	if (!chip)
		return 0;

	mcp4901_stop_playback(chip, true, true);

	if (chip->card)
		snd_card_free(chip->card);

	free_gpios();
	release_mmio_backend();
	platform_set_drvdata(pdev, NULL);
	kfree(chip);

	pr_info(DRV_NAME ": unloaded\n");
	return 0;
}

static struct platform_driver mcp4901_driver = {
	.probe = mcp4901_probe,
	.remove = mcp4901_remove,
	.driver = {
		.name = DRV_NAME,
	},
};

static int __init mcp4901_init(void)
{
	int ret;

	ret = validate_module_params();
	if (ret)
		return ret;

	ret = platform_driver_register(&mcp4901_driver);
	if (ret)
		return ret;

	mcp4901_pdev = platform_device_register_simple(DRV_NAME, -1, NULL, 0);
	if (IS_ERR(mcp4901_pdev)) {
		ret = PTR_ERR(mcp4901_pdev);
		mcp4901_pdev = NULL;
		platform_driver_unregister(&mcp4901_driver);
		return ret;
	}

	return 0;
}

static void __exit mcp4901_exit(void)
{
	if (mcp4901_pdev) {
		platform_device_unregister(mcp4901_pdev);
		mcp4901_pdev = NULL;
	}

	platform_driver_unregister(&mcp4901_driver);
}

module_init(mcp4901_init);
module_exit(mcp4901_exit);

MODULE_AUTHOR("bartek@env.pl");
MODULE_DESCRIPTION("ALSA PCM driver for MCP4901/MCP4911/MCP4921 over direct GPIO1 MMIO bit-bang");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRV_NAME);
