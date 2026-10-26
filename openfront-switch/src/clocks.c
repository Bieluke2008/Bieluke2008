// Runs the CPU at 1785 MHz, the top of Nintendo's own clock table (the
// "boost" games get while loading) instead of the 1020 MHz default. The
// engine is a JS interpreter, so its tick time scales with the CPU clock.
//
// clkrst/pcv set the CPU alone; appletSetCpuBoostMode, the fallback, also
// drops the GPU to 76.8 MHz and only works when launched as an application.
#include "clocks.h"

#ifdef __SWITCH__
#include <switch.h>

#define BOOST_HZ 1785000000u

static bool use_clkrst, use_pcv, use_applet;
static ClkrstSession session;
static u32 old_hz;

static u32 current_hz(void) {
    u32 hz = 0;
    if (use_clkrst) clkrstGetClockRate(&session, &hz);
    else if (use_pcv) pcvGetClockRate(PcvModule_CpuBus, &hz);
    return hz;
}

static void set_hz(u32 hz) {
    if (use_clkrst) clkrstSetClockRate(&session, hz);
    else if (use_pcv) pcvSetClockRate(PcvModule_CpuBus, hz);
}

void clocks_boost(void) {
    if (hosversionAtLeast(8, 0, 0)) {
        if (R_SUCCEEDED(clkrstInitialize())) {
            if (R_SUCCEEDED(clkrstOpenSession(&session, PcvModuleId_CpuBus, 3))) use_clkrst = true;
            else clkrstExit();
        }
    } else if (R_SUCCEEDED(pcvInitialize())) {
        use_pcv = true;
    }
    if (use_clkrst || use_pcv) {
        old_hz = current_hz();
        set_hz(BOOST_HZ);
        if (current_hz() >= BOOST_HZ) return;
        if (use_clkrst) clkrstCloseSession(&session), clkrstExit();
        if (use_pcv) pcvExit();
        use_clkrst = use_pcv = false;
    }
    use_applet = R_SUCCEEDED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad));
}

void clocks_poll(void) {
    if ((use_clkrst || use_pcv) && current_hz() < BOOST_HZ) set_hz(BOOST_HZ);
}

void clocks_restore(void) {
    if (use_clkrst || use_pcv) {
        if (old_hz) set_hz(old_hz);
        if (use_clkrst) clkrstCloseSession(&session), clkrstExit();
        if (use_pcv) pcvExit();
        use_clkrst = use_pcv = false;
    }
    if (use_applet) appletSetCpuBoostMode(ApmCpuBoostMode_Normal), use_applet = false;
}

#else

void clocks_boost(void) {}
void clocks_poll(void) {}
void clocks_restore(void) {}

#endif
