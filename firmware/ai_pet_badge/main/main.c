#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"
#include "nvs_flash.h"
#include "src/misc/cache/instance/lv_image_cache.h"

#define FRAME_W 360
#define FRAME_H 360
#define FRAME_BYTES (FRAME_W * FRAME_H * 2)
#define BG_COLOR 0xFFF0F5

static const char *TAG = "badge";
extern const uint8_t assets_start[] asm("_binary_animations_badge_start");
extern const uint8_t assets_end[] asm("_binary_animations_badge_end");

typedef struct {
    char magic[8];
    uint32_t version, width, height, fps, clip_count, frame_count;
} pack_header_t;

typedef struct {
    char name[16];
    uint32_t first, count;
} pack_clip_t;

typedef struct {
    uint32_t offset, length;
} pack_frame_t;

static const pack_header_t *pack;
static const pack_clip_t *clips;
static const pack_frame_t *frames;
static lv_obj_t *character_image;
static lv_obj_t *status_label;
static lv_image_dsc_t images[2];
static uint8_t *pixels[2];
static volatile uint32_t requested_clip;
static volatile uint32_t clip_revision;
static volatile uint32_t touch_count;
static volatile uint32_t displayed_frames;
static volatile uint32_t decode_time_us;
static esp_codec_dev_handle_t speaker;

static esp_err_t check_assets(void)
{
    size_t length = assets_end - assets_start;
    if (length < sizeof(pack_header_t)) return ESP_ERR_INVALID_SIZE;
    pack = (const pack_header_t *)assets_start;
    if (memcmp(pack->magic, "BDGZ565\0", 8) || pack->version != 1 ||
        pack->width != FRAME_W || pack->height != FRAME_H ||
        !pack->fps || pack->fps > 30 || !pack->clip_count || pack->clip_count > 16 ||
        !pack->frame_count || pack->frame_count > 4096) return ESP_ERR_INVALID_ARG;
    size_t table_end = sizeof(*pack) + pack->clip_count * sizeof(*clips) + pack->frame_count * sizeof(*frames);
    if (table_end > length) return ESP_ERR_INVALID_SIZE;
    clips = (const pack_clip_t *)(assets_start + sizeof(*pack));
    frames = (const pack_frame_t *)(clips + pack->clip_count);
    for (uint32_t i = 0; i < pack->clip_count; ++i) {
        if (!memchr(clips[i].name, 0, sizeof(clips[i].name)) || !clips[i].count ||
            clips[i].first >= pack->frame_count || clips[i].count > pack->frame_count - clips[i].first)
            return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t i = 0; i < pack->frame_count; ++i) {
        if (!frames[i].length || frames[i].offset < table_end || frames[i].offset > length ||
            frames[i].length > length - frames[i].offset) return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "Assets validated: %u clips, %u frames, %u fps, %u bytes",
             (unsigned)pack->clip_count, (unsigned)pack->frame_count, (unsigned)pack->fps, (unsigned)length);
    return ESP_OK;
}

static void next_clip(void)
{
    requested_clip = (requested_clip + 1) % pack->clip_count;
    ++clip_revision;
}

static void on_touch(lv_event_t *event)
{
    (void)event;
    ++touch_count;
    next_clip();
    ESP_LOGI(TAG, "Touch #%u -> %s", (unsigned)touch_count, clips[requested_clip].name);
}

static void print_status(void)
{
    printf("BADGE_STATUS {\"clip\":\"%s\",\"frames\":%u,\"touches\":%u,\"decode_us\":%u,"
           "\"heap\":%u,\"min_heap\":%u,\"psram\":%u}\n",
           clips[requested_clip].name, (unsigned)displayed_frames, (unsigned)touch_count,
           (unsigned)decode_time_us, (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void diagnostic_tone(void)
{
    if (!speaker) speaker = bsp_audio_codec_speaker_init();
    if (!speaker) { ESP_LOGE(TAG, "Speaker initialization failed"); return; }
    esp_codec_dev_sample_info_t format = {.sample_rate = 16000, .channel = 1, .bits_per_sample = 16};
    int result = esp_codec_dev_open(speaker, &format);
    if (result != ESP_OK) { ESP_LOGE(TAG, "Speaker open failed: %d", result); return; }
    esp_codec_dev_set_out_vol(speaker, 35);
    int16_t samples[256];
    const float frequencies[] = {523.25f, 659.25f, 783.99f};
    for (int note = 0; note < 3; ++note) {
        for (int block = 0; block < 10; ++block) {
            for (int i = 0; i < 256; ++i) {
                int sample = block * 256 + i;
                float envelope = fminf(1.0f, fminf(sample / 160.0f, (2560 - sample) / 160.0f));
                samples[i] = (int16_t)(3500 * envelope * sinf(6.2831853f * frequencies[note] * sample / 16000));
            }
            if (esp_codec_dev_write(speaker, samples, sizeof(samples)) != ESP_OK) {
                ESP_LOGE(TAG, "Speaker write failed");
                esp_codec_dev_close(speaker);
                return;
            }
        }
    }
    esp_codec_dev_close(speaker);
    ESP_LOGI(TAG, "Diagnostic tone completed");
}

static void command_task(void *arg)
{
    (void)arg;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 1024, .rx_buffer_size = 2048};
        if (usb_serial_jtag_driver_install(&config) != ESP_OK) { vTaskDelete(NULL); return; }
    }
    char line[64];
    size_t used = 0;
    while (true) {
        uint8_t byte;
        if (usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(250)) != 1) continue;
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[used] = 0;
            if (!strcmp(line, "STATUS")) print_status();
            else if (!strcmp(line, "NEXT")) { next_clip(); print_status(); }
            else if (!strcmp(line, "TONE")) diagnostic_tone();
            else if (used) printf("BADGE_ERROR unknown command\n");
            used = 0;
        } else if (byte >= 32 && byte < 127) {
            if (used < sizeof(line) - 1) line[used++] = byte;
            else used = 0;
        }
    }
}

