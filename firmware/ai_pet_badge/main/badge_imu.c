#include "badge_imu.h"
#include "badge_imu_filter.h"
#include "bsp/esp-bsp.h"
#include "qmi8658.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "badge_imu";
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static badge_imu_state_t state = {.flat = true};
static bool inhibit, calibrate;

void badge_imu_get(badge_imu_state_t *out)
{
    portENTER_CRITICAL(&mux); *out = state; portEXIT_CRITICAL(&mux);
}
void badge_imu_inhibit(bool value)
{
    portENTER_CRITICAL(&mux); inhibit = value; portEXIT_CRITICAL(&mux);
}
void badge_imu_calibrate(void)
{
    portENTER_CRITICAL(&mux); calibrate = true; portEXIT_CRITICAL(&mux);
    printf("BADGE_IMU_CALIBRATION requested\n");
}
void badge_imu_status(void)
{
    badge_imu_state_t s; badge_imu_get(&s);
    printf("BADGE_IMU {\"available\":%s,\"flat\":%s,\"stable\":%s,\"calibrated\":%s,"
           "\"angle\":%.2f,\"dx\":%.2f,\"dy\":%.2f,\"lean\":%.2f,"
           "\"accel_g\":[%.4f,%.4f,%.4f],\"gyro_rads\":[%.4f,%.4f,%.4f],"
           "\"gravity\":[%.4f,%.4f,%.4f],\"shakes\":%u,\"errors\":%u}\n",
           s.available?"true":"false", s.flat?"true":"false", s.stable?"true":"false",
           s.calibrated?"true":"false", s.angle, s.dx, s.dy, s.lean,
           s.accel[0],s.accel[1],s.accel[2],s.gyro[0],s.gyro[1],s.gyro[2],
           s.gravity[0],s.gravity[1],s.gravity[2],(unsigned)s.shakes,(unsigned)s.errors);
}

