#ifndef HTP_IDS_H
#define HTP_IDS_H
#include <stdint.h>
/* epoch_s > 0  -> "c-YYYYMMDD-HHMMSS-xxxx" (UTC civil date)
 * epoch_s <= 0 -> "c-b<bootcount>-<mono_ms>-xxxx"  (RTC never set)
 * xxxx = rand2 as 4 lowercase hex chars. Output fits the bridge's
 * accepted alphabet [A-Za-z0-9_-]{1,128}. */
void htp_make_capture_id(char out[64], long long epoch_s, uint32_t bootcount,
                         unsigned mono_ms, const uint8_t rand2[2]);
#endif
