/* ES8311 audio: record 16 kHz mono WAV to SD, play a WAV back from SD.
 *
 * The ES8311 is a mono codec (one ADC fed by the mic, one DAC feeding the
 * speaker amp) wired to one full-duplex I2S port. Both directions therefore
 * share BCLK/WS and one sample rate: they are opened and closed together,
 * and changing the playback rate necessarily re-rates the capture side too.
 * That is why audio_play_wav() closes and re-opens the codec device rather
 * than reconfiguring one direction.
 *
 * On the wire the I2S frame is always 16-bit *stereo* even though the codec
 * is mono: on capture the left slot carries the mic and the right slot
 * carries the DAC output as a reference (the ES8311's AEC reference; see
 * es8311_codec_cfg_t.no_dac_ref), so recording keeps the left slot only. On
 * playback a mono file is duplicated into both slots.
 *
 * Registers/bring-up order are entirely esp_codec_dev's (managed component,
 * Apache-2.0); this file only owns the I2C bus, the I2S channel pair, the
 * WAV framing and the SD streaming.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "tick_ms.h"
#include "wav.h"

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio";

/* Pin map (design §2). I2C is the codec's control channel; I2S carries the
 * samples; the PA enable is driven by the codec driver via its GPIO
 * interface, not by this file. */
#define PIN_I2C_SDA  47
#define PIN_I2C_SCL  48
#define PIN_I2S_MCLK 14
#define PIN_I2S_BCLK 15
#define PIN_I2S_WS   38
#define PIN_I2S_DOUT 45
#define PIN_I2S_DIN  16
#define PIN_PA_EN    46

#define AUDIO_I2C_PORT I2C_NUM_0
#define AUDIO_I2S_PORT I2S_NUM_0

/* ES8311 7-bit address is 0x18 or 0x19 depending on the CE strap. Probe
 * both: the board's strap is not documented in the design and a wrong
 * guess would look identical to "codec is dead". esp_codec_dev's I2C
 * wrapper wants the 8-bit form (it shifts back down internally), hence the
 * <<1 at the call site. */
#define ES8311_ADDR7_CE_LOW  0x18
#define ES8311_ADDR7_CE_HIGH 0x19
#define I2C_PROBE_TIMEOUT_MS 50

#define AUDIO_RATE_HZ 16000   /* capture rate; design §5 uploads are 16 kHz mono.
                                 Playback uses whatever the WAV header says --
                                 bridge replies are 24 kHz provider-native. */

/* Tuning constants below are UNTUNED starting points (checkpoint C4 is the
 * first time any of this meets a real microphone and speaker); revisit once
 * the operator reports how the recording and the reply actually sound. */
#define AUDIO_RAIL_SETTLE_MS 100   /* untuned: analog rail + codec LDO settle before I2C */
#define AUDIO_MIC_GAIN_DB    30.0f /* untuned: ES8311 PGA step, 0/6/.../42 dB */
#define AUDIO_OUT_VOL        85    /* untuned: 0-100 on esp_codec_dev's volume curve */
#define AUDIO_DRAIN_MS       300   /* untuned: > the DMA ring below at 16 kHz (256 ms),
                                      so the tail of a clip is actually heard before
                                      the PA is cut */

/* DMA ring: 8 x 512 frames = 4096 frames = 256 ms at 16 kHz, 16 kB of
 * internal RAM. Deliberately larger than the IDF default (6 x 240 = 90 ms):
 * capture is streamed straight to the SD card and a FAT allocation stall of
 * a hundred-odd milliseconds is normal, so the ring has to cover it or the
 * recording drops samples. Untuned. */
#define AUDIO_DMA_DESC   8
#define AUDIO_DMA_FRAMES 512

/* Capture reads a stereo chunk and writes half of it (left slot only).
 * 8192 B stereo = 2048 frames = 128 ms at 16 kHz, so keep_going() -- the
 * record button -- is sampled about 8x/s. */
#define REC_STEREO_BYTES 8192
#define REC_MONO_BYTES   (REC_STEREO_BYTES / 2)

/* Playback reads this many bytes of file data at a time; a mono file
 * doubles into a buffer twice this size. */
#define PLAY_CHUNK_BYTES 2048

#define BEEP_HZ           1000
#define BEEP_MS           200
#define BEEP_CHUNK_MS     10
#define BEEP_CHUNK_FRAMES (AUDIO_RATE_HZ / 1000 * BEEP_CHUNK_MS)   /* 160 frames */
#define BEEP_AMPLITUDE    3000   /* untuned: ~9% of full scale, a chime not an alarm */

