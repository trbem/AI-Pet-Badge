#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
#include "badge_dashboard.h"
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
static lv_image_dsc_t images[2];
static uint8_t *pixels[2];
static uint8_t *transition_from;
typedef enum {
    MOTION_IDLE, MOTION_STAND_UP, MOTION_BLEND_ACTION, MOTION_ACTION,
    MOTION_BLEND_SIT, MOTION_SIT_DOWN, MOTION_STAND_PAUSE
} motion_stage_t;
static const char *const stage_names[] = {
    "idle", "stand_up", "blend_to_action", "action", "blend_to_sit", "sit_down", "stand_pause"
};
static portMUX_TYPE request_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t idle_clip = UINT32_MAX;
static uint32_t next_action_clip;
static TaskHandle_t animation_task_handle;
static volatile uint32_t requested_clip;
static volatile uint32_t clip_revision;
static volatile uint32_t current_clip;
static volatile uint32_t current_frame;
static volatile bool animation_playing;
static volatile motion_stage_t current_stage = MOTION_IDLE;
static volatile uint32_t active_action;
static volatile uint32_t pending_clip = UINT32_MAX;
static volatile uint32_t completed_actions;
static volatile uint32_t late_frames;
static volatile uint32_t touch_count;
static volatile uint32_t displayed_frames;
static volatile uint32_t decode_time_us;
static volatile uint32_t render_time_us;
static volatile uint32_t peak_render_time_us;
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
        if (!memchr(clips[i].name, 0, sizeof(clips[i].name)) || clips[i].count < 2 ||
            clips[i].first >= pack->frame_count || clips[i].count > pack->frame_count - clips[i].first)
            return ESP_ERR_INVALID_SIZE;
        if (!strcmp(clips[i].name, "sit")) idle_clip = i;
    }
    if (idle_clip == UINT32_MAX) return ESP_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < pack->frame_count; ++i) {
        if (!frames[i].length || frames[i].offset < table_end || frames[i].offset > length ||
            frames[i].length > length - frames[i].offset) return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "Assets validated: %u clips, %u frames, %u fps, %u bytes",
             (unsigned)pack->clip_count, (unsigned)pack->frame_count, (unsigned)pack->fps, (unsigned)length);
    return ESP_OK;
}

static uint32_t request_next_action(void)
{
    portENTER_CRITICAL(&request_mux);
    uint32_t selected = next_action_clip;
    requested_clip = selected;
    next_action_clip = (next_action_clip + 1) % pack->clip_count;
    ++clip_revision;
    portEXIT_CRITICAL(&request_mux);
    if (animation_task_handle) xTaskNotifyGive(animation_task_handle);
    return selected;
}

static void on_touch(lv_event_t *event)
{
    (void)event;
    ++touch_count;
    uint32_t selected = request_next_action();
    ESP_LOGI(TAG, "Touch #%u -> queued %s", (unsigned)touch_count, clips[selected].name);
}

static void print_status(void)
{
    uint32_t queued = pending_clip;
    printf("BADGE_STATUS {\"clip\":\"%s\",\"playing\":%s,\"stage\":\"%s\",\"action\":\"%s\",\"pending\":\"%s\","
           "\"completed\":%u,\"late_frames\":%u,\"frame_index\":%u,\"frames\":%u,\"touches\":%u,\"decode_us\":%u,"
           "\"render_us\":%u,\"peak_render_us\":%u,"
           "\"heap\":%u,\"min_heap\":%u,\"psram\":%u}\n",
           clips[current_clip].name, animation_playing ? "true" : "false", stage_names[current_stage],
           clips[active_action].name, queued < pack->clip_count ? clips[queued].name : "",
           (unsigned)completed_actions, (unsigned)late_frames, (unsigned)current_frame,
           (unsigned)displayed_frames, (unsigned)touch_count,
           (unsigned)decode_time_us, (unsigned)render_time_us, (unsigned)peak_render_time_us,
           (unsigned)esp_get_free_heap_size(),
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
            else if (!strcmp(line, "NEXT")) { request_next_action(); print_status(); }
            else if (!strcmp(line, "TONE")) diagnostic_tone();
            else if (!strcmp(line, "DASH")) badge_dashboard_status();
            else if (!strcmp(line, "SHOT")) badge_dashboard_snapshot();
            else if (!strncmp(line, "TIME ", 5)) {
                badge_dashboard_set_time((time_t)strtoll(line + 5, NULL, 10));
                badge_dashboard_status();
            }
            else if (used) printf("BADGE_ERROR unknown command\n");
            used = 0;
        } else if (byte >= 32 && byte < 127) {
            if (used < sizeof(line) - 1) line[used++] = byte;
            else used = 0;
        }
    }
}

static void blend_frame(uint8_t *destination, unsigned step, unsigned count)
{
    unsigned t = step * 256 / count;
    unsigned alpha = t * t * (768 - 2 * t) / 65536;
    const uint16_t *source = (const uint16_t *)transition_from;
    uint16_t *target = (uint16_t *)destination;
    for (size_t i = 0; i < FRAME_W * FRAME_H; ++i) {
        uint16_t a = source[i], b = target[i];
        // The canvas is mostly identical background; it already contains b.
        if (a == b) continue;
        unsigned red = (((a >> 11) * (256 - alpha) + (b >> 11) * alpha + 128) >> 8);
        unsigned green = ((((a >> 5) & 63) * (256 - alpha) + ((b >> 5) & 63) * alpha + 128) >> 8);
        unsigned blue = (((a & 31) * (256 - alpha) + (b & 31) * alpha + 128) >> 8);
        target[i] = (red << 11) | (green << 5) | blue;
    }
}

