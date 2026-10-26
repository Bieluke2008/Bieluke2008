#pragma once

// CPU clock and core for the JS engine thread (Switch only; no-ops elsewhere).
void clocks_boost(void);
void clocks_poll(void);  // re-applies it after the system changed it (dock, sleep)
void clocks_restore(void);
void clocks_own_core(void);  // moves the calling thread off the render thread's core
