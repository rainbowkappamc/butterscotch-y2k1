//* animalcrossing.h
//* Animal Crossing N64 port — top-level entry point.
 

#ifndef ANIMALCROSSING_H
#define ANIMALCROSSING_H

#include <libdragon.h>

/*
 * rtc_mode — set during platform_init().
 *   0 = libdragon hardware RTC (TC8521 present and readable)
 *   1 = N64_RTC software clock (hardware RTC unavailable)
 */
extern int rtc_mode;

/*
 * ac_main — called from main() after libdragon is fully initialised.
 * Never returns.
 */
void ac_main(void);

#endif /* ANIMALCROSSING_H */