static void set_motion_label(bool playing)
{
    badge_dashboard_motion(playing);
}

static void animation_task(void *arg)
{
    (void)arg;
    tinfl_decompressor *inflater = heap_caps_malloc(sizeof(*inflater), MALLOC_CAP_8BIT);
    assert(inflater);
    uint32_t selected = idle_clip;
    uint32_t index = clips[idle_clip].count - 1, revision = 0;
    uint32_t queued = UINT32_MAX;
    motion_stage_t stage = MOTION_IDLE;
    bool needs_frame = true;
    unsigned blend_step = 1, pause_left = 0;
    const unsigned blend_frames = (pack->fps + 2) / 4;
    unsigned slot = 0;
    int64_t deadline = esp_timer_get_time();
    set_motion_label(false);
    while (true) {
        // Keep only the latest pending request; an in-progress action always
        // reaches its seated endpoint before another action can start.
        portENTER_CRITICAL(&request_mux);
        if (revision != clip_revision) {
            queued = requested_clip;
            revision = clip_revision;
        }
        portEXIT_CRITICAL(&request_mux);
        pending_clip = queued;
        badge_dashboard_pending(queued != UINT32_MAX);
        if (stage == MOTION_IDLE && queued != UINT32_MAX) {
            active_action = queued;
            queued = pending_clip = UINT32_MAX;
            selected = idle_clip;
            index = clips[idle_clip].count - 2;
            stage = MOTION_STAND_UP;
            needs_frame = true;
            deadline = esp_timer_get_time();
            set_motion_label(true);
            ESP_LOGI(TAG, "Motion %s: stand_up", clips[active_action].name);
        }
        if (!needs_frame) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const pack_clip_t *clip = &clips[selected];
        const pack_frame_t *frame = &frames[clip->first + index];
        bool blending = stage == MOTION_BLEND_ACTION || stage == MOTION_BLEND_SIT;
        if (blending && blend_step == 1) memcpy(transition_from, pixels[slot ^ 1], FRAME_BYTES);
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
        if (blending) blend_frame(pixels[slot], blend_step, blend_frames);
        bsp_display_lock(-1);
        lv_image_cache_drop(&images[slot]);
        lv_image_set_src(character_image, &images[slot]);
        lv_obj_invalidate(character_image);
        bsp_display_unlock();
        render_time_us = esp_timer_get_time() - started;
        if (render_time_us > peak_render_time_us) peak_render_time_us = render_time_us;
        current_clip = selected;
        current_frame = index;
        animation_playing = stage != MOTION_IDLE;
        current_stage = stage;
        slot ^= 1;
        ++displayed_frames;
        motion_stage_t previous_stage = stage;
        switch (stage) {
        case MOTION_IDLE:
            needs_frame = false;
            break;
        case MOTION_STAND_UP:
            if (index) --index;
            else if (active_action == idle_clip) {
                stage = MOTION_STAND_PAUSE;
                pause_left = blend_frames;
            } else {
                stage = MOTION_BLEND_ACTION;
                selected = active_action;
                index = 0;
                blend_step = 1;
            }
            break;
        case MOTION_STAND_PAUSE:
            if (!--pause_left) { stage = MOTION_SIT_DOWN; index = 1; }
            break;
        case MOTION_BLEND_ACTION:
            if (++blend_step > blend_frames) { stage = MOTION_ACTION; index = 1; }
            break;
        case MOTION_ACTION:
            if (++index >= clip->count) {
                stage = MOTION_BLEND_SIT;
                selected = idle_clip;
                index = 0;
                blend_step = 1;
            }
            break;
        case MOTION_BLEND_SIT:
            if (++blend_step > blend_frames) { stage = MOTION_SIT_DOWN; index = 1; }
            break;
        case MOTION_SIT_DOWN:
            if (++index >= clip->count) {
                stage = current_stage = MOTION_IDLE;
                animation_playing = false;
                needs_frame = false;
                ++completed_actions;
                set_motion_label(false);
                ESP_LOGI(TAG, "Motion %s completed; seated idle, frame updates paused", clips[active_action].name);
            }
            break;
        }
        if (stage != previous_stage && stage != MOTION_IDLE) {
            ESP_LOGI(TAG, "Motion %s: %s", clips[active_action].name, stage_names[stage]);
        }
        int64_t now = esp_timer_get_time();
        deadline += 1000000 / pack->fps;
        if (needs_frame) {
            if (deadline > now) {
                TickType_t delay = pdMS_TO_TICKS((deadline - now) / 1000);
                vTaskDelay(delay ? delay : 1);
            } else {
                ++late_frames;
                deadline = now;
                vTaskDelay(1);
            }
        }
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
    requested_clip = current_clip = idle_clip;
    active_action = idle_clip;
    current_frame = clips[idle_clip].count - 1;
    for (int i = 0; i < 2; ++i) {
        pixels[i] = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(pixels[i]);
        images[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = FRAME_W, .h = FRAME_H, .stride = FRAME_W * 2},
            .data_size = FRAME_BYTES, .data = pixels[i]
        };
    }
    transition_from = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(transition_from);
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
    badge_dashboard_create(screen);
    bsp_display_unlock();
    badge_dashboard_start();
    ESP_LOGI(TAG, "Elysia dashboard ready; USB commands: STATUS, NEXT, TONE, DASH, SHOT, TIME <epoch>");
    assert(xTaskCreatePinnedToCore(animation_task, "animation", 8192, NULL, 3, &animation_task_handle, 1) == pdPASS);
    assert(xTaskCreate(command_task, "commands", 12288, NULL, 2, NULL) == pdPASS);
    badge_network_start();
}