static esp_err_t connect(qmi8658_dev_t *dev)
{
    if (dev->dev_handle) { i2c_master_bus_rm_device(dev->dev_handle); dev->dev_handle = NULL; }
    esp_err_t err = qmi8658_init(dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH);
    if (err == ESP_OK) err = qmi8658_set_accel_range(dev, QMI8658_ACCEL_RANGE_8G);
    if (err == ESP_OK) err = qmi8658_set_gyro_range(dev, QMI8658_GYRO_RANGE_1024DPS);
    if (err == ESP_OK) err = qmi8658_set_accel_odr(dev, QMI8658_ACCEL_ODR_125HZ);
    if (err == ESP_OK) err = qmi8658_set_gyro_odr(dev, QMI8658_GYRO_ODR_125HZ);
    if (err == ESP_OK) err = qmi8658_enable_sensors(dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    qmi8658_set_accel_unit_mps2(dev, true);
    qmi8658_set_gyro_unit_rads(dev, true);
    return err;
}

static void imu_task(void *arg)
{
    (void)arg;
    badge_imu_filter_t f;
    qmi8658_dev_t dev = {0};
    nvs_handle_t nvs;
    float zero = 0;
    bool calibrated = false;
    if (nvs_open("badge_imu", NVS_READONLY, &nvs) == ESP_OK) {
        size_t length = sizeof(zero);
        calibrated = nvs_get_blob(nvs, "zero", &zero, &length) == ESP_OK && length == sizeof(zero) && isfinite(zero);
        if (!calibrated) zero = 0;
        nvs_close(nvs);
    }
    badge_imu_filter_init(&f, zero);
    bool ready = false;
    int failures = 0, calibration_count = 0;
    float sum_x = 0, sum_y = 0, calibration_gravity[3] = {0};
    int calibration_unstable = 0;
    int64_t calibration_wait_until = 0;
    int64_t retry_at = 0;
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        int64_t now = esp_timer_get_time();
        bool blocked, requested;
        portENTER_CRITICAL(&mux);
        blocked = inhibit; requested = calibrate; calibrate = false;
        portEXIT_CRITICAL(&mux);
        if (!ready && now >= retry_at) {
            ready = connect(&dev) == ESP_OK;
            retry_at = now + 5000000;
            if (ready) { failures = 0; ESP_LOGI(TAG, "QMI8658 ready: 8g, 1024dps, 125Hz, sampling 100Hz"); }
            else { portENTER_CRITICAL(&mux); ++state.errors; portEXIT_CRITICAL(&mux); }
        }
        float a[3], w[3];
        if (ready) {
            esp_err_t err = qmi8658_read_accel(&dev, &a[0], &a[1], &a[2]);
            if (err == ESP_OK) err = qmi8658_read_gyro(&dev, &w[0], &w[1], &w[2]);
            if (err != ESP_OK) {
                portENTER_CRITICAL(&mux); ++state.errors; portEXIT_CRITICAL(&mux);
                if (++failures >= 5) {
                    ready = false; retry_at = now + 5000000;
                    badge_imu_filter_lost(&f);
                    ESP_LOGW(TAG, "Sensor unavailable; preserving screen orientation");
                }
            } else {
                failures = 0;
                for (int i = 0; i < 3; ++i) a[i] /= ONE_G;
                badge_imu_filter_step(&f, a, w, now, blocked);
            }
        }
        if (requested) {
            calibration_wait_until = now+10000000;
            calibration_count = 0;
            printf("BADGE_IMU_CALIBRATION waiting for upright stable window (10s)\n");
        }
        if (calibration_wait_until) {
            if (ready && !failures && f.stable && hypotf(f.gravity[0],f.gravity[1]) > 0.5f) {
                calibration_count = 1; sum_x = sum_y = 0; calibration_unstable = 0;
                memcpy(calibration_gravity,f.gravity,sizeof(calibration_gravity));
                calibration_wait_until = 0;
            } else if (now >= calibration_wait_until) {
                printf("BADGE_IMU_CALIBRATION rejected: no upright stable window; previous value retained\n");
                calibration_wait_until = 0;
            }
        }
        if (calibration_count) {
            float speed = sqrtf(f.gyro[0]*f.gyro[0]+f.gyro[1]*f.gyro[1]+f.gyro[2]*f.gyro[2]);
            float linear = sqrtf(f.linear[0]*f.linear[0]+f.linear[1]*f.linear[1]+f.linear[2]*f.linear[2]);
            float dot = 0;
            for (int i=0;i<3;++i) dot += calibration_gravity[i]*f.gravity[i];
            if (f.motion_speed > 0.25f || linear > 0.18f) ++calibration_unstable;
            if (!ready || failures || speed > 0.6f || calibration_unstable > 20 || dot < 0.998629535f ||
                hypotf(f.gravity[0],f.gravity[1]) <= 0.5f) {
                calibration_count = 0;
                printf("BADGE_IMU_CALIBRATION rejected: moved/flat/read failure (speed=%.3f, unstable=%d, dot=%.5f); previous value retained\n",
                       speed,calibration_unstable,dot);
            } else {
                sum_x += f.gravity[0]; sum_y += f.gravity[1];
                if (++calibration_count > 100) {
                    float candidate = atan2f(-sum_x,sum_y)*57.295779513f;
                    esp_err_t err = nvs_open("badge_imu", NVS_READWRITE, &nvs);
                    if (err == ESP_OK) {
                        err = nvs_set_blob(nvs,"zero",&candidate,sizeof(candidate));
                        if (err == ESP_OK) err = nvs_commit(nvs);
                        nvs_close(nvs);
                    }
                    if (err == ESP_OK) {
                        f.zero = candidate; calibrated = true;
                        printf("BADGE_IMU_CALIBRATION saved %.3f\n", candidate);
                    } else printf("BADGE_IMU_CALIBRATION failed: NVS %s\n", esp_err_to_name(err));
                    calibration_count = 0;
                }
            }
        }
        if (!ready) { f.dx *= 0.9f; f.dy *= 0.9f; f.lean *= 0.9f; }
        portENTER_CRITICAL(&mux);
        state.available = ready; state.flat = f.flat; state.stable = ready && f.stable;
        state.calibrated = calibrated; state.angle = f.angle;
        state.dx = f.dx; state.dy = f.dy; state.lean = f.lean;
        memcpy(state.accel,f.accel,sizeof(state.accel)); memcpy(state.gyro,f.gyro,sizeof(state.gyro));
        memcpy(state.gravity,f.gravity,sizeof(state.gravity)); state.shakes = f.shakes;
        state.sample_us = f.last_us;
        portEXIT_CRITICAL(&mux);
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    }
}

void badge_imu_start(void)
{
    configASSERT(xTaskCreatePinnedToCore(imu_task,"badge_imu",4096,NULL,5,NULL,0) == pdPASS);
}
