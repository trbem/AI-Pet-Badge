#include "badge_imu_filter.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RAD_TO_DEG 57.295779513f
static float clampf(float x, float lo, float hi) { return fminf(hi, fmaxf(lo, x)); }
static float length3(const float *v) { return sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]); }

float badge_imu_angle_delta(float target, float current)
{
    return remainderf(target - current, 360.0f);
}

float badge_imu_gravity_angle(const badge_imu_filter_t *s)
{
    return atan2f(-s->gravity[0], s->gravity[1]) * RAD_TO_DEG;
}

void badge_imu_filter_init(badge_imu_filter_t *s, float zero)
{
    memset(s, 0, sizeof(*s));
    s->zero = zero;
    s->flat = true;
}

void badge_imu_filter_lost(badge_imu_filter_t *s)
{
    s->initialized = s->stable = s->armed = false;
    s->flat = true;
    s->stable_since = s->resume_since = s->last_us = 0;
    memset(s->peak_sign, 0, sizeof(s->peak_sign));
    /* Retain last presentation angle across disconnect/reconnect. */
}

bool badge_imu_filter_step(badge_imu_filter_t *s, const float a[3], const float w[3],
                           int64_t now, bool inhibited)
{
    float norm = length3(a);
    if (!isfinite(norm) || norm < 0.01f || !isfinite(length3(w))) return false;
    memcpy(s->accel, a, sizeof(s->accel));
    memcpy(s->gyro, w, sizeof(s->gyro));
    float dt = s->last_us ? (now - s->last_us) * 0.000001f : 0.01f;
    if (dt <= 0 || dt > 0.1f) { badge_imu_filter_lost(s); dt = 0.01f; }
    s->last_us = now;
    if (!s->initialized) {
        for (int i = 0; i < 3; ++i) s->gravity[i] = a[i] / norm;
        s->neutral[0] = atan2f(s->gravity[0], hypotf(s->gravity[1], s->gravity[2]));
        s->neutral[1] = atan2f(s->gravity[2], hypotf(s->gravity[0], s->gravity[1]));
        s->initialized = true;
        s->rearm_at = now + 500000;
    }
    float gx = s->gravity[0], gy = s->gravity[1], gz = s->gravity[2];
    float wx = w[0]-s->bias[0], wy = w[1]-s->bias[1], wz = w[2]-s->bias[2];
    s->gravity[0] += (wz*gy-wy*gz)*dt;
    s->gravity[1] += (wx*gz-wz*gx)*dt;
    s->gravity[2] += (wy*gx-wx*gy)*dt;
    float correction = (norm > 0.85f && norm < 1.15f && now >= s->reject_accel_until)
                       ? dt / (0.5f + dt) : 0;
    for (int i = 0; i < 3; ++i)
        s->gravity[i] += correction * (a[i]/norm - s->gravity[i]);
    float gl = length3(s->gravity);
    for (int i = 0; i < 3; ++i) {
        s->gravity[i] /= gl;
        s->linear[i] += dt/(0.015f+dt) * (a[i]-s->gravity[i]-s->linear[i]);
    }
    float speed = sqrtf(wx*wx+wy*wy+wz*wz);
    s->motion_speed += dt/(0.05f+dt)*(speed-s->motion_speed);
    s->stable = fabsf(norm-1) < 0.12f && s->motion_speed < 0.12f && length3(s->linear) < 0.12f;
    if (!s->stable) s->stable_since = 0;
    else if (!s->stable_since) s->stable_since = now;
    bool settled = s->stable_since && now-s->stable_since >= 500000;
    if (s->stable_since && now-s->stable_since >= 1000000) {
        for (int i = 0; i < 3; ++i) s->bias[i] += dt/(2.0f+dt)*(w[i]-s->bias[i]);
    }
    if (inhibited || s->inhibited) {
        s->armed = false;
        s->rearm_at = now + 2000000;
        memset(s->peak_sign, 0, sizeof(s->peak_sign));
    }
    s->inhibited = inhibited;
    if (!inhibited && settled && now >= s->rearm_at) s->armed = true;
    bool shake = false;
    if (s->armed) {
        for (int i = 0; i < 3; ++i) {
            if (fabsf(s->linear[i]) < 0.8f) continue;
            int sign = s->linear[i] > 0 ? 1 : -1;
            int64_t age = now - s->peak_us[i];
            if (s->peak_sign[i] == -sign && age >= 40000 && age <= 600000) {
                shake = true;
                break;
            }
            if (s->peak_sign[i] != sign || age > 600000) {
                s->peak_sign[i] = sign;
                s->peak_us[i] = now;
            }
        }
    }
    if (shake) {
        ++s->shakes;
        s->armed = false;
        s->rearm_at = now + 2000000;
        s->reject_accel_until = now + 250000;
        memset(s->peak_sign, 0, sizeof(s->peak_sign));
    }
    float projection = hypotf(s->gravity[0], s->gravity[1]);
    if (projection < 0.25f) { s->flat = true; s->resume_since = 0; }
    if (s->flat && projection > 0.35f) {
        if (!s->resume_since) s->resume_since = now;
        if (now-s->resume_since >= 200000) s->flat = false;
    } else if (projection <= 0.35f) s->resume_since = 0;
    if (!s->flat) {
        float delta = badge_imu_angle_delta(badge_imu_gravity_angle(s)-s->zero, s->angle);
        if (fabsf(delta) > 0.5f)
            s->angle += clampf(delta * dt/(0.12f+dt), -360*dt, 360*dt);
        /* Bound accumulation while preserving wrap-safe deltas. */
        s->angle = remainderf(s->angle, 360.0f);
    }
    float tilt[2] = {atan2f(s->gravity[0], hypotf(s->gravity[1], s->gravity[2])),
                     atan2f(s->gravity[2], hypotf(s->gravity[0], s->gravity[1]))};
    for (int i = 0; i < 2; ++i) s->neutral[i] += dt/(0.8f+dt)*(tilt[i]-s->neutral[i]);
    float dx = settled ? 0 : clampf((tilt[0]-s->neutral[0])*12, -4, 4);
    float dy = settled ? 0 : clampf((tilt[1]-s->neutral[1])*10, -3, 3);
    float lean = settled ? 0 : clampf((tilt[0]-s->neutral[0])*10-wz*2, -3, 3);
    float easing = dt/(0.08f+dt);
    s->dx += easing*(dx-s->dx); s->dy += easing*(dy-s->dy); s->lean += easing*(lean-s->lean);
    return shake;
}
