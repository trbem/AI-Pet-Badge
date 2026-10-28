/* Deterministic replay. Uses local state only; never injects movement into the live UI. */
#include "badge_motion.h"
#include <stdio.h>
#ifndef BADGE_HOST_MOTION_SELFTEST
#include "badge_imu_filter.h"
#include <math.h>
#endif

#define CHECK(condition, name) do { \
    if (!(condition)) { ++failures; printf("BADGE_TEST FAIL %s\n",name); } \
    else printf("BADGE_TEST PASS %s\n",name); \
} while (0)

#ifndef BADGE_HOST_MOTION_SELFTEST
static void still(badge_imu_filter_t *s, int64_t *now, int count, bool inhibited)
{
    const float a[3]={0,-1,0},w[3]={0,0,0};
    while (count--) { *now+=10000; badge_imu_filter_step(s,a,w,*now,inhibited); }
}
static unsigned shake(badge_imu_filter_t *s, int64_t *now, bool inhibited)
{
    unsigned before=s->shakes;
    float a[3]={1.8f,-1,0},w[3]={0,0,0};
    for (int i=0;i<18;++i) {
        a[0]=i<8?1.8f:-1.8f;
        *now+=10000; badge_imu_filter_step(s,a,w,*now,inhibited);
    }
    return s->shakes-before;
}

int badge_imu_filter_selftest(void)
{
    int failures=0;
    badge_imu_filter_t f;
    int64_t now=1000000;
    badge_imu_filter_init(&f,180);
    still(&f,&now,100,false);
    CHECK(f.stable && !f.flat && fabsf(f.angle)<0.6f,"upright stable");
    for (int i=1;i<=720;++i) {
        float phi=i*0.5f*0.01745329252f;
        float a[3]={-sinf(phi),-cosf(phi),0},w[3]={0,0,0.872664626f};
        float previous=f.angle;
        now+=10000; badge_imu_filter_step(&f,a,w,now,false);
        if (fabsf(badge_imu_angle_delta(f.angle,previous))>5) ++failures;
    }
    printf("BADGE_TEST rotation_lag_deg %.3f expected 6.0 (50 dps * 120 ms)\n",f.angle);
    CHECK(fabsf(badge_imu_angle_delta(f.angle,6))<1.5f,"continuous 360 crossing and smoothing lag");
    CHECK(f.shakes==0,"slow rotation does not shake");
    still(&f,&now,180,false);
    CHECK(fabsf(f.angle)<0.6f && fabsf(f.dx)<0.1f && fabsf(f.lean)<0.1f,"settled pose returns neutral");
    badge_imu_filter_init(&f,0);
    f.angle=73;
    float a[3]={0,0,1},w[3]={0,0,0};
    for (int i=0;i<200;++i) { now+=10000; badge_imu_filter_step(&f,a,w,now,false); }
    CHECK(f.flat && f.angle==73,"flat holds last orientation");
    badge_imu_filter_lost(&f);
    CHECK(f.angle==73 && !f.initialized,"disconnect preserves angle");
    badge_imu_filter_init(&f,180);
    still(&f,&now,100,false);
    CHECK(shake(&f,&now,false)==1,"opposite peaks trigger once");
    CHECK(shake(&f,&now,true)==0,"dizzy suppresses repeat shake");
    still(&f,&now,100,false);
    CHECK(shake(&f,&now,false)==0,"recovery cooldown blocks shake");
    still(&f,&now,250,false);
    CHECK(shake(&f,&now,false)==1,"quiet cooldown rearms");
    badge_imu_filter_init(&f,180);
    still(&f,&now,100,false);
    a[0]=1.8f;a[1]=-1;a[2]=0;
    for (int i=0;i<8;++i) { now+=10000; badge_imu_filter_step(&f,a,w,now,false); }
    still(&f,&now,100,false);
    CHECK(f.shakes==0,"single pickup impulse does not shake");
    now+=2000000;
    a[0]=0; a[1]=-1;
    badge_imu_filter_step(&f,a,w,now,false);
    CHECK(isfinite(f.angle) && fabsf(f.angle)<0.6f,"long sample gap resets estimator safely");
    badge_imu_filter_init(&f,180);
    still(&f,&now,100,false);
    for (int i=1;i<=180;++i) {
        float p=i*0.5f*0.01745329252f;
        a[0]=0;a[1]=-cosf(p);a[2]=sinf(p);w[0]=0.872664626f;
        now+=10000;badge_imu_filter_step(&f,a,w,now,false);
    }
    CHECK(f.flat && f.shakes==0,"tilt into flat locks without false shake");
    return failures;
}