static i2c_master_bus_handle_t     s_i2c;
static i2s_chan_handle_t           s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl_if;
static const audio_codec_if_t      *s_codec_if;
static const audio_codec_data_if_t *s_data_if;
static esp_codec_dev_handle_t       s_dev;
static uint32_t                     s_rate;   /* rate the device is open at; 0 = closed */

/* Opens (or re-opens) the codec at `rate`. Gain and volume are applied
 * after the open because esp_codec_dev rejects them in the closed state,
 * and a re-open resets them. */
static int codec_open_at(uint32_t rate)
{
    if (s_dev == NULL) return -1;
    if (s_rate == rate) return 0;
    if (s_rate) {
        esp_codec_dev_close(s_dev);
        s_rate = 0;
    }
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel         = 2,
        .sample_rate     = rate,
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    if (r != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec open at %u Hz failed (%d)", (unsigned)rate, r);
        return -1;
    }
    s_rate = rate;
    esp_codec_dev_set_in_gain(s_dev, AUDIO_MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(s_dev, AUDIO_OUT_VOL);
    return 0;
}

static int i2c_up(uint8_t *addr7_out)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = AUDIO_I2C_PORT,
        .sda_io_num        = PIN_I2C_SDA,
        .scl_io_num        = PIN_I2C_SCL,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags             = { .enable_internal_pullup = true },
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        s_i2c = NULL;
        return -1;
    }
    if (i2c_master_probe(s_i2c, ES8311_ADDR7_CE_LOW, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
        *addr7_out = ES8311_ADDR7_CE_LOW;
    } else if (i2c_master_probe(s_i2c, ES8311_ADDR7_CE_HIGH, I2C_PROBE_TIMEOUT_MS) == ESP_OK) {
        *addr7_out = ES8311_ADDR7_CE_HIGH;
    } else {
        ESP_LOGE(TAG, "no ES8311 at 0x%02x or 0x%02x (rail off? SDA %d / SCL %d miswired?)",
                 ES8311_ADDR7_CE_LOW, ES8311_ADDR7_CE_HIGH, PIN_I2C_SDA, PIN_I2C_SCL);
        return -1;
    }
    ESP_LOGI(TAG, "ES8311 found at 0x%02x", *addr7_out);
    return 0;
}

static int i2s_up(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = AUDIO_DMA_DESC;
    chan_cfg.dma_frame_num = AUDIO_DMA_FRAMES;
    /* Send silence, not the previous buffer's contents, when nothing is
     * queued -- otherwise the tail of the last clip loops audibly while
     * the mic is recording. */
    chan_cfg.auto_clear = true;

    /* One call, both handles: full duplex on a single port, sharing BCLK
     * and WS (the ES8311 has one clock domain, so it cannot be otherwise). */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, &s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx = s_rx = NULL;
        return -1;
    }

    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,   /* the codec runs off our MCLK (use_mclk below) */
            .bclk = PIN_I2S_BCLK,
            .ws   = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din  = PIN_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    /* Init only -- esp_codec_dev enables/disables the channels itself as
     * the device is opened and closed, and reconfigures the clock on a
     * re-open. Enabling here would fight it. */
    if (i2s_channel_init_std_mode(s_tx, &std) != ESP_OK ||
        i2s_channel_init_std_mode(s_rx, &std) != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed");
        return -1;
    }
    return 0;
}

int audio_init(void)
{
    if (s_dev) return 0;   /* idempotent: already up */

    board_rail_audio(1);
    board_delay_ms(AUDIO_RAIL_SETTLE_MS);

    uint8_t addr7 = 0;
    if (i2c_up(&addr7) != 0) goto fail;
    if (i2s_up() != 0) goto fail;

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = AUDIO_I2C_PORT,
        .addr       = (uint8_t)(addr7 << 1),   /* esp_codec_dev wants the 8-bit form */
        .bus_handle = s_i2c,
    };
    s_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_ctrl_if == NULL) { ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed"); goto fail; }

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if     = s_ctrl_if,
        .gpio_if     = audio_codec_new_gpio(),   /* drives PA enable */
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin      = PIN_PA_EN,
        .pa_reverted = false,                    /* PA on when the pin is high */
        .master_mode = false,                    /* the S3 is I2S master, the codec is slave */
        .use_mclk    = true,
        .digital_mic = false,
    };
    s_codec_if = es8311_codec_new(&es_cfg);
    if (s_codec_if == NULL) { ESP_LOGE(TAG, "es8311_codec_new failed"); goto fail; }

    audio_codec_i2s_cfg_t data_cfg = {
        .port      = AUDIO_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    s_data_if = audio_codec_new_i2s_data(&data_cfg);
    if (s_data_if == NULL) { ESP_LOGE(TAG, "audio_codec_new_i2s_data failed"); goto fail; }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = s_codec_if,
        .data_if  = s_data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (s_dev == NULL) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); goto fail; }

    if (codec_open_at(AUDIO_RATE_HZ) != 0) goto fail;

    ESP_LOGI(TAG, "audio up: %d Hz, mic gain %.0f dB, out vol %d",
             AUDIO_RATE_HZ, (double)AUDIO_MIC_GAIN_DB, AUDIO_OUT_VOL);
    return 0;