static void animation_task(void *arg)
{
    (void)arg;
    tinfl_decompressor *inflater = heap_caps_malloc(sizeof(*inflater), MALLOC_CAP_8BIT);
    assert(inflater);
    uint32_t index = 0, revision = UINT32_MAX;
    unsigned slot = 0;
    int64_t previous_frame = esp_timer_get_time();
    int64_t last_report = previous_frame;
    uint32_t previous_count = 0;
    while (true) {
        uint32_t selected = requested_clip;
        bool changed = revision != clip_revision;
        if (changed) { index = 0; revision = clip_revision; }
        const pack_clip_t *clip = &clips[selected];
        const pack_frame_t *frame = &frames[clip->first + index];
        int64_t started = esp_timer_get_time();
        tinfl_init(inflater);
        size_t input_length = frame->length;
        size_t count = FRAME_BYTES;
        tinfl_status status = tinfl_decompress(inflater, assets_start + frame->offset, &input_length,
                                               pixels[slot], pixels[slot], &count,
                                               TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        decode_time_us = esp_timer_get_time() - started;
        if (status != TINFL_STATUS_DONE || count != FRAME_BYTES || input_length != frame->length) {
            ESP_LOGE(TAG, "Frame decompression failed: %s[%u]", clip->name, (unsigned)index);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        bsp_display_lock(-1);
        lv_image_cache_drop(&images[slot]);
        lv_image_set_src(character_image, &images[slot]);
        if (changed) lv_label_set_text(status_label, "待机中 · 轻触切换动作");
        lv_obj_invalidate(character_image);
        bsp_display_unlock();
        slot ^= 1;
        ++displayed_frames;
        if (++index >= clip->count) { next_clip(); index = 0; }
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 10000000) {
            float fps = (displayed_frames - previous_count) * 1000000.0f / (now - last_report);
            ESP_LOGI(TAG, "Playback %.1f fps; decode %.1f ms; heap %u; minimum %u", fps,
                     decode_time_us / 1000.0f, (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
            previous_count = displayed_frames;
            last_report = now;
        }
        int64_t deadline = previous_frame + 1000000 / pack->fps;
        if (deadline > now) vTaskDelay(pdMS_TO_TICKS((deadline - now) / 1000));
        else vTaskDelay(1);
        previous_frame = esp_timer_get_time();
    }
}

void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    ESP_ERROR_CHECK(check_assets());
    for (int i = 0; i < 2; ++i) {
        pixels[i] = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(pixels[i]);
        images[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = FRAME_W, .h = FRAME_H, .stride = FRAME_W * 2},
            .data_size = FRAME_BYTES, .data = pixels[i]
        };
    }
    lv_display_t *display = bsp_display_start();
    assert(display);
    ESP_ERROR_CHECK(bsp_display_brightness_set(75));
    bsp_display_lock(-1);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, on_touch, LV_EVENT_CLICKED, NULL);
    character_image = lv_image_create(screen);
    lv_obj_set_size(character_image, FRAME_W, FRAME_H);
    lv_obj_align(character_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(character_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x865571), 0);
    lv_label_set_text(title, "爱莉希雅");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 17);
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_font(status_label, &lv_font_source_han_sans_sc_16_cjk, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x865571), 0);
    lv_label_set_text(status_label, "正在准备角色");
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -22);
    bsp_display_unlock();
    ESP_LOGI(TAG, "Elysia animation prototype ready; USB commands: STATUS, NEXT, TONE");
    assert(xTaskCreatePinnedToCore(animation_task, "animation", 8192, NULL, 3, NULL, 1) == pdPASS);
    assert(xTaskCreate(command_task, "commands", 6144, NULL, 2, NULL) == pdPASS);
}