#endif

int badge_motion_selftest(void)
{
    int failures=0;
    const uint32_t counts[3]={106,106,36};
    badge_motion_t m;
    badge_motion_init(&m,2,counts,24);
    badge_motion_tick(&m,1000000,BADGE_NO_ACTION,false);
    CHECK(m.stage==MOTION_IDLE && m.frame==35,"seated idle has no autoplay");
    badge_motion_tick(&m,1010000,0,false);
    CHECK(m.stage==MOTION_STAND_UP,"tap starts standing transition");
    badge_motion_tick(&m,1020000,1,false);
    CHECK(m.pending==1,"latest touch queued");
    badge_motion_tick(&m,1030000,0,true);
    CHECK(m.stage==MOTION_DIZZY_ENTER && m.pending==BADGE_NO_ACTION && m.clip==2 && m.frame==35,
          "shake cancels active and queued action");
    CHECK(badge_motion_blend(&m,1030000)==0 && badge_motion_blend(&m,1280000)==1,
          "dizzy transition has smooth endpoints");
    uint32_t epoch=m.blend_epoch;
    badge_motion_tick(&m,1400000,0,true);
    CHECK(m.stage==MOTION_DIZZY && m.pending==BADGE_NO_ACTION && m.blend_epoch==epoch,
          "dizzy ignores taps and repeated shakes");
    badge_motion_tick(&m,4030000,1,false);
    CHECK(m.stage==MOTION_RECOVER,"dizzy lasts three seconds");
    badge_motion_tick(&m,4430000,1,true);
    CHECK(m.stage==MOTION_IDLE && m.pending==BADGE_NO_ACTION && m.completed==0,
          "recovery returns seated without cancelled action");
    badge_motion_tick(&m,4440000,1,false);
    for (int i=1;i<=220;++i) badge_motion_tick(&m,4440000+i*50000LL,BADGE_NO_ACTION,false);
    CHECK(m.stage==MOTION_IDLE && m.completed==1 && m.frame==35,"normal jump completes seated");
    const uint32_t standing_counts[3]={26,20,19};
    badge_motion_init(&m,2,standing_counts,24);
    m.standing_idle=true;
    badge_motion_tick(&m,1000000,BADGE_NO_ACTION,false);
    CHECK(m.stage==MOTION_IDLE && m.frame==18,"standing idle has no autoplay");
    for (unsigned action=0;action<3;++action) {
        int64_t start=2000000+action*3000000LL;
        badge_motion_tick(&m,start,action,false);
        CHECK(m.stage==MOTION_BLEND_ACTION && m.clip==action,"standing action blends directly from neutral");
        bool frames_valid=true;
        for (int i=1;i<=50;++i) {
            badge_motion_tick(&m,start+i*50000LL,BADGE_NO_ACTION,false);
            if (m.frame>=m.count[m.clip]) frames_valid=false;
            if (m.stage==MOTION_IDLE) break;
        }
        CHECK(frames_valid,"standing frames stay inside clip");
        CHECK(m.stage==MOTION_IDLE && m.clip==2 && m.frame==18 && m.completed==action+1,
              "standing action returns to static neutral");
    }
    badge_motion_tick(&m,12000000,0,false);
    badge_motion_tick(&m,12010000,1,false);
    badge_motion_tick(&m,12020000,BADGE_NO_ACTION,true);
    CHECK(m.stage==MOTION_DIZZY_ENTER && m.pending==BADGE_NO_ACTION && m.frame==18,
          "standing shake cancels active and queued action");
    badge_motion_tick(&m,12300000,2,false);
    badge_motion_tick(&m,15020000,2,false);
    badge_motion_tick(&m,15420000,2,false);
    CHECK(m.stage==MOTION_IDLE && m.pending==BADGE_NO_ACTION && m.completed==3 && m.frame==18,
          "standing dizzy recovers without resuming cancelled action");
    return failures;
}

#ifdef BADGE_HOST_SELFTEST
int main(void) { return badge_imu_filter_selftest()+badge_motion_selftest(); }
#elif defined(BADGE_HOST_MOTION_SELFTEST)
int main(void) { return badge_motion_selftest(); }
#endif