fail:
    audio_deinit();
    return -1;
}

void audio_deinit(void)
{
    if (s_dev) {
        if (s_rate) esp_codec_dev_close(s_dev);
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    s_rate = 0;
    if (s_data_if)  { audio_codec_delete_data_if(s_data_if);   s_data_if = NULL; }
    if (s_codec_if) { audio_codec_delete_codec_if(s_codec_if); s_codec_if = NULL; }
    if (s_ctrl_if)  { audio_codec_delete_ctrl_if(s_ctrl_if);   s_ctrl_if = NULL; }
    if (s_tx) { i2s_del_channel(s_tx); s_tx = NULL; }
    if (s_rx) { i2s_del_channel(s_rx); s_rx = NULL; }
    if (s_i2c) { i2c_del_master_bus(s_i2c); s_i2c = NULL; }
    board_rail_audio(0);
}

long audio_record_to(const char *path, int (*keep_going)(void *), void *ctx, unsigned max_ms)
{
    if (s_dev == NULL) { ESP_LOGE(TAG, "record: audio not initialised"); return -1; }
    if (codec_open_at(AUDIO_RATE_HZ) != 0) return -1;

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "record: fopen %s failed (errno %d)", path, errno);
        return -1;
    }

    /* Placeholder header: the real one needs the byte count, which is only
     * known at the end, so it is patched in place on the way out. */
    uint8_t hdr[44];
    memset(hdr, 0, sizeof hdr);
    if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        ESP_LOGE(TAG, "record: header write failed (errno %d)", errno);
        fclose(f);
        return -1;
    }

    /* Heap, not stack: the main task's stack is 8 kB and these are 12 kB. */
    int16_t *stereo = malloc(REC_STEREO_BYTES);
    int16_t *mono   = malloc(REC_MONO_BYTES);
    if (stereo == NULL || mono == NULL) {
        ESP_LOGE(TAG, "record: out of memory");
        free(stereo); free(mono); fclose(f);
        return -1;
    }

    const int frames_per_chunk = REC_STEREO_BYTES / 4;   /* 2 ch x 16 bit */
    uint32_t total = 0;
    int64_t start_us = esp_timer_get_time();
    int failed = 0;

    while (1) {
        if (keep_going && !keep_going(ctx)) break;
        if ((esp_timer_get_time() - start_us) / 1000 >= (int64_t)max_ms) {
            ESP_LOGW(TAG, "record: max_ms (%u) reached, stopping", max_ms);
            break;
        }
        int r = esp_codec_dev_read(s_dev, stereo, REC_STEREO_BYTES);
        if (r != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "record: codec read failed (%d)", r);
            failed = 1;
            break;
        }
        for (int i = 0; i < frames_per_chunk; i++) mono[i] = stereo[2 * i];   /* left slot = mic */
        size_t want = (size_t)frames_per_chunk * 2;
        if (fwrite(mono, 1, want, f) != want) {
            ESP_LOGE(TAG, "record: short write at %u bytes (errno %d)", (unsigned)total, errno);
            failed = 1;
            break;
        }
        total += (uint32_t)want;
    }

    free(stereo);
    free(mono);
    if (failed) { fclose(f); return -1; }

    wav_write_header(hdr, AUDIO_RATE_HZ, 16, 1, total);
    if (fseek(f, 0, SEEK_SET) != 0 || fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        ESP_LOGE(TAG, "record: header patch failed (errno %d)", errno);
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "record: fclose failed (errno %d)", errno);
        return -1;
    }
    return (long)total;
}

