#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Pure math: acceleration in g, angular velocity in rad/s, monotonic time in us.
 * Board X/Y are screen-plane axes; calibration supplies the upright offset. */
typedef struct {
    float gravity[3], bias[3], linear[3], neutral[2];
    float angle, zero, dx, dy, lean, motion_speed;
    float accel[3], gyro[3];
    int64_t last_us, stable_since, resume_since, rearm_at, reject_accel_until;
    int64_t peak_us[3];
    int peak_sign[3];
    uint32_t shakes;
    bool initialized, flat, stable, armed, inhibited;
} badge_imu_filter_t;

void badge_imu_filter_init(badge_imu_filter_t *s, float zero);
bool badge_imu_filter_step(badge_imu_filter_t *s, const float a[3], const float w[3],
                           int64_t now, bool inhibited);
void badge_imu_filter_lost(badge_imu_filter_t *s);
float badge_imu_gravity_angle(const badge_imu_filter_t *s);
float badge_imu_angle_delta(float target, float current);
int badge_imu_filter_selftest(void);