int audio_play_wav(const char *path, int (*stop_now)(void *), void *ctx)
{
    if (s_dev == NULL) { ESP_LOGE(TAG, "play: audio not initialised"); return -1; }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "play: fopen %s failed (errno %d)", path, errno);
        return -1;
    }

    uint8_t head[512];
    size_t got = fread(head, 1, sizeof head, f);
    wav_info_t inf;
    if (wav_parse_header(head, got, &inf) != 0) {
        ESP_LOGE(TAG, "play: %s is not a WAV we can parse", path);
        fclose(f);
        return -1;
    }
    if (inf.bits != 16 || inf.channels < 1 || inf.channels > 2) {
        ESP_LOGE(TAG, "play: unsupported format %u-bit %u-ch", inf.bits, inf.channels);
        fclose(f);
        return -1;
    }
    ESP_LOGI(TAG, "play: %s %u Hz %u ch, %u data bytes",
             path, (unsigned)inf.sample_rate, inf.channels, (unsigned)inf.data_bytes);

    /* Rate comes from the file, never a constant: uploads are 16 kHz but
     * bridge replies are 24 kHz, and playing one at the other's rate is the
     * classic chipmunk/slow-motion failure. */
    if (codec_open_at(inf.sample_rate) != 0) { fclose(f); return -1; }
    if (fseek(f, (long)inf.data_offset, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "play: seek to data failed (errno %d)", errno);
        fclose(f);
        return -1;
    }

    uint8_t *fbuf = malloc(PLAY_CHUNK_BYTES);
    int16_t *out  = malloc(PLAY_CHUNK_BYTES * 2);   /* mono -> stereo doubles it */
    if (fbuf == NULL || out == NULL) {
        ESP_LOGE(TAG, "play: out of memory");
        free(fbuf); free(out); fclose(f);
        return -1;
    }

    /* data_bytes == 0 means the writer never patched the length (a streamed
     * WAV); fall back to "until EOF" rather than playing nothing. */
    uint32_t remaining = inf.data_bytes ? inf.data_bytes : UINT32_MAX;
    const size_t frame_bytes = (size_t)inf.channels * 2;
    int failed = 0;

    while (remaining > 0) {
        if (stop_now && stop_now(ctx)) { ESP_LOGI(TAG, "play: stopped by caller"); break; }
        size_t want = remaining < PLAY_CHUNK_BYTES ? remaining : PLAY_CHUNK_BYTES;
        size_t n = fread(fbuf, 1, want, f);
        if (n == 0) break;                       /* EOF */
        remaining -= (uint32_t)n;
        n -= n % frame_bytes;                    /* whole frames only */
        if (n == 0) break;

        void *p;
        int   len;
        if (inf.channels == 1) {
            const int16_t *m = (const int16_t *)fbuf;
            size_t frames = n / 2;
            for (size_t i = 0; i < frames; i++) { out[2 * i] = m[i]; out[2 * i + 1] = m[i]; }
            p   = out;
            len = (int)(frames * 4);
        } else {
            p   = fbuf;
            len = (int)n;
        }
        int r = esp_codec_dev_write(s_dev, p, len);
        if (r != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "play: codec write failed (%d)", r);
            failed = 1;
            break;
        }
    }

    free(fbuf);
    free(out);
    fclose(f);
    /* esp_codec_dev_write() returns once the data is queued, so the last
     * few hundred ms are still in the DMA ring. Let them play out before
     * the caller closes the device or cuts the rail. */
    board_delay_ms(AUDIO_DRAIN_MS);
    return failed ? -1 : 0;
}

void audio_beep(void)
{
    if (s_dev == NULL) return;
    /* Generated at the capture rate so the pitch is fixed no matter what
     * the last thing played was (a 24 kHz reply leaves the device at
     * 24 kHz). */
    if (codec_open_at(AUDIO_RATE_HZ) != 0) return;

    int16_t buf[BEEP_CHUNK_FRAMES * 2];
    const int period = AUDIO_RATE_HZ / BEEP_HZ;   /* 16 samples at 16 kHz */
    for (int i = 0; i < BEEP_CHUNK_FRAMES; i++) {
        int16_t s = (i % period) < (period / 2) ? (int16_t)BEEP_AMPLITUDE
                                                : (int16_t)-BEEP_AMPLITUDE;
        buf[2 * i]     = s;
        buf[2 * i + 1] = s;
    }
    /* BEEP_CHUNK_FRAMES is a whole number of periods, so repeating the
     * chunk keeps the waveform phase-continuous. */
    for (int rep = 0; rep < BEEP_MS / BEEP_CHUNK_MS; rep++) {
        if (esp_codec_dev_write(s_dev, buf, (int)sizeof buf) != ESP_CODEC_DEV_OK) break;
    }
    board_delay_ms(AUDIO_DRAIN_MS);
}
