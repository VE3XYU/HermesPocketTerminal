# HTP Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Firmware for the Hermes Pocket Terminal (ESP32-S3, board profile `S3_ePaper_1_54`) implementing the full device behavior of the HTP protocol design — capture → upload → poll → reply playback, scheduled sync, dashboard, notifications, Recordings menu, Settings — per `docs/superpowers/specs/2026-08-05-htp-firmware-design.md`.

**Architecture:** Pure ESP-IDF project in `firmware/`. Three pure-C, host-testable components (`htp_client` protocol layer, `app_core` state machines/storage logic, `ui` framebuffer/widgets) reached by hardware only through injected port interfaces; a `board` component (BSP) and `main/` wiring implement those ports on the device. Host tests run on this Linux machine with plain CMake + gcc and consume the bridge's golden contract fixtures. Hardware work lands in stage gates ("checkpoints") where the operator flashes over USB and reports serial output.

**Tech Stack:** ESP-IDF v5.5 (pinned; see Task 12), C11, CMake + CTest for host tests, vendored cJSON (MIT) and font8x8 (public domain), `espressif/esp_codec_dev` via the IDF component registry, `bridge --mock` for integration.

## Global Constraints

Every task's requirements implicitly include all of these.

- **Purity rule:** `components/htp_client`, `components/app_core`, and `components/ui` contain **no ESP-IDF includes** and never call `time()`, `rand()`, `sleep()`, or any OS/hardware API directly. Clocks, RNG, storage, transport, and key-value state are injected via the port structs defined in Task 1/3. Verification (run before every commit that touches them): `grep -rn "esp_\|freertos\|driver/" firmware/components/htp_client/src firmware/components/app_core/src firmware/components/ui/src` must output nothing.
- **Host tests are IDF-free:** `firmware/tests/host` builds with plain `cmake` + `gcc`, no ESP-IDF installed or sourced. All host tests must pass before every commit: `ctest --test-dir firmware/tests/host/build --output-on-failure`.
- **Timestamps** are integer Unix epoch seconds (`long long`), UTC — never floats, never ISO strings. Monotonic time is `unsigned` milliseconds.
- **Fixed-size buffers:** protocol structs use fixed char arrays with safe truncation (`str_copy` from Task 1). Heap is allowed only inside cJSON parsing and transport bodies.
- **Every network request carries a timeout:** `htp_request_t.timeout_ms > 0` always; transports must enforce it.
- **Licensing:** `reference/pala_note/` is consulted for pin maps and init order but **never copied from and never committed**. Third-party code enters only from upstream with license headers intact: cJSON v1.7.18 (MIT), font8x8 (public domain), `esp_codec_dev` via the component manager (Apache-2.0). No Adafruit/GFX font tables, no vendor LUT tables copied from the reference.
- **Public repo hygiene:** no real LAN IPs, hostnames, SSIDs, or tokens in committed files. Examples use `192.0.2.x` (TEST-NET-1) and `<placeholder>` markers. `firmware/sdkconfig` (generated) is gitignored; only `sdkconfig.defaults` is committed.
- **Protocol constants** (from the protocol design and bridge code): capture ID alphabet `[A-Za-z0-9_-]{1,128}`; upload WAV is 16 kHz / 16-bit / mono; max recording 120 s; dashboard ≤ 32 items, item text ≤ 40 chars (server-truncated); every dashboard response carries `sync_interval` seconds; retry rules — 401 never retried, other 4xx never retried, 5xx/network exponential backoff.
- **Commits** use `feat(firmware): ...` / `fix(firmware): ...` / `test(firmware): ...` subjects.
- **Hardware checkpoints (C1–C7)** are steps the session cannot verify itself: the operator flashes the build from their PC and reports serial output and observed behavior. **Stop and wait at every checkpoint; do not proceed on assumption.**

## File Map

```
firmware/
  CMakeLists.txt                      # IDF project file                     (Task 12)
  sdkconfig.defaults                  # target, PSRAM, log defaults          (Task 12)
  partitions.csv                      # nvs / phy / factory                  (Task 12)
  README.md                           # build, flash, provisioning           (Task 19)
  main/
    CMakeLists.txt                                                          (Task 12)
    main.c                            # bring-up harness → final dispatch    (Tasks 12–18)
    idf_ports.c / idf_ports.h         # port_storage/kv/clock/rng on IDF     (Task 14)
    idf_transport.c / idf_transport.h # htp_transport_t on esp_http_client   (Task 16)
    idf_wifi.c / idf_wifi.h           # profiles + BSSID fast join           (Task 16)
  components/
    vendor/cjson/                     # cJSON.c, cJSON.h, LICENSE (MIT)      (Task 1)
    htp_client/
      include/htp_client.h            # types, errors, client API            (Task 3)
      include/htp_ids.h                                                      (Task 1)
      include/htp_backoff.h                                                  (Task 2)
      src/ids.c                       # capture ID generation                (Task 1)
      src/backoff.c                                                          (Task 2)
      src/client.c                    # endpoint calls, JSON parse/build     (Task 3)
      CMakeLists.txt                                                         (Task 1)
    app_core/
      include/ports.h                 # port_storage/clock/rng/kv structs    (Task 1)
      include/util.h                  # str_copy                             (Task 1)
      include/wav.h                                                          (Task 2)
      include/app_config.h            # config.json + wifi.json structs      (Task 4)
      include/sidecar.h                                                      (Task 5)
      include/rec_index.h                                                    (Task 5)
      include/capture_flow.h                                                 (Task 6)
      include/sync.h                                                         (Task 7)
      include/gesture.h                                                      (Task 9)
      include/ui_flow.h                                                      (Task 10)
      src/util.c                                                             (Task 1)
      src/wav.c                                                              (Task 2)
      src/app_config.c                                                       (Task 4)
      src/wifi_select.c                                                      (Task 4)
      src/sidecar.c                                                          (Task 5)
      src/rec_index.c                                                        (Task 5)
      src/capture_flow.c                                                     (Task 6)
      src/sync.c                                                             (Task 7)
      src/gesture.c                                                          (Task 9)
      src/ui_flow.c                                                          (Task 10)
      CMakeLists.txt                                                         (Task 1)
    ui/
      include/ui_fb.h                                                        (Task 8)
      include/ui_widgets.h                                                   (Task 9)
      fonts/font8x8_basic.h           # public domain, vendored              (Task 8)
      src/fb.c                                                               (Task 8)
      src/widgets.c                                                          (Task 9)
      CMakeLists.txt                                                         (Task 8)
    board/
      include/board.h                 # BSP API surface                      (Task 12)
      src/power.c                     # rails, VBAT latch, deep sleep        (Task 12)
      src/buttons.c                                                          (Task 12)
      src/epd_ssd1681.c                                                      (Task 13)
      src/sdcard.c                                                           (Task 14)
      src/audio.c                     # ES8311 record/play/beep              (Task 15)
      src/battery.c                                                          (Task 18)
      src/rtc_pcf8563.c                                                      (Task 17)
      idf_component.yml               # esp_codec_dev dependency             (Task 15)
      CMakeLists.txt                                                         (Task 12)
  tests/host/
    CMakeLists.txt                                                           (Task 1)
    harness.h                                                                (Task 1)
    fakes/fake_transport.c / .h                                              (Task 3)
    fakes/fake_storage.c / .h                                                (Task 5)
    fakes/fake_kv.c / .h                                                     (Task 4)
    fakes/posix_transport.c / .h      # real-socket transport, tests only    (Task 11)
    test_ids.c                                                               (Task 1)
    test_backoff.c / test_wav.c                                              (Task 2)
    test_client.c                                                            (Task 3)
    test_config.c / test_wifi_select.c                                       (Task 4)
    test_sidecar.c / test_rec_index.c                                        (Task 5)
    test_capture_flow.c                                                      (Task 6)
    test_sync.c                                                              (Task 7)
    test_fb.c                                                                (Task 8)
    test_widgets.c / test_gesture.c                                          (Task 9)
    test_ui_flow.c                                                           (Task 10)
    integration/test_mock_bridge.c                                           (Task 11)
  tools/
    setup-idf.sh                                                             (Task 12)
    pack-flash.sh / FLASHING.md                                              (Task 12)
    run-mock.sh                                                              (Task 11)
```

Stages: **A** = host-only TDD (Tasks 1–11), **B** = target bring-up with checkpoints C1–C5 (Tasks 12–16), **C** = on-target integration C6–C7 and the reliability checklist (Tasks 17–19).

---

### Task 1: Host test scaffold, vendored cJSON, ports, capture IDs

**Files:**
- Create: `firmware/components/vendor/cjson/{cJSON.c,cJSON.h,LICENSE}` (downloaded, pinned)
- Create: `firmware/components/vendor/cjson/CMakeLists.txt`
- Create: `firmware/components/htp_client/include/htp_ids.h`, `firmware/components/htp_client/src/ids.c`, `firmware/components/htp_client/CMakeLists.txt`
- Create: `firmware/components/app_core/include/ports.h`, `firmware/components/app_core/include/util.h`, `firmware/components/app_core/src/util.c`, `firmware/components/app_core/CMakeLists.txt`
- Create: `firmware/tests/host/CMakeLists.txt`, `firmware/tests/host/harness.h`
- Test: `firmware/tests/host/test_ids.c`

**Interfaces (produced — later tasks depend on these exact signatures):**

```c
// ports.h — injected by main/ (device) or fakes (host)
typedef struct {
    void *ctx;
    int  (*read)(void *ctx, const char *path, void *buf, size_t cap, size_t *len);
    int  (*write)(void *ctx, const char *path, const void *data, size_t len); // atomic replace
    int  (*append)(void *ctx, const char *path, const void *data, size_t len);
    int  (*remove)(void *ctx, const char *path);
    int  (*exists)(void *ctx, const char *path);           // 1 yes, 0 no
    long long (*free_bytes)(void *ctx);
    int  (*list)(void *ctx, const char *dir,
                 int (*cb)(const char *name, void *u), void *u); // cb!=0 stops
} port_storage_t;

typedef struct {
    void *ctx;
    long long (*epoch_s)(void *ctx);          // <= 0 when RTC never set
    unsigned  (*mono_ms)(void *ctx);
    void      (*sleep_ms)(void *ctx, unsigned ms);
} port_clock_t;

typedef struct { void *ctx; void (*fill)(void *ctx, uint8_t *buf, size_t n); } port_rng_t;

typedef struct {
    void *ctx;
    int (*get)(void *ctx, const char *key, char *buf, size_t cap);  // 0 ok, -1 missing
    int (*set)(void *ctx, const char *key, const char *val);
} port_kv_t;

// util.h
size_t str_copy(char *dst, size_t cap, const char *src);  // always NUL-terminates, returns copied len

// htp_ids.h
void htp_make_capture_id(char out[64], long long epoch_s, uint32_t bootcount,
                         unsigned mono_ms, const uint8_t rand2[2]);
```

All error-returning functions in this codebase return `0` for success and `-1` for failure unless a richer code is documented.

- [ ] **Step 1: Create the directory tree and vendor cJSON**

```bash
# from the repository root
mkdir -p firmware/components/{vendor/cjson,htp_client/{include,src},app_core/{include,src},ui/{include,src,fonts},board/{include,src}} \
         firmware/tests/host/{fakes,integration} firmware/main firmware/tools
curl -fsSL -o firmware/components/vendor/cjson/cJSON.c  https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.c
curl -fsSL -o firmware/components/vendor/cjson/cJSON.h  https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/cJSON.h
curl -fsSL -o firmware/components/vendor/cjson/LICENSE  https://raw.githubusercontent.com/DaveGamble/cJSON/v1.7.18/LICENSE
head -5 firmware/components/vendor/cjson/cJSON.h   # confirm MIT header present
```

`firmware/components/vendor/cjson/CMakeLists.txt` (dual-mode: IDF component on device, ignored on host — the host build compiles the sources directly):

```cmake
if(ESP_PLATFORM)
  idf_component_register(SRCS "cJSON.c" INCLUDE_DIRS ".")
endif()
```

`firmware/components/htp_client/CMakeLists.txt`:

```cmake
if(ESP_PLATFORM)
  idf_component_register(
    SRCS "src/ids.c" "src/backoff.c" "src/client.c"
    INCLUDE_DIRS "include"
    REQUIRES vendor_cjson app_core)
endif()
```

(References `backoff.c`/`client.c` which arrive in Tasks 2–3 — the IDF build is not exercised until Task 12, so listing them now is safe and avoids re-edits.)

`firmware/components/app_core/CMakeLists.txt`:

```cmake
if(ESP_PLATFORM)
  idf_component_register(
    SRCS "src/util.c" "src/wav.c" "src/app_config.c" "src/wifi_select.c"
         "src/sidecar.c" "src/rec_index.c" "src/capture_flow.c" "src/sync.c"
         "src/gesture.c" "src/ui_flow.c"
    INCLUDE_DIRS "include"
    REQUIRES vendor_cjson htp_client ui)
endif()
```

- [ ] **Step 2: Write the harness, host CMake, and the failing ID test**

`firmware/tests/host/harness.h`:

```c
#ifndef HARNESS_H
#define HARNESS_H
#include <stdio.h>
#include <string.h>
static int h_checks = 0, h_fails = 0;
#define CHECK(cond) do { h_checks++; if (!(cond)) { h_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_INT(a, b) do { long long _x = (long long)(a), _y = (long long)(b); h_checks++; \
    if (_x != _y) { h_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, _x, _y); } } while (0)
#define CHECK_EQ_STR(a, b) do { const char *_x = (a), *_y = (b); h_checks++; \
    if (strcmp(_x, _y) != 0) { h_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, _x, _y); } } while (0)
#define HARNESS_REPORT() (fprintf(stderr, "%d checks, %d failures\n", h_checks, h_fails), \
    h_fails ? 1 : 0)
#endif
```

`firmware/tests/host/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(htp_host_tests C)
set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -g)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(FIXDIR ${FW}/../bridge/tests/fixtures/contract)

add_library(vendor_cjson ${FW}/components/vendor/cjson/cJSON.c)
target_include_directories(vendor_cjson PUBLIC ${FW}/components/vendor/cjson)

# Component source lists grow as tasks add files; keep them explicit.
add_library(fw_components
  ${FW}/components/htp_client/src/ids.c
  ${FW}/components/app_core/src/util.c
)
target_include_directories(fw_components PUBLIC
  ${FW}/components/htp_client/include
  ${FW}/components/app_core/include
  ${FW}/components/ui/include
  ${FW}/components/ui/fonts
)
target_link_libraries(fw_components PUBLIC vendor_cjson)

enable_testing()
set(FAKES "")   # grows: fakes/fake_transport.c etc.
function(host_test name)
  add_executable(${name} ${name}.c ${FAKES})
  target_link_libraries(${name} fw_components)
  target_compile_definitions(${name} PRIVATE FIXDIR="${FIXDIR}")
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

host_test(test_ids)
```

`firmware/tests/host/test_ids.c`:

```c
#include "harness.h"
#include "htp_ids.h"
#include <ctype.h>

static int valid_alphabet(const char *s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!(isalnum((unsigned char)*s) || *s == '_' || *s == '-')) return 0;
    return 1;
}

int main(void) {
    char id[64];
    const uint8_t r[2] = {0x3f, 0xa9};

    /* RTC set: 1785838502 == 2026-08-04 10:15:02 UTC */
    htp_make_capture_id(id, 1785838502LL, 7, 1234, r);
    CHECK_EQ_STR(id, "c-20260804-101502-3fa9");
    CHECK(valid_alphabet(id));

    /* RTC never set (epoch <= 0): boot counter + monotonic ms */
    htp_make_capture_id(id, 0, 17, 4523, r);
    CHECK_EQ_STR(id, "c-b17-4523-3fa9");
    CHECK(valid_alphabet(id));

    /* Random suffix is zero-padded */
    const uint8_t r2[2] = {0x00, 0x0a};
    htp_make_capture_id(id, 1785838502LL, 0, 0, r2);
    CHECK_EQ_STR(id, "c-20260804-101502-000a");

    /* str_copy truncates safely */
    char small[8];
    extern size_t str_copy(char *, size_t, const char *);
    size_t n = str_copy(small, sizeof small, "abcdefghij");
    CHECK_EQ_INT(n, 7);
    CHECK_EQ_STR(small, "abcdefg");
    return HARNESS_REPORT();
}
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cmake -S firmware/tests/host -B firmware/tests/host/build && cmake --build firmware/tests/host/build -j2
```

Expected: build FAILS — `htp_ids.h: No such file or directory` (the test exists, the unit does not).

- [ ] **Step 4: Implement `util.c` and `ids.c`**

`firmware/components/app_core/include/util.h`:

```c
#ifndef APP_UTIL_H
#define APP_UTIL_H
#include <stddef.h>
size_t str_copy(char *dst, size_t cap, const char *src);
#endif
```

`firmware/components/app_core/src/util.c`:

```c
#include "util.h"
#include <string.h>

size_t str_copy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return 0;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}
```

`firmware/components/htp_client/include/htp_ids.h`:

```c
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
```

`firmware/components/htp_client/src/ids.c` (civil-date math is Howard Hinnant's public-domain `civil_from_days` algorithm — no libc `gmtime` so behavior is identical on host and target):

```c
#include "htp_ids.h"
#include <stdio.h>

static void civil_from_days(long long z, int *y, unsigned *m, unsigned *d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long yr = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yr + (*m <= 2));
}

void htp_make_capture_id(char out[64], long long epoch_s, uint32_t bootcount,
                         unsigned mono_ms, const uint8_t rand2[2]) {
    unsigned suffix = ((unsigned)rand2[0] << 8) | rand2[1];
    if (epoch_s > 0) {
        long long days = epoch_s / 86400;
        long long rem = epoch_s % 86400;
        int y; unsigned m, d;
        civil_from_days(days, &y, &m, &d);
        snprintf(out, 64, "c-%04d%02u%02u-%02lld%02lld%02lld-%04x",
                 y, m, d, rem / 3600, (rem % 3600) / 60, rem % 60, suffix);
    } else {
        snprintf(out, 64, "c-b%lu-%u-%04x", (unsigned long)bootcount, mono_ms, suffix);
    }
}
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cmake --build firmware/tests/host/build -j2 && ctest --test-dir firmware/tests/host/build --output-on-failure
```

Expected: `test_ids` PASS (`100% tests passed`).

- [ ] **Step 6: Verify the purity rule and commit**

```bash
grep -rn "esp_\|freertos\|driver/" firmware/components/htp_client/src firmware/components/app_core/src && echo "PURITY VIOLATION" || true
git add firmware/
git commit -m "feat(firmware): host test scaffold, vendored cJSON, ports, capture IDs"
```

---

### Task 2: Backoff policy and WAV header codec

**Files:**
- Create: `firmware/components/htp_client/include/htp_backoff.h`, `firmware/components/htp_client/src/backoff.c`
- Create: `firmware/components/app_core/include/wav.h`, `firmware/components/app_core/src/wav.c`
- Modify: `firmware/tests/host/CMakeLists.txt` (add sources + tests)
- Test: `firmware/tests/host/test_backoff.c`, `firmware/tests/host/test_wav.c`

**Interfaces:**
- Consumes: `harness.h` (Task 1).
- Produces:

```c
// htp_backoff.h
typedef struct { int attempt; unsigned base_ms, cap_ms; } htp_backoff_t;
void htp_backoff_init(htp_backoff_t *b, unsigned base_ms, unsigned cap_ms);
unsigned htp_backoff_next(htp_backoff_t *b);   // base, 2*base, 4*base ... capped

// wav.h
typedef struct {
    uint32_t sample_rate; uint16_t bits, channels;
    uint32_t data_bytes;  uint32_t data_offset;
} wav_info_t;
void wav_write_header(uint8_t out[44], uint32_t sample_rate, uint16_t bits,
                      uint16_t channels, uint32_t data_bytes);
int  wav_parse_header(const uint8_t *buf, size_t len, wav_info_t *out); // 0 ok, -1 bad
```

- [ ] **Step 1: Write the failing tests**

`firmware/tests/host/test_backoff.c`:

```c
#include "harness.h"
#include "htp_backoff.h"

int main(void) {
    htp_backoff_t b;
    htp_backoff_init(&b, 1000, 30000);
    CHECK_EQ_INT(htp_backoff_next(&b), 1000);
    CHECK_EQ_INT(htp_backoff_next(&b), 2000);
    CHECK_EQ_INT(htp_backoff_next(&b), 4000);
    CHECK_EQ_INT(htp_backoff_next(&b), 8000);
    CHECK_EQ_INT(htp_backoff_next(&b), 16000);
    CHECK_EQ_INT(htp_backoff_next(&b), 30000);  /* capped */
    CHECK_EQ_INT(htp_backoff_next(&b), 30000);  /* stays capped */
    htp_backoff_init(&b, 1000, 30000);
    CHECK_EQ_INT(htp_backoff_next(&b), 1000);   /* re-init resets */
    return HARNESS_REPORT();
}
```

`firmware/tests/host/test_wav.c`:

```c
#include "harness.h"
#include "wav.h"
#include <string.h>

int main(void) {
    uint8_t h[44];
    /* Upload format: 16 kHz, 16-bit, mono, 32000 data bytes (1 s) */
    wav_write_header(h, 16000, 16, 1, 32000);
    CHECK(memcmp(h, "RIFF", 4) == 0);
    CHECK(memcmp(h + 8, "WAVEfmt ", 8) == 0);
    /* RIFF size = 36 + data */
    uint32_t riff = (uint32_t)h[4] | h[5] << 8 | h[6] << 16 | (uint32_t)h[7] << 24;
    CHECK_EQ_INT(riff, 32036);
    /* byte rate = rate * block_align = 16000 * 2 */
    uint32_t brate = (uint32_t)h[28] | h[29] << 8 | h[30] << 16 | (uint32_t)h[31] << 24;
    CHECK_EQ_INT(brate, 32000);
    CHECK_EQ_INT(h[32] | h[33] << 8, 2);   /* block align */
    CHECK_EQ_INT(h[34] | h[35] << 8, 16);  /* bits */

    /* Round-trip through the parser */
    wav_info_t inf;
    CHECK_EQ_INT(wav_parse_header(h, sizeof h, &inf), 0);
    CHECK_EQ_INT(inf.sample_rate, 16000);
    CHECK_EQ_INT(inf.channels, 1);
    CHECK_EQ_INT(inf.bits, 16);
    CHECK_EQ_INT(inf.data_bytes, 32000);
    CHECK_EQ_INT(inf.data_offset, 44);

    /* Reply format: 24 kHz mono parses too */
    wav_write_header(h, 24000, 16, 1, 4800);
    CHECK_EQ_INT(wav_parse_header(h, sizeof h, &inf), 0);
    CHECK_EQ_INT(inf.sample_rate, 24000);

    /* Extra chunk (e.g. LIST) between fmt and data is skipped */
    uint8_t x[64] = {0};
    memcpy(x, "RIFF", 4); uint32_t sz = 56; memcpy(x + 4, &sz, 4);
    memcpy(x + 8, "WAVEfmt ", 8); uint32_t fl = 16; memcpy(x + 16, &fl, 4);
    uint16_t fmt = 1, ch = 1, ba = 2, bits = 16; uint32_t rate = 16000, br = 32000;
    memcpy(x + 20, &fmt, 2); memcpy(x + 22, &ch, 2); memcpy(x + 24, &rate, 4);
    memcpy(x + 28, &br, 4); memcpy(x + 32, &ba, 2); memcpy(x + 34, &bits, 2);
    memcpy(x + 36, "LIST", 4); uint32_t ll = 4; memcpy(x + 40, &ll, 4);
    memcpy(x + 48, "data", 4); uint32_t db = 8; memcpy(x + 52, &db, 4);
    CHECK_EQ_INT(wav_parse_header(x, sizeof x, &inf), 0);
    CHECK_EQ_INT(inf.data_offset, 56);
    CHECK_EQ_INT(inf.data_bytes, 8);

    /* Garbage and short buffers are rejected */
    CHECK_EQ_INT(wav_parse_header((const uint8_t *)"nope", 4, &inf), -1);
    CHECK_EQ_INT(wav_parse_header(h, 10, &inf), -1);
    return HARNESS_REPORT();
}
```

Add to `firmware/tests/host/CMakeLists.txt`: sources `${FW}/components/htp_client/src/backoff.c` and `${FW}/components/app_core/src/wav.c` in `fw_components`; `host_test(test_backoff)` and `host_test(test_wav)` at the bottom.

- [ ] **Step 2: Run to verify failure**

```bash
cmake -S firmware/tests/host -B firmware/tests/host/build && cmake --build firmware/tests/host/build -j2
```

Expected: FAILS — missing `htp_backoff.h` / `wav.h`.

- [ ] **Step 3: Implement**

`firmware/components/htp_client/src/backoff.c`:

```c
#include "htp_backoff.h"

void htp_backoff_init(htp_backoff_t *b, unsigned base_ms, unsigned cap_ms) {
    b->attempt = 0; b->base_ms = base_ms; b->cap_ms = cap_ms;
}

unsigned htp_backoff_next(htp_backoff_t *b) {
    unsigned v = b->base_ms;
    for (int i = 0; i < b->attempt && v < b->cap_ms; i++) v *= 2;
    if (v > b->cap_ms) v = b->cap_ms;
    b->attempt++;
    return v;
}
```

`firmware/components/app_core/src/wav.c`:

```c
#include "wav.h"
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

void wav_write_header(uint8_t out[44], uint32_t sample_rate, uint16_t bits,
                      uint16_t channels, uint32_t data_bytes) {
    uint16_t block_align = (uint16_t)(channels * bits / 8);
    memcpy(out, "RIFF", 4);          wr32(out + 4, 36 + data_bytes);
    memcpy(out + 8, "WAVEfmt ", 8);  wr32(out + 16, 16);
    wr16(out + 20, 1);               wr16(out + 22, channels);
    wr32(out + 24, sample_rate);     wr32(out + 28, sample_rate * block_align);
    wr16(out + 32, block_align);     wr16(out + 34, bits);
    memcpy(out + 36, "data", 4);     wr32(out + 40, data_bytes);
}

int wav_parse_header(const uint8_t *buf, size_t len, wav_info_t *out) {
    if (len < 44 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) return -1;
    size_t pos = 12;
    int have_fmt = 0;
    while (pos + 8 <= len) {
        uint32_t csz = rd32(buf + pos + 4);
        if (!memcmp(buf + pos, "fmt ", 4)) {
            if (pos + 8 + 16 > len) return -1;
            const uint8_t *f = buf + pos + 8;
            out->channels = rd16(f + 2);
            out->sample_rate = rd32(f + 4);
            out->bits = rd16(f + 14);
            have_fmt = 1;
        } else if (!memcmp(buf + pos, "data", 4)) {
            if (!have_fmt) return -1;
            out->data_bytes = csz;
            out->data_offset = (uint32_t)(pos + 8);
            return 0;
        }
        pos += 8 + csz + (csz & 1);
    }
    return -1;
}
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cmake --build firmware/tests/host/build -j2 && ctest --test-dir firmware/tests/host/build --output-on-failure
```

Expected: 3/3 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add firmware/
git commit -m "feat(firmware): backoff policy and WAV header codec"
```

---

### Task 3: HTP client — endpoints, JSON, error mapping (contract fixtures)

**Files:**
- Create: `firmware/components/htp_client/include/htp_client.h`, `firmware/components/htp_client/src/client.c`
- Create: `firmware/tests/host/fakes/fake_transport.c`, `firmware/tests/host/fakes/fake_transport.h`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_client.c`

**Interfaces:**
- Consumes: `str_copy` (Task 1), cJSON, contract fixtures at the CMake-provided `FIXDIR` (from `bridge/tests/fixtures/contract/`).
- Produces (verbatim — every later task uses these):

```c
// htp_client.h
#define HTP_OK           0
#define HTP_ERR_NETWORK -1   /* transport failure / timeout: retry with backoff */
#define HTP_ERR_AUTH    -2   /* 401: display auth error, never retry */
#define HTP_ERR_CLIENT  -3   /* other 4xx: never retry */
#define HTP_ERR_SERVER  -4   /* 5xx: retry with backoff */
#define HTP_ERR_PROTO   -5   /* malformed response */

typedef struct { const char *name; const char *value; } htp_header_t;

typedef struct {
    const char *method;            /* "GET" | "POST" */
    const char *path;              /* "/htp/v1/...", may embed query */
    htp_header_t headers[8];       /* protocol headers; client adds Authorization + X-Battery */
    int header_count;
    const char *content_type;      /* NULL when no body */
    const uint8_t *body; size_t body_len;   /* inline body, or */
    const char *body_file;         /* stream this file when non-NULL */
    const char *sink_file;         /* write response body to this path when non-NULL */
    int timeout_ms;                /* always > 0 */
} htp_request_t;

typedef struct {
    int transport_err;   /* nonzero = network-level failure; status invalid */
    int status;
    const char *body;    /* NUL-terminated; owned by transport until next perform() */
    size_t body_len;
} htp_response_t;

typedef struct {
    int (*perform)(void *ctx, const htp_request_t *req, htp_response_t *resp);
    void *ctx;
} htp_transport_t;

typedef struct {
    htp_transport_t *transport;
    const char *token;
    int battery_pct;               /* refreshed by caller; -1 omits the header */
} htp_client_t;

typedef enum { HTP_ST_RECEIVED, HTP_ST_TRANSCRIBING, HTP_ST_PROCESSING, HTP_ST_DONE,
               HTP_ST_REPLY_READY, HTP_ST_FAILED, HTP_ST_UNKNOWN } htp_capture_state_t;

typedef struct {
    char id[64];
    htp_capture_state_t state;
    char transcript[1024];         /* "" when absent; safe-truncated */
    char conversation_id[32];      /* "" when absent */
    char error[48];                /* failed reason slug */
} htp_capture_status_t;

typedef struct { char id[32]; char text[64]; int done; char style[12]; } htp_dash_item_t;
typedef struct {
    int unchanged;
    char rev[24]; char title[48];
    htp_dash_item_t items[32]; int item_count;
    long long server_time; int sync_interval;
} htp_dashboard_t;

typedef struct { char id[24]; char text[200]; int urgent; long long created; } htp_notification_t;
typedef struct { htp_notification_t items[16]; int count; long long server_time; } htp_notifications_t;

typedef struct {
    const char *capture_id;
    const char *wav_path;
    long long recorded_at;         /* <= 0 -> omit X-Recorded-At */
    const char *conversation_id;   /* NULL or "" -> omit X-Conversation-Id */
} htp_upload_params_t;

void htp_client_init(htp_client_t *c, htp_transport_t *t, const char *token);
int  htp_upload_capture(htp_client_t *c, const htp_upload_params_t *p);
int  htp_poll_captures(htp_client_t *c, const char *const ids[], int n,
                       htp_capture_status_t out[], int max_out, long long *server_time);
                       /* returns count >= 0, or HTP_ERR_* */
int  htp_download_reply(htp_client_t *c, const char *capture_id, const char *dest_path);
int  htp_get_dashboard(htp_client_t *c, const char *rev, htp_dashboard_t *out);
int  htp_complete_item(htp_client_t *c, const char *item_id, char rev_out[24]);
int  htp_get_notifications(htp_client_t *c, htp_notifications_t *out);
int  htp_ack_notifications(htp_client_t *c, const char *const ids[], int n);
```

Behavioral contract: every call maps the response with one rule — `transport_err != 0 →
HTP_ERR_NETWORK`; `401 → HTP_ERR_AUTH`; other `4xx → HTP_ERR_CLIENT`; `5xx →
HTP_ERR_SERVER`; unparseable body → `HTP_ERR_PROTO`. Unknown JSON fields are ignored
everywhere (protocol §3 versioning rule). Unknown `state` strings parse as
`HTP_ST_UNKNOWN`; unknown `priority` values parse as not-urgent (protocol §5.7).

- [ ] **Step 1: Write the fake transport**

`firmware/tests/host/fakes/fake_transport.h`:

```c
#ifndef FAKE_TRANSPORT_H
#define FAKE_TRANSPORT_H
#include "htp_client.h"

#define FT_MAX 32
typedef struct {
    /* scripted responses, consumed in order */
    struct { int transport_err; int status; char body[4096]; } resp[FT_MAX];
    int resp_count, resp_next;
    /* captured requests (headers flattened to "Name: value" lines) */
    struct {
        char method[8]; char path[256]; char content_type[40];
        char headers[8][160]; int header_count;
        char body[1024]; size_t body_len;
        char body_file[128]; char sink_file[128];
        int timeout_ms;
    } req[FT_MAX];
    int req_count;
} fake_transport_t;

void ft_init(fake_transport_t *ft, htp_transport_t *out);
void ft_push(fake_transport_t *ft, int transport_err, int status, const char *body);
void ft_push_fixture(fake_transport_t *ft, int status, const char *fixture_name); /* FIXDIR/<name> */
const char *ft_find_header(fake_transport_t *ft, int req_idx, const char *name);  /* NULL if absent */
#endif
```

`firmware/tests/host/fakes/fake_transport.c`:

```c
#include "fake_transport.h"
#include <stdio.h>
#include <string.h>

static int ft_perform(void *ctx, const htp_request_t *q, htp_response_t *r) {
    fake_transport_t *ft = ctx;
    int i = ft->req_count++;
    snprintf(ft->req[i].method, 8, "%s", q->method);
    snprintf(ft->req[i].path, 256, "%s", q->path);
    snprintf(ft->req[i].content_type, 40, "%s", q->content_type ? q->content_type : "");
    ft->req[i].header_count = q->header_count;
    for (int h = 0; h < q->header_count; h++)
        snprintf(ft->req[i].headers[h], 160, "%s: %s", q->headers[h].name, q->headers[h].value);
    ft->req[i].body_len = q->body_len;
    if (q->body && q->body_len < sizeof ft->req[i].body)
        { memcpy(ft->req[i].body, q->body, q->body_len); ft->req[i].body[q->body_len] = 0; }
    snprintf(ft->req[i].body_file, 128, "%s", q->body_file ? q->body_file : "");
    snprintf(ft->req[i].sink_file, 128, "%s", q->sink_file ? q->sink_file : "");
    ft->req[i].timeout_ms = q->timeout_ms;

    if (ft->resp_next >= ft->resp_count) { r->transport_err = 1; return -1; }
    int j = ft->resp_next++;
    r->transport_err = ft->resp[j].transport_err;
    r->status = ft->resp[j].status;
    r->body = ft->resp[j].body;
    r->body_len = strlen(ft->resp[j].body);
    /* sink_file: the real transport writes the body to disk; the fake does too */
    if (q->sink_file && !r->transport_err && r->status == 200) {
        FILE *f = fopen(q->sink_file, "wb");
        if (f) { fwrite(r->body, 1, r->body_len, f); fclose(f); }
    }
    return r->transport_err ? -1 : 0;
}

void ft_init(fake_transport_t *ft, htp_transport_t *out) {
    memset(ft, 0, sizeof *ft);
    out->perform = ft_perform; out->ctx = ft;
}

void ft_push(fake_transport_t *ft, int terr, int status, const char *body) {
    int j = ft->resp_count++;
    ft->resp[j].transport_err = terr; ft->resp[j].status = status;
    snprintf(ft->resp[j].body, sizeof ft->resp[j].body, "%s", body ? body : "");
}

void ft_push_fixture(fake_transport_t *ft, int status, const char *name) {
    char path[512]; snprintf(path, sizeof path, "%s/%s", FIXDIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "missing fixture %s\n", path); ft_push(ft, 1, 0, ""); return; }
    int j = ft->resp_count++;
    ft->resp[j].transport_err = 0; ft->resp[j].status = status;
    size_t n = fread(ft->resp[j].body, 1, sizeof ft->resp[j].body - 1, f);
    ft->resp[j].body[n] = 0; fclose(f);
}

const char *ft_find_header(fake_transport_t *ft, int i, const char *name) {
    size_t nl = strlen(name);
    for (int h = 0; h < ft->req[i].header_count; h++)
        if (!strncmp(ft->req[i].headers[h], name, nl) && ft->req[i].headers[h][nl] == ':')
            return ft->req[i].headers[h] + nl + 2;
    return NULL;
}
```

Note: `FIXDIR` is a compile definition, but `fake_transport.c` is compiled per-test via the
`FAKES` list, so it sees each test's definition. Add to `firmware/tests/host/CMakeLists.txt`:
`set(FAKES fakes/fake_transport.c)` (replacing the empty set), `src/client.c` in
`fw_components`, and `host_test(test_client)`.

- [ ] **Step 2: Write the failing client test**

`firmware/tests/host/test_client.c` — every response body that has a golden fixture uses it:

```c
#include "harness.h"
#include "htp_client.h"
#include "fakes/fake_transport.h"
#include <string.h>
#include <stdio.h>

static fake_transport_t ft;
static htp_transport_t tr;
static htp_client_t cl;

static void fresh(void) { ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok-abc"); cl.battery_pct = 78; }

static void test_upload_request_shape(void) {
    fresh();
    ft_push_fixture(&ft, 200, "captures_post.json");
    htp_upload_params_t p = { .capture_id = "c-20260804-101502-3fa9",
        .wav_path = "/rec/c-20260804-101502-3fa9.wav", .recorded_at = 1785838502LL,
        .conversation_id = NULL };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_OK);
    CHECK_EQ_STR(ft.req[0].method, "POST");
    CHECK_EQ_STR(ft.req[0].path, "/htp/v1/captures");
    CHECK_EQ_STR(ft.req[0].content_type, "audio/wav");
    CHECK_EQ_STR(ft.req[0].body_file, "/rec/c-20260804-101502-3fa9.wav");
    CHECK(ft.req[0].timeout_ms > 0);
    CHECK_EQ_STR(ft_find_header(&ft, 0, "Authorization"), "Bearer tok-abc");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Capture-Id"), "c-20260804-101502-3fa9");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Capture-Mode"), "auto");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Recorded-At"), "1785838502");
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Battery"), "78");
    CHECK(ft_find_header(&ft, 0, "X-Conversation-Id") == NULL);
}

static void test_upload_omits_recorded_at_when_clockless(void) {
    fresh();
    ft_push(&ft, 0, 200, "{\"id\":\"c-b17-4523-3fa9\",\"state\":\"received\"}");
    htp_upload_params_t p = { .capture_id = "c-b17-4523-3fa9",
        .wav_path = "/rec/c-b17-4523-3fa9.wav", .recorded_at = 0,
        .conversation_id = "v-4b81" };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_OK);
    CHECK(ft_find_header(&ft, 0, "X-Recorded-At") == NULL);
    CHECK_EQ_STR(ft_find_header(&ft, 0, "X-Conversation-Id"), "v-4b81");
}

static void test_upload_id_mismatch_is_proto_error(void) {
    fresh();
    ft_push(&ft, 0, 200, "{\"id\":\"c-other\",\"state\":\"received\"}");
    htp_upload_params_t p = { .capture_id = "c-mine", .wav_path = "/x.wav" };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_ERR_PROTO);
}

static void test_poll_parses_fixture(void) {
    fresh();
    ft_push_fixture(&ft, 200, "captures_get.json");
    const char *ids[] = { "c-20260804-101502-3fa9", "c-missing" };
    htp_capture_status_t st[4]; long long server_time = 0;
    int n = htp_poll_captures(&cl, ids, 2, st, 4, &server_time);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(server_time, 1000000);
    CHECK(strstr(ft.req[0].path, "/htp/v1/captures?ids=c-20260804-101502-3fa9,c-missing") != NULL);
    CHECK_EQ_STR(st[0].id, "c-20260804-101502-3fa9");
    CHECK_EQ_INT(st[0].state, HTP_ST_DONE);
    CHECK_EQ_STR(st[0].transcript, "Add milk to the shopping list");
    CHECK_EQ_INT(st[1].state, HTP_ST_UNKNOWN);
}

static void test_poll_reply_ready_and_failed(void) {
    fresh();
    ft_push(&ft, 0, 200,
        "{\"server_time\":1000005,\"captures\":["
        "{\"id\":\"c-a\",\"state\":\"reply_ready\",\"transcript\":\"Hey\",\"conversation_id\":\"v-mock\"},"
        "{\"id\":\"c-b\",\"state\":\"failed\",\"error\":\"transcription_failed\"},"
        "{\"id\":\"c-c\",\"state\":\"someday_new_state\",\"novel_field\":true}]}");
    const char *ids[] = { "c-a", "c-b", "c-c" };
    htp_capture_status_t st[4]; long long t;
    CHECK_EQ_INT(htp_poll_captures(&cl, ids, 3, st, 4, &t), 3);
    CHECK_EQ_INT(st[0].state, HTP_ST_REPLY_READY);
    CHECK_EQ_STR(st[0].conversation_id, "v-mock");
    CHECK_EQ_INT(st[1].state, HTP_ST_FAILED);
    CHECK_EQ_STR(st[1].error, "transcription_failed");
    CHECK_EQ_INT(st[2].state, HTP_ST_UNKNOWN);   /* unknown state string tolerated */
}

static void test_dashboard_fixture(void) {
    fresh();
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "7c1a", &d), HTP_OK);
    CHECK(strstr(ft.req[0].path, "/htp/v1/dashboard?rev=7c1a") != NULL);
    CHECK_EQ_INT(d.unchanged, 0);
    CHECK_EQ_STR(d.rev, "44818d09");
    CHECK_EQ_STR(d.title, "Today");
    CHECK_EQ_INT(d.item_count, 2);
    CHECK_EQ_STR(d.items[0].id, "t-9f2");
    CHECK_EQ_STR(d.items[0].text, "Buy milk");
    CHECK_EQ_INT(d.items[0].done, 0);
    CHECK_EQ_INT(d.items[1].done, 1);
    CHECK_EQ_STR(d.items[1].style, "dim");
    CHECK_EQ_INT(d.sync_interval, 600);
    CHECK_EQ_INT(d.server_time, 1000000);
}

static void test_dashboard_unchanged_fixture(void) {
    fresh();
    ft_push_fixture(&ft, 200, "dashboard_unchanged.json");
    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "1f6aab9c", &d), HTP_OK);
    CHECK_EQ_INT(d.unchanged, 1);
    CHECK_EQ_STR(d.rev, "1f6aab9c");
    CHECK_EQ_INT(d.sync_interval, 600);
    /* empty rev omits the query parameter */
    fresh();
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_OK);
    CHECK_EQ_STR(ft.req[0].path, "/htp/v1/dashboard");
}

static void test_notifications_and_ack(void) {
    fresh();
    ft_push_fixture(&ft, 200, "notifications_get.json");
    ft_push_fixture(&ft, 200, "ack_post.json");
    htp_notifications_t nn;
    CHECK_EQ_INT(htp_get_notifications(&cl, &nn), HTP_OK);
    CHECK_EQ_INT(nn.count, 1);
    CHECK_EQ_STR(nn.items[0].id, "n-1");
    CHECK_EQ_INT(nn.items[0].urgent, 1);
    CHECK_EQ_INT(nn.items[0].created, 1000000);
    const char *ids[] = { "n-1" };
    CHECK_EQ_INT(htp_ack_notifications(&cl, ids, 1), HTP_OK);
    CHECK_EQ_STR(ft.req[1].method, "POST");
    CHECK_EQ_STR(ft.req[1].path, "/htp/v1/notifications/ack");
    CHECK_EQ_STR(ft.req[1].body, "{\"ids\":[\"n-1\"]}");
    CHECK_EQ_STR(ft.req[1].content_type, "application/json");
}

static void test_complete(void) {
    fresh();
    ft_push_fixture(&ft, 200, "complete_post.json");
    char rev[24];
    CHECK_EQ_INT(htp_complete_item(&cl, "t-9f2", rev), HTP_OK);
    CHECK_EQ_STR(rev, "62d2e25e");
    CHECK_EQ_STR(ft.req[0].body, "{\"item_id\":\"t-9f2\"}");
}

static void test_download_reply(void) {
    fresh();
    ft_push(&ft, 0, 200, "RIFFxxxxWAVE");    /* content passthrough, not parsed here */
    CHECK_EQ_INT(htp_download_reply(&cl, "c-a", "/tmp/htp_test_reply.wav"), HTP_OK);
    CHECK_EQ_STR(ft.req[0].sink_file, "/tmp/htp_test_reply.wav");
    CHECK(strstr(ft.req[0].path, "/htp/v1/captures/c-a/reply.wav") != NULL);
}

static void test_error_mapping(void) {
    fresh();
    ft_push_fixture(&ft, 401, "error_unauthorized.json");
    ft_push_fixture(&ft, 400, "error_invalid_capture_id.json");
    ft_push(&ft, 0, 500, "{\"error\":\"speech_provider_error\"}");
    ft_push(&ft, 1, 0, "");                       /* network failure */
    ft_push_fixture(&ft, 404, "error_reply_not_found.json");
    ft_push(&ft, 0, 200, "this is not json");
    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_AUTH);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_CLIENT);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_SERVER);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_NETWORK);
    CHECK_EQ_INT(htp_download_reply(&cl, "c-x", "/tmp/htp_test_reply.wav"), HTP_ERR_CLIENT);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_ERR_PROTO);
}

int main(void) {
    test_upload_request_shape();
    test_upload_omits_recorded_at_when_clockless();
    test_upload_id_mismatch_is_proto_error();
    test_poll_parses_fixture();
    test_poll_reply_ready_and_failed();
    test_dashboard_fixture();
    test_dashboard_unchanged_fixture();
    test_notifications_and_ack();
    test_complete();
    test_download_reply();
    test_error_mapping();
    return HARNESS_REPORT();
}
```

- [ ] **Step 3: Run to verify failure** — `cmake -S firmware/tests/host -B firmware/tests/host/build && cmake --build firmware/tests/host/build -j2`. Expected: FAILS, `htp_client.h` not found.

- [ ] **Step 4: Implement `client.c`**

Core shape (implement all endpoints with these helpers; the pattern below is complete for
the two hardest calls — the rest follow it mechanically):

```c
#include "htp_client.h"
#include "util.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

#define HTP_TIMEOUT_MS       15000
#define HTP_UPLOAD_TIMEOUT_MS 60000   /* 3.8 MB over weak Wi-Fi */

void htp_client_init(htp_client_t *c, htp_transport_t *t, const char *token) {
    c->transport = t; c->token = token; c->battery_pct = -1;
}

static int status_to_err(const htp_response_t *r) {
    if (r->transport_err) return HTP_ERR_NETWORK;
    if (r->status == 401) return HTP_ERR_AUTH;
    if (r->status >= 500) return HTP_ERR_SERVER;
    if (r->status >= 400) return HTP_ERR_CLIENT;
    return HTP_OK;
}

/* Adds Authorization and X-Battery, performs, maps errors.
 * On HTP_OK, *out_json holds the parsed body (caller must cJSON_Delete),
 * unless req->sink_file was set, in which case out_json is untouched. */
static int perform(htp_client_t *c, htp_request_t *req, cJSON **out_json) {
    static char battery[8];
    char auth[192];
    snprintf(auth, sizeof auth, "Bearer %s", c->token);
    req->headers[req->header_count++] = (htp_header_t){ "Authorization", auth };
    if (c->battery_pct >= 0) {
        snprintf(battery, sizeof battery, "%d", c->battery_pct);
        req->headers[req->header_count++] = (htp_header_t){ "X-Battery", battery };
    }
    if (req->timeout_ms <= 0) req->timeout_ms = HTP_TIMEOUT_MS;

    htp_response_t resp = {0};
    c->transport->perform(c->transport->ctx, req, &resp);
    int err = status_to_err(&resp);
    if (err != HTP_OK) return err;
    if (req->sink_file) return HTP_OK;
    cJSON *j = cJSON_ParseWithLength(resp.body, resp.body_len);
    if (!j) return HTP_ERR_PROTO;
    *out_json = j;
    return HTP_OK;
}

static void get_str(cJSON *obj, const char *key, char *dst, size_t cap) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    str_copy(dst, cap, cJSON_IsString(v) ? v->valuestring : "");
}

static htp_capture_state_t parse_state(const char *s) {
    if (!strcmp(s, "received"))     return HTP_ST_RECEIVED;
    if (!strcmp(s, "transcribing")) return HTP_ST_TRANSCRIBING;
    if (!strcmp(s, "processing"))   return HTP_ST_PROCESSING;
    if (!strcmp(s, "done"))         return HTP_ST_DONE;
    if (!strcmp(s, "reply_ready"))  return HTP_ST_REPLY_READY;
    if (!strcmp(s, "failed"))       return HTP_ST_FAILED;
    return HTP_ST_UNKNOWN;
}

int htp_upload_capture(htp_client_t *c, const htp_upload_params_t *p) {
    char recorded[24];
    htp_request_t req = { .method = "POST", .path = "/htp/v1/captures",
        .content_type = "audio/wav", .body_file = p->wav_path,
        .timeout_ms = HTP_UPLOAD_TIMEOUT_MS };
    req.headers[req.header_count++] = (htp_header_t){ "X-Capture-Id", p->capture_id };
    req.headers[req.header_count++] = (htp_header_t){ "X-Capture-Mode", "auto" };
    if (p->recorded_at > 0) {
        snprintf(recorded, sizeof recorded, "%lld", p->recorded_at);
        req.headers[req.header_count++] = (htp_header_t){ "X-Recorded-At", recorded };
    }
    if (p->conversation_id && p->conversation_id[0])
        req.headers[req.header_count++] = (htp_header_t){ "X-Conversation-Id", p->conversation_id };

    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    char id[64]; get_str(j, "id", id, sizeof id);
    int ok = !strcmp(id, p->capture_id);
    cJSON_Delete(j);
    return ok ? HTP_OK : HTP_ERR_PROTO;
}

int htp_poll_captures(htp_client_t *c, const char *const ids[], int n,
                      htp_capture_status_t out[], int max_out, long long *server_time) {
    char path[2560];   /* 32 ids x 64 chars + separators must fit */
    int pos = snprintf(path, sizeof path, "/htp/v1/captures?ids=");
    for (int i = 0; i < n; i++)
        pos += snprintf(path + pos, sizeof path - pos, "%s%s", i ? "," : "", ids[i]);
    htp_request_t req = { .method = "GET", .path = path };
    cJSON *j = NULL;
    int err = perform(c, &req, &j);
    if (err != HTP_OK) return err;
    cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "server_time");
    if (server_time) *server_time = cJSON_IsNumber(st) ? (long long)st->valuedouble : 0;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "captures");
    int count = 0;
    cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (count >= max_out) break;
        htp_capture_status_t *s = &out[count++];
        char state[24];
        get_str(it, "id", s->id, sizeof s->id);
        get_str(it, "state", state, sizeof state);
        s->state = parse_state(state);
        get_str(it, "transcript", s->transcript, sizeof s->transcript);
        get_str(it, "conversation_id", s->conversation_id, sizeof s->conversation_id);
        get_str(it, "error", s->error, sizeof s->error);
    }
    cJSON_Delete(j);
    return count;
}
```

Remaining endpoints, same pattern:

- `htp_download_reply`: GET `"/htp/v1/captures/%s/reply.wav"`, `sink_file = dest_path`, no JSON parse.
- `htp_get_dashboard`: GET `/htp/v1/dashboard` (append `?rev=%s` only when `rev[0]`); parse `unchanged` (`cJSON_IsTrue`), `rev`, `title`, `server_time`, `sync_interval`, and the `items` array into `htp_dash_item_t` (`done` via `cJSON_IsTrue`, `style` default `""`); `item_count` capped at 32.
- `htp_complete_item`: POST body built with `snprintf(body, sizeof body, "{\"item_id\":\"%s\"}", item_id)`, `content_type = "application/json"`; parse `rev` into `rev_out`; return `HTP_ERR_PROTO` when `ok` is not true.
- `htp_get_notifications`: GET `/htp/v1/notifications`; `urgent = (strcmp(priority,"urgent")==0)`; `created` numeric; count capped at 16.
- `htp_ack_notifications`: POST `{"ids":[...]}` built with snprintf into a 1024-byte buffer; parse `ok`.

- [ ] **Step 5: Run tests to verify they pass** — `cmake --build firmware/tests/host/build -j2 && ctest --test-dir firmware/tests/host/build --output-on-failure`. Expected: 4/4 PASS.

- [ ] **Step 6: Purity grep + commit**

```bash
grep -rn "esp_\|freertos\|driver/" firmware/components/htp_client/src && echo "PURITY VIOLATION" || true
git add firmware/
git commit -m "feat(firmware): HTP client with contract-fixture coverage"
```

---

### Task 4: Device configuration — config.json, wifi.json, profile selection

**Files:**
- Create: `firmware/components/app_core/include/app_config.h`, `firmware/components/app_core/src/app_config.c`, `firmware/components/app_core/src/wifi_select.c`
- Create: `firmware/tests/host/fakes/fake_kv.c`, `firmware/tests/host/fakes/fake_kv.h`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_config.c`, `firmware/tests/host/test_wifi_select.c`

**Interfaces:**
- Consumes: `port_kv_t` (Task 1), cJSON, `str_copy`.
- Produces:

```c
// app_config.h
typedef struct {
    char bridge_url[128];        /* e.g. "https://htp.example.net" or "http://192.0.2.10:8787" */
    char token[128];
    int  sync_interval_s;        /* default 600; superseded at runtime by dashboard sync_interval */
    int  silence_timeout_s;      /* 0 = disabled (default) */
    char log_level[8];           /* "error"|"warn"|"info"|"debug", default "info" */
} app_config_t;
int app_config_parse(const char *json, size_t len, app_config_t *out); /* -1 when url/token missing */

typedef struct {
    char ssid[33]; char password[65];
    int has_static; char ip[16], gateway[16], netmask[16];
} wifi_profile_t;
typedef struct { wifi_profile_t nets[8]; int count; } wifi_profiles_t;
int wifi_profiles_parse(const char *json, size_t len, wifi_profiles_t *out);

// wifi_select.c (declared in app_config.h)
typedef struct { char ssid[33]; int rssi; } wifi_scan_hit_t;
int wifi_select_profile(const wifi_profiles_t *p, const wifi_scan_hit_t hits[], int nhits);
    /* index of highest-priority profile present in scan, or -1 */
typedef struct { uint8_t bssid[6]; uint8_t channel; int valid; } wifi_fast_join_t;
void wifi_fast_join_load(port_kv_t *kv, wifi_fast_join_t *out);   /* kv keys "bssid" hex12, "chan" */
void wifi_fast_join_store(port_kv_t *kv, const uint8_t bssid[6], uint8_t channel);
```

- [ ] **Step 1: Write fake kv and the failing tests**

`firmware/tests/host/fakes/fake_kv.h`:

```c
#ifndef FAKE_KV_H
#define FAKE_KV_H
#include "ports.h"
typedef struct { char keys[16][32]; char vals[16][64]; int count; } fake_kv_t;
void fkv_init(fake_kv_t *f, port_kv_t *out);
#endif
```

`firmware/tests/host/fakes/fake_kv.c`:

```c
#include "fake_kv.h"
#include "util.h"
#include <string.h>

static int fkv_get(void *ctx, const char *key, char *buf, size_t cap) {
    fake_kv_t *f = ctx;
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->keys[i], key)) { str_copy(buf, cap, f->vals[i]); return 0; }
    return -1;
}
static int fkv_set(void *ctx, const char *key, const char *val) {
    fake_kv_t *f = ctx;
    for (int i = 0; i < f->count; i++)
        if (!strcmp(f->keys[i], key)) { str_copy(f->vals[i], 64, val); return 0; }
    if (f->count >= 16) return -1;
    str_copy(f->keys[f->count], 32, key);
    str_copy(f->vals[f->count], 64, val);
    f->count++;
    return 0;
}
void fkv_init(fake_kv_t *f, port_kv_t *out) {
    memset(f, 0, sizeof *f);
    out->ctx = f; out->get = fkv_get; out->set = fkv_set;
}
```

`firmware/tests/host/test_config.c`:

```c
#include "harness.h"
#include "app_config.h"
#include <string.h>

int main(void) {
    app_config_t c;
    const char *full =
        "{\"bridge_url\":\"http://192.0.2.10:8787\",\"token\":\"tok-abc\","
        "\"sync_interval_s\":300,\"silence_timeout_s\":8,\"log_level\":\"debug\","
        "\"future_field\":42}";
    CHECK_EQ_INT(app_config_parse(full, strlen(full), &c), 0);
    CHECK_EQ_STR(c.bridge_url, "http://192.0.2.10:8787");
    CHECK_EQ_STR(c.token, "tok-abc");
    CHECK_EQ_INT(c.sync_interval_s, 300);
    CHECK_EQ_INT(c.silence_timeout_s, 8);
    CHECK_EQ_STR(c.log_level, "debug");

    const char *minimal = "{\"bridge_url\":\"https://htp.example.net\",\"token\":\"t\"}";
    CHECK_EQ_INT(app_config_parse(minimal, strlen(minimal), &c), 0);
    CHECK_EQ_INT(c.sync_interval_s, 600);        /* defaults */
    CHECK_EQ_INT(c.silence_timeout_s, 0);
    CHECK_EQ_STR(c.log_level, "info");

    const char *no_token = "{\"bridge_url\":\"https://x\"}";
    CHECK_EQ_INT(app_config_parse(no_token, strlen(no_token), &c), -1);
    CHECK_EQ_INT(app_config_parse("not json", 8, &c), -1);

    wifi_profiles_t w;
    const char *nets =
        "{\"networks\":[{\"ssid\":\"home\",\"password\":\"pw1\"},"
        "{\"ssid\":\"office\",\"password\":\"pw2\","
        "\"static\":{\"ip\":\"192.0.2.20\",\"gateway\":\"192.0.2.1\",\"netmask\":\"255.255.255.0\"}}]}";
    CHECK_EQ_INT(wifi_profiles_parse(nets, strlen(nets), &w), 0);
    CHECK_EQ_INT(w.count, 2);
    CHECK_EQ_STR(w.nets[0].ssid, "home");
    CHECK_EQ_INT(w.nets[0].has_static, 0);
    CHECK_EQ_INT(w.nets[1].has_static, 1);
    CHECK_EQ_STR(w.nets[1].ip, "192.0.2.20");
    return HARNESS_REPORT();
}
```

`firmware/tests/host/test_wifi_select.c`:

```c
#include "harness.h"
#include "app_config.h"
#include "fakes/fake_kv.h"
#include <string.h>

int main(void) {
    wifi_profiles_t p = { .count = 3 };
    strcpy(p.nets[0].ssid, "first"); strcpy(p.nets[1].ssid, "second");
    strcpy(p.nets[2].ssid, "third");

    /* Priority = profile order, not signal strength */
    wifi_scan_hit_t hits[] = { { "third", -40 }, { "second", -80 } };
    CHECK_EQ_INT(wifi_select_profile(&p, hits, 2), 1);
    wifi_scan_hit_t only3[] = { { "third", -40 } };
    CHECK_EQ_INT(wifi_select_profile(&p, only3, 1), 2);
    wifi_scan_hit_t none[] = { { "stranger", -30 } };
    CHECK_EQ_INT(wifi_select_profile(&p, none, 1), -1);
    CHECK_EQ_INT(wifi_select_profile(&p, NULL, 0), -1);

    /* BSSID fast-join cache round-trip through kv */
    fake_kv_t fk; port_kv_t kv; fkv_init(&fk, &kv);
    wifi_fast_join_t fj;
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 0);
    const uint8_t bssid[6] = { 0xa0, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5 };
    wifi_fast_join_store(&kv, bssid, 6);
    wifi_fast_join_load(&kv, &fj);
    CHECK_EQ_INT(fj.valid, 1);
    CHECK_EQ_INT(fj.channel, 6);
    CHECK_EQ_INT(fj.bssid[0], 0xa0);
    CHECK_EQ_INT(fj.bssid[5], 0xf5);
    return HARNESS_REPORT();
}
```

CMake: add `src/app_config.c`, `src/wifi_select.c` to `fw_components`; `set(FAKES fakes/fake_transport.c fakes/fake_kv.c)`; `host_test(test_config)`, `host_test(test_wifi_select)`.

- [ ] **Step 2: Run to verify failure** — build fails on missing `app_config.h`.

- [ ] **Step 3: Implement** — `app_config.c` parses with cJSON exactly as Task 3's `get_str` pattern (copy that static helper locally): missing `bridge_url` or `token` (or empty string) returns -1; defaults `sync_interval_s=600`, `silence_timeout_s=0`, `log_level="info"`. `wifi_profiles_parse` walks `networks` (cap 8), `has_static=1` only when a `static` object with all three fields is present. `wifi_select_profile` is two nested loops over profiles-then-hits returning the first profile index found. `wifi_fast_join_store` writes kv `"bssid"` as 12 lowercase hex chars and `"chan"` as decimal; `wifi_fast_join_load` parses them back (`valid=0` when either key missing or malformed — use `sscanf(buf, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx", ...)`).

- [ ] **Step 4: Run tests to verify they pass** — `ctest` 6/6 PASS.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): device config, wifi profiles, fast-join cache"`

---

### Task 5: Sidecars and the recordings index

**Files:**
- Create: `firmware/components/app_core/include/sidecar.h`, `firmware/components/app_core/src/sidecar.c`
- Create: `firmware/components/app_core/include/rec_index.h`, `firmware/components/app_core/src/rec_index.c`
- Create: `firmware/tests/host/fakes/fake_storage.c`, `firmware/tests/host/fakes/fake_storage.h`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_sidecar.c`, `firmware/tests/host/test_rec_index.c`

**Interfaces:**
- Consumes: `port_storage_t` (Task 1), cJSON.
- Produces:

```c
// sidecar.h — device-side per-capture source of truth, stored at /rec/<id>.json
typedef struct {
    char id[64];
    char state[16];          /* "not_uploaded" | "uploaded" | "done" | "reply_ready" | "failed" */
    char transcript[1024];
    char conversation_id[32];
    char error[48];
    long long recorded_at;   /* 0 = unknown */
    long long uploaded_at;   /* 0 = never confirmed */
} sidecar_t;
void sidecar_init(sidecar_t *sc, const char *capture_id);        /* state=not_uploaded */
int  sidecar_load(port_storage_t *st, const char *capture_id, sidecar_t *out);
int  sidecar_save(port_storage_t *st, const sidecar_t *sc);      /* atomic via st->write */
void sidecar_path(char out[96], const char *capture_id);         /* "/rec/<id>.json" */
void sidecar_wav_path(char out[96], const char *capture_id);     /* "/rec/<id>.wav"  */

// rec_index.h — append-only /rec/index, one capture id per line
int rec_index_append(port_storage_t *st, const char *capture_id);
int rec_index_list(port_storage_t *st, char ids[][64], int max); /* newest first; returns count */
```

- [ ] **Step 1: Write the in-memory fake storage and failing tests**

`firmware/tests/host/fakes/fake_storage.h`:

```c
#ifndef FAKE_STORAGE_H
#define FAKE_STORAGE_H
#include "ports.h"
#define FS_MAX_FILES 64
#define FS_MAX_BYTES 65536
typedef struct {
    struct { char path[96]; char data[FS_MAX_BYTES]; size_t len; int used; } files[FS_MAX_FILES];
    long long free_bytes_value;   /* settable by tests; default 1<<30 */
    int fail_writes;              /* when 1, write/append return -1 (SD-full simulation) */
} fake_storage_t;
void fstore_init(fake_storage_t *f, port_storage_t *out);
const char *fstore_get(fake_storage_t *f, const char *path);   /* NULL if absent */
void fstore_put(fake_storage_t *f, const char *path, const char *data);
#endif
```

`firmware/tests/host/fakes/fake_storage.c`:

```c
#include "fake_storage.h"
#include "util.h"
#include <string.h>

static int slot(fake_storage_t *f, const char *path, int create) {
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (f->files[i].used && !strcmp(f->files[i].path, path)) return i;
    if (!create) return -1;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (!f->files[i].used) {
            f->files[i].used = 1; f->files[i].len = 0;
            str_copy(f->files[i].path, 96, path);
            return i;
        }
    return -1;
}
static int fs_read(void *ctx, const char *p, void *buf, size_t cap, size_t *len) {
    fake_storage_t *f = ctx; int i = slot(f, p, 0);
    if (i < 0) return -1;
    size_t n = f->files[i].len < cap ? f->files[i].len : cap;
    memcpy(buf, f->files[i].data, n); if (len) *len = n;
    return 0;
}
static int fs_write(void *ctx, const char *p, const void *d, size_t n) {
    fake_storage_t *f = ctx;
    if (f->fail_writes || n > FS_MAX_BYTES) return -1;
    int i = slot(f, p, 1); if (i < 0) return -1;
    memcpy(f->files[i].data, d, n); f->files[i].len = n;
    return 0;
}
static int fs_append(void *ctx, const char *p, const void *d, size_t n) {
    fake_storage_t *f = ctx;
    if (f->fail_writes) return -1;
    int i = slot(f, p, 1); if (i < 0 || f->files[i].len + n > FS_MAX_BYTES) return -1;
    memcpy(f->files[i].data + f->files[i].len, d, n); f->files[i].len += n;
    return 0;
}
static int fs_remove(void *ctx, const char *p) {
    fake_storage_t *f = ctx; int i = slot(f, p, 0);
    if (i < 0) return -1;
    f->files[i].used = 0; return 0;
}
static int fs_exists(void *ctx, const char *p) { return slot(ctx, p, 0) >= 0; }
static long long fs_free(void *ctx) { return ((fake_storage_t *)ctx)->free_bytes_value; }
static int fs_list(void *ctx, const char *dir, int (*cb)(const char *, void *), void *u) {
    fake_storage_t *f = ctx; size_t dl = strlen(dir);
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (f->files[i].used && !strncmp(f->files[i].path, dir, dl))
            if (cb(f->files[i].path + dl + 1, u)) return 0;   /* +1 skips '/' */
    return 0;
}
void fstore_init(fake_storage_t *f, port_storage_t *out) {
    memset(f, 0, sizeof *f);
    f->free_bytes_value = 1LL << 30;
    out->ctx = f; out->read = fs_read; out->write = fs_write; out->append = fs_append;
    out->remove = fs_remove; out->exists = fs_exists; out->free_bytes = fs_free; out->list = fs_list;
}
const char *fstore_get(fake_storage_t *f, const char *path) {
    int i = slot(f, path, 0);
    if (i < 0) return NULL;
    f->files[i].data[f->files[i].len] = 0;
    return f->files[i].data;
}
void fstore_put(fake_storage_t *f, const char *path, const char *data) {
    fs_write(f, path, data, strlen(data));
}
```

`firmware/tests/host/test_sidecar.c`:

```c
#include "harness.h"
#include "sidecar.h"
#include "fakes/fake_storage.h"
#include <string.h>

int main(void) {
    fake_storage_t fs; port_storage_t st; fstore_init(&fs, &st);

    sidecar_t sc;
    sidecar_init(&sc, "c-20260804-101502-3fa9");
    CHECK_EQ_STR(sc.state, "not_uploaded");
    CHECK_EQ_STR(sc.id, "c-20260804-101502-3fa9");
    sc.recorded_at = 1785838502LL;
    CHECK_EQ_INT(sidecar_save(&st, &sc), 0);

    sidecar_t back;
    CHECK_EQ_INT(sidecar_load(&st, "c-20260804-101502-3fa9", &back), 0);
    CHECK_EQ_STR(back.state, "not_uploaded");
    CHECK_EQ_INT(back.recorded_at, 1785838502LL);
    CHECK_EQ_STR(back.transcript, "");

    /* Backfill preserves other fields */
    str_copy(back.transcript, sizeof back.transcript, "Add milk to the shopping list");
    str_copy(back.state, sizeof back.state, "done");
    CHECK_EQ_INT(sidecar_save(&st, &back), 0);
    sidecar_t again;
    CHECK_EQ_INT(sidecar_load(&st, "c-20260804-101502-3fa9", &again), 0);
    CHECK_EQ_STR(again.transcript, "Add milk to the shopping list");
    CHECK_EQ_INT(again.recorded_at, 1785838502LL);

    CHECK_EQ_INT(sidecar_load(&st, "c-nope", &again), -1);

    char p[96];
    sidecar_path(p, "c-x");     CHECK_EQ_STR(p, "/rec/c-x.json");
    sidecar_wav_path(p, "c-x"); CHECK_EQ_STR(p, "/rec/c-x.wav");

    /* SD-full: save fails loudly, never silently */
    fs.fail_writes = 1;
    CHECK_EQ_INT(sidecar_save(&st, &sc), -1);
    return HARNESS_REPORT();
}
```

`firmware/tests/host/test_rec_index.c`:

```c
#include "harness.h"
#include "rec_index.h"
#include "fakes/fake_storage.h"

int main(void) {
    fake_storage_t fs; port_storage_t st; fstore_init(&fs, &st);
    char ids[8][64];

    CHECK_EQ_INT(rec_index_list(&st, ids, 8), 0);   /* empty index is fine */

    CHECK_EQ_INT(rec_index_append(&st, "c-oldest"), 0);
    CHECK_EQ_INT(rec_index_append(&st, "c-middle"), 0);
    CHECK_EQ_INT(rec_index_append(&st, "c-newest"), 0);

    int n = rec_index_list(&st, ids, 8);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_STR(ids[0], "c-newest");    /* newest first */
    CHECK_EQ_STR(ids[1], "c-middle");
    CHECK_EQ_STR(ids[2], "c-oldest");

    /* max caps the result at the newest entries */
    n = rec_index_list(&st, ids, 2);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_STR(ids[0], "c-newest");
    CHECK_EQ_STR(ids[1], "c-middle");
    return HARNESS_REPORT();
}
```

CMake: add `src/sidecar.c`, `src/rec_index.c` to `fw_components`; add `fakes/fake_storage.c` to `FAKES`; `host_test(test_sidecar)`, `host_test(test_rec_index)`.

- [ ] **Step 2: Run to verify failure** — missing `sidecar.h`.

- [ ] **Step 3: Implement** — `sidecar.c`: `sidecar_save` renders a flat cJSON object (`id`, `state`, `transcript`, `conversation_id`, `error`, `recorded_at`, `uploaded_at`) with `cJSON_PrintUnformatted` and writes via `st->write` (the port is atomic-replace by contract); `sidecar_load` reads up to 4096 bytes, parses, `get_str`-copies each field, numbers default 0. `rec_index.c`: `rec_index_append` appends `"<id>\n"`; `rec_index_list` reads the whole file (cap 16384 bytes), splits lines into a forward array, then copies the **last** `max` entries in reverse order into `ids`.

- [ ] **Step 4: Run tests to verify they pass** — `ctest` 8/8 PASS.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): capture sidecars and recordings index"`

---

### Task 6: Capture flow state machine

**Files:**
- Create: `firmware/components/app_core/include/capture_flow.h`, `firmware/components/app_core/src/capture_flow.c`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_capture_flow.c`

**Interfaces:**
- Consumes: `htp_client_t` and all `htp_*` calls (Task 3), `sidecar_*`/`rec_index_append` (Task 5), `htp_backoff_t` (Task 2), `port_storage_t`/`port_clock_t` (Task 1), fakes (Tasks 3/5).
- Produces:

```c
// capture_flow.h
typedef enum { CAPTURE_DONE, CAPTURE_REPLY_READY, CAPTURE_FAILED, CAPTURE_OFFLINE,
               CAPTURE_TIMEOUT, CAPTURE_AUTH_ERROR } capture_outcome_t;

typedef struct {
    htp_client_t  *client;
    port_storage_t *storage;
    port_clock_t   *clock;
    void (*on_status)(void *ui_ctx, const char *line);  /* short status line for the display */
    void *ui_ctx;
    unsigned poll_interval_ms;   /* 1000 in production */
    unsigned poll_window_ms;     /* 60000 in production */
    const char *reply_path;      /* "/reply.tmp.wav" */
} capture_ctx_t;

/* Run after the WAV is finalized on SD and the sidecar exists (state not_uploaded).
 * Uploads (up to 3 attempts with backoff inside the session), then polls at
 * poll_interval_ms until a terminal state or poll_window_ms elapses.
 * Updates the sidecar at every transition. On CAPTURE_REPLY_READY the reply has
 * already been downloaded to reply_path. */
capture_outcome_t capture_run(capture_ctx_t *cx, sidecar_t *sc);

/* Sync step 1: re-upload every sidecar in state not_uploaded (or reported unknown).
 * Returns number of captures successfully confirmed uploaded. Does NOT poll. */
int capture_retry_pending(capture_ctx_t *cx);
```

State rules (each is a test below): upload success → sidecar `uploaded` + `uploaded_at`;
poll `done` → sidecar `done` + transcript backfilled, outcome `CAPTURE_DONE`; poll
`reply_ready` → download reply, sidecar `reply_ready` + `conversation_id`, outcome
`CAPTURE_REPLY_READY`; poll `failed` → sidecar `failed` + error slug; `401` anywhere →
`CAPTURE_AUTH_ERROR` (no retry); network/5xx on upload → retry ×3 with backoff, then
`CAPTURE_OFFLINE` (sidecar stays `not_uploaded`); poll window exhausted →
`CAPTURE_TIMEOUT` (sidecar keeps last state); poll network errors don't abort the window
— they just consume it.

- [ ] **Step 1: Write the failing tests**

`firmware/tests/host/test_capture_flow.c` (fake clock advances `mono_ms` by 250 per call
and records `sleep_ms` durations, so 1 Hz polling and the 60 s window are testable
without real time):

```c
#include "harness.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "fakes/fake_transport.h"
#include "fakes/fake_storage.h"
#include <string.h>

static struct { unsigned now; unsigned slept[64]; int nslept; } CLK;
static long long clk_epoch(void *c) { (void)c; return 1785838502LL; }
static unsigned clk_mono(void *c) { (void)c; return CLK.now += 250; }
static void clk_sleep(void *c, unsigned ms) { (void)c; CLK.now += ms; CLK.slept[CLK.nslept++] = ms; }

#include "util.h"

static char status_log[16][64]; static int status_count;
static void on_status(void *u, const char *line) { (void)u; str_copy(status_log[status_count++], 64, line); }

static fake_transport_t ft; static htp_transport_t tr; static htp_client_t cl;
static fake_storage_t fs; static port_storage_t st;
static port_clock_t clock_port = { NULL, clk_epoch, clk_mono, clk_sleep };
static capture_ctx_t cx;
static sidecar_t sc;

static void fresh(const char *capture_id) {
    memset(&CLK, 0, sizeof CLK); status_count = 0;
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok"); cl.battery_pct = 50;
    fstore_init(&fs, &st);
    cx = (capture_ctx_t){ .client = &cl, .storage = &st, .clock = &clock_port,
        .on_status = on_status, .poll_interval_ms = 1000, .poll_window_ms = 60000,
        .reply_path = "/tmp/htp_flow_reply.wav" };
    sidecar_init(&sc, capture_id);
    sc.recorded_at = 1785838502LL;
    sidecar_save(&st, &sc);
    fstore_put(&fs, "/rec/dummy.wav", "RIFF");   /* wav presence not checked by flow */
}

static void test_note_done(void) {
    fresh("c-note");
    ft_push(&ft, 0, 200, "{\"id\":\"c-note\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-note\",\"state\":\"transcribing\"}]}");
    ft_push(&ft, 0, 200, "{\"server_time\":2,\"captures\":[{\"id\":\"c-note\",\"state\":\"done\","
                          "\"transcript\":\"Buy milk\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_DONE);
    sidecar_t back; sidecar_load(&st, "c-note", &back);
    CHECK_EQ_STR(back.state, "done");
    CHECK_EQ_STR(back.transcript, "Buy milk");
    CHECK(back.uploaded_at > 0);
    CHECK_EQ_INT(CLK.slept[0], 1000);            /* polled at 1 Hz */
}

static void test_conversation_reply(void) {
    fresh("c-conv");
    ft_push(&ft, 0, 200, "{\"id\":\"c-conv\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-conv\",\"state\":\"reply_ready\","
                          "\"transcript\":\"Hey Hermes hi\",\"conversation_id\":\"v-1\"}]}");
    ft_push(&ft, 0, 200, "RIFF-reply-bytes");     /* reply.wav download */
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_REPLY_READY);
    sidecar_t back; sidecar_load(&st, "c-conv", &back);
    CHECK_EQ_STR(back.state, "reply_ready");
    CHECK_EQ_STR(back.conversation_id, "v-1");
    CHECK(strstr(ft.req[2].path, "/htp/v1/captures/c-conv/reply.wav") != NULL);
}

static void test_offline_after_three_upload_attempts(void) {
    fresh("c-off");
    ft_push(&ft, 1, 0, ""); ft_push(&ft, 0, 500, "{\"error\":\"x\"}"); ft_push(&ft, 1, 0, "");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_OFFLINE);
    CHECK_EQ_INT(ft.req_count, 3);
    CHECK_EQ_INT(CLK.slept[0], 1000);            /* backoff between attempts: 1 s, 2 s */
    CHECK_EQ_INT(CLK.slept[1], 2000);
    sidecar_t back; sidecar_load(&st, "c-off", &back);
    CHECK_EQ_STR(back.state, "not_uploaded");
}

static void test_auth_error_stops_immediately(void) {
    fresh("c-auth");
    ft_push(&ft, 0, 401, "{\"error\":\"unauthorized\"}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_AUTH_ERROR);
    CHECK_EQ_INT(ft.req_count, 1);               /* no retries on 401 */
}

static void test_poll_window_timeout(void) {
    fresh("c-slow");
    ft_push(&ft, 0, 200, "{\"id\":\"c-slow\",\"state\":\"received\"}");
    for (int i = 0; i < 80; i++)
        ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-slow\",\"state\":\"processing\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_TIMEOUT);
    CHECK(ft.req_count < 70);                    /* window bounded the polls */
    sidecar_t back; sidecar_load(&st, "c-slow", &back);
    CHECK_EQ_STR(back.state, "uploaded");
}

static void test_failed_capture(void) {
    fresh("c-bad");
    ft_push(&ft, 0, 200, "{\"id\":\"c-bad\",\"state\":\"received\"}");
    ft_push(&ft, 0, 200, "{\"server_time\":1,\"captures\":[{\"id\":\"c-bad\",\"state\":\"failed\","
                          "\"error\":\"transcription_failed\"}]}");
    CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_FAILED);
    sidecar_t back; sidecar_load(&st, "c-bad", &back);
    CHECK_EQ_STR(back.state, "failed");
    CHECK_EQ_STR(back.error, "transcription_failed");
}

static void test_retry_pending(void) {
    fresh("c-one");                               /* c-one: not_uploaded */
    sidecar_t two; sidecar_init(&two, "c-two");   /* c-two: already uploaded */
    str_copy(two.state, sizeof two.state, "uploaded");
    sidecar_save(&st, &two);
    sidecar_t three; sidecar_init(&three, "c-three");
    sidecar_save(&st, &three);
    rec_index_append(&st, "c-one"); rec_index_append(&st, "c-two"); rec_index_append(&st, "c-three");

    ft_push(&ft, 0, 200, "{\"id\":\"c-three\",\"state\":\"received\"}");  /* newest first */
    ft_push(&ft, 0, 200, "{\"id\":\"c-one\",\"state\":\"received\"}");
    CHECK_EQ_INT(capture_retry_pending(&cx), 2);
    CHECK_EQ_INT(ft.req_count, 2);                /* c-two untouched */
    sidecar_t back; sidecar_load(&st, "c-one", &back);
    CHECK_EQ_STR(back.state, "uploaded");
}

int main(void) {
    test_note_done();
    test_conversation_reply();
    test_offline_after_three_upload_attempts();
    test_auth_error_stops_immediately();
    test_poll_window_timeout();
    test_failed_capture();
    test_retry_pending();
    return HARNESS_REPORT();
}
```

CMake: add `src/capture_flow.c` to `fw_components`; `host_test(test_capture_flow)`.

- [ ] **Step 2: Run to verify failure** — missing `capture_flow.h`.

- [ ] **Step 3: Implement `capture_flow.c`**

```c
#include "capture_flow.h"
#include "htp_backoff.h"
#include "rec_index.h"
#include "util.h"
#include <string.h>

static void status(capture_ctx_t *cx, const char *line) {
    if (cx->on_status) cx->on_status(cx->ui_ctx, line);
}

static int upload_once(capture_ctx_t *cx, sidecar_t *sc) {
    char wav[96]; sidecar_wav_path(wav, sc->id);
    htp_upload_params_t p = { .capture_id = sc->id, .wav_path = wav,
        .recorded_at = sc->recorded_at,
        .conversation_id = sc->conversation_id[0] ? sc->conversation_id : NULL };
    return htp_upload_capture(cx->client, &p);
}

/* 3 attempts, backoff between; HTP_OK / HTP_ERR_AUTH / HTP_ERR_CLIENT end early */
static int upload_with_retry(capture_ctx_t *cx, sidecar_t *sc) {
    htp_backoff_t b; htp_backoff_init(&b, 1000, 30000);
    int err = 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) cx->clock->sleep_ms(cx->clock->ctx, htp_backoff_next(&b));
        err = upload_once(cx, sc);
        if (err == HTP_OK || err == HTP_ERR_AUTH || err == HTP_ERR_CLIENT) return err;
    }
    return err;
}

capture_outcome_t capture_run(capture_ctx_t *cx, sidecar_t *sc) {
    if (strcmp(sc->state, "not_uploaded") == 0) {
        status(cx, "Uploading...");
        int err = upload_with_retry(cx, sc);
        if (err == HTP_ERR_AUTH) { status(cx, "Auth error"); return CAPTURE_AUTH_ERROR; }
        if (err != HTP_OK && err != HTP_ERR_CLIENT) {
            status(cx, "Saved, will upload later");
            return CAPTURE_OFFLINE;
        }
        if (err == HTP_ERR_CLIENT) { status(cx, "Upload rejected"); return CAPTURE_FAILED; }
        str_copy(sc->state, sizeof sc->state, "uploaded");
        sc->uploaded_at = cx->clock->epoch_s(cx->clock->ctx);
        sidecar_save(cx->storage, sc);
        status(cx, "Uploaded");
    }

    unsigned start = cx->clock->mono_ms(cx->clock->ctx);
    const char *ids[1] = { sc->id };
    while (cx->clock->mono_ms(cx->clock->ctx) - start < cx->poll_window_ms) {
        htp_capture_status_t stat; long long t;
        int n = htp_poll_captures(cx->client, ids, 1, &stat, 1, &t);
        if (n == HTP_ERR_AUTH) return CAPTURE_AUTH_ERROR;
        if (n == 1) {
            if (stat.transcript[0])
                str_copy(sc->transcript, sizeof sc->transcript, stat.transcript);
            if (stat.conversation_id[0])
                str_copy(sc->conversation_id, sizeof sc->conversation_id, stat.conversation_id);
            if (stat.state == HTP_ST_DONE) {
                str_copy(sc->state, sizeof sc->state, "done");
                sidecar_save(cx->storage, sc);
                status(cx, "Noted");
                return CAPTURE_DONE;
            }
            if (stat.state == HTP_ST_REPLY_READY) {
                if (htp_download_reply(cx->client, sc->id, cx->reply_path) == HTP_OK) {
                    str_copy(sc->state, sizeof sc->state, "reply_ready");
                    sidecar_save(cx->storage, sc);
                    return CAPTURE_REPLY_READY;
                } /* download failed: keep polling, window still bounds us */
            }
            if (stat.state == HTP_ST_FAILED) {
                str_copy(sc->state, sizeof sc->state, "failed");
                str_copy(sc->error, sizeof sc->error, stat.error);
                sidecar_save(cx->storage, sc);
                status(cx, "Failed");
                return CAPTURE_FAILED;
            }
            if (stat.state == HTP_ST_UNKNOWN) {
                /* bridge lost it: mark for re-upload, bail to retry path */
                str_copy(sc->state, sizeof sc->state, "not_uploaded");
                sidecar_save(cx->storage, sc);
                return CAPTURE_OFFLINE;
            }
        }
        cx->clock->sleep_ms(cx->clock->ctx, cx->poll_interval_ms);
    }
    sidecar_save(cx->storage, sc);
    return CAPTURE_TIMEOUT;
}

int capture_retry_pending(capture_ctx_t *cx) {
    char ids[32][64];
    int n = rec_index_list(cx->storage, ids, 32);
    int confirmed = 0;
    for (int i = 0; i < n; i++) {
        sidecar_t sc;
        if (sidecar_load(cx->storage, ids[i], &sc) != 0) continue;
        if (strcmp(sc.state, "not_uploaded") != 0) continue;
        if (upload_once(cx, &sc) == HTP_OK) {
            str_copy(sc.state, sizeof sc.state, "uploaded");
            sc.uploaded_at = cx->clock->epoch_s(cx->clock->ctx);
            sidecar_save(cx->storage, &sc);
            confirmed++;
        }
    }
    return confirmed;
}
```

- [ ] **Step 4: Run tests to verify they pass** — `ctest` 9/9 PASS.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): capture flow state machine"`

---

### Task 7: Sync cycle

**Files:**
- Create: `firmware/components/app_core/include/sync.h`, `firmware/components/app_core/src/sync.c`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_sync.c`

**Interfaces:**
- Consumes: everything from Tasks 3–6 plus `port_kv_t`.
- Produces:

```c
// sync.h
typedef struct {
    htp_client_t   *client;
    port_storage_t *storage;
    port_kv_t      *kv;         /* keys: "dash_rev", "sync_s", "acked" (comma list) */
    port_clock_t   *clock;
    capture_ctx_t  *capture;    /* for capture_retry_pending */
    /* Render callbacks return 0 once content is actually on the panel (ack gate). */
    int  (*render_dashboard)(void *ui_ctx, const htp_dashboard_t *d);
    int  (*render_notifications)(void *ui_ctx, const htp_notifications_t *n);
    void (*chime)(void *ui_ctx);
    void *ui_ctx;
    void (*set_rtc)(void *rtc_ctx, long long epoch);  /* drift correction */
    void *rtc_ctx;
} sync_ctx_t;

typedef struct {
    int uploads_retried, notifs_fetched, notifs_acked;
    int dashboard_changed;      /* 1 when a redraw happened */
    int sync_interval_s;        /* value to arm the next timer wake with */
} sync_report_t;

int sync_cycle(sync_ctx_t *cx, sync_report_t *out);   /* 0 ok; -1 total network failure */
```

Ordering contract (protocol §7.3, asserted by test): retry uploads → notifications →
dashboard → transcript backfill → ack. Chime only when an urgent notification is
present **and** it is not in the kv `"acked"` dedup ring. Ack only what
`render_notifications` returned 0 for. Dashboard `unchanged` → no render call. RTC
corrected when `|server_time - epoch_s()| > 2`.

- [ ] **Step 1: Write the failing test**

`firmware/tests/host/test_sync.c`:

```c
#include "harness.h"
#include "sync.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "fakes/fake_transport.h"
#include "fakes/fake_storage.h"
#include "fakes/fake_kv.h"
#include <string.h>

size_t str_copy(char *, size_t, const char *);

static char events[24][32]; static int nevents;
static void ev(const char *e) { str_copy(events[nevents++], 32, e); }

static long long epoch_now = 1785838502LL;
static long long clk_epoch(void *c) { (void)c; return epoch_now; }
static unsigned mono = 0;
static unsigned clk_mono(void *c) { (void)c; return mono += 100; }
static void clk_sleep(void *c, unsigned ms) { (void)c; mono += ms; }

static int rend_dash_ok(void *u, const htp_dashboard_t *d) { (void)u; (void)d; ev("dashboard"); return 0; }
static int rend_notif_ok(void *u, const htp_notifications_t *n) { (void)u; (void)n; ev("notifications"); return 0; }
static int rend_notif_fail(void *u, const htp_notifications_t *n) { (void)u; (void)n; ev("notif_fail"); return -1; }
static void chime(void *u) { (void)u; ev("chime"); }
static long long rtc_set_to = 0;
static void set_rtc(void *c, long long e) { (void)c; rtc_set_to = e; ev("set_rtc"); }

static fake_transport_t ft; static htp_transport_t tr; static htp_client_t cl;
static fake_storage_t fs; static port_storage_t st;
static fake_kv_t fk; static port_kv_t kv;
static port_clock_t ck = { NULL, clk_epoch, clk_mono, clk_sleep };
static capture_ctx_t cap;
static sync_ctx_t sx;

static void fresh(void) {
    nevents = 0; rtc_set_to = 0; mono = 0; epoch_now = 1785838502LL;
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok");
    fstore_init(&fs, &st); fkv_init(&fk, &kv);
    cap = (capture_ctx_t){ .client = &cl, .storage = &st, .clock = &ck,
        .poll_interval_ms = 1000, .poll_window_ms = 60000, .reply_path = "/tmp/r.wav" };
    sx = (sync_ctx_t){ .client = &cl, .storage = &st, .kv = &kv, .clock = &ck,
        .capture = &cap, .render_dashboard = rend_dash_ok,
        .render_notifications = rend_notif_ok, .chime = chime,
        .set_rtc = set_rtc };
}

/* One pending upload; urgent notification; changed dashboard; one sidecar needing
 * transcript backfill. Asserts the exact §7.3 order. */
static void test_full_cycle_order(void) {
    fresh();
    sidecar_t pending; sidecar_init(&pending, "c-pend"); sidecar_save(&st, &pending);
    rec_index_append(&st, "c-pend");
    sidecar_t up; sidecar_init(&up, "c-up");
    str_copy(up.state, sizeof up.state, "uploaded"); sidecar_save(&st, &up);
    rec_index_append(&st, "c-up");

    ft_push(&ft, 0, 200, "{\"id\":\"c-pend\",\"state\":\"received\"}");         /* 1 retry   */
    ft_push_fixture(&ft, 200, "notifications_get.json");                         /* 2 notifs  */
    ft_push_fixture(&ft, 200, "dashboard_get.json");                             /* 3 dash    */
    ft_push(&ft, 0, 200, "{\"server_time\":1785838509,\"captures\":["            /* 4 backfill*/
        "{\"id\":\"c-up\",\"state\":\"done\",\"transcript\":\"backfilled words\"},"
        "{\"id\":\"c-pend\",\"state\":\"transcribing\"}]}");
    ft_push_fixture(&ft, 200, "ack_post.json");                                  /* 5 ack     */

    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    CHECK_EQ_INT(rep.uploads_retried, 1);
    CHECK_EQ_INT(rep.notifs_fetched, 1);
    CHECK_EQ_INT(rep.notifs_acked, 1);
    CHECK_EQ_INT(rep.dashboard_changed, 1);
    CHECK_EQ_INT(rep.sync_interval_s, 600);

    /* request order on the wire */
    CHECK(strstr(ft.req[0].path, "/captures") != NULL);           /* upload retry */
    CHECK(strstr(ft.req[1].path, "/notifications") != NULL);
    CHECK(strstr(ft.req[2].path, "/dashboard") != NULL);
    CHECK(strstr(ft.req[3].path, "/captures?ids=") != NULL);
    CHECK(strstr(ft.req[4].path, "/notifications/ack") != NULL);
    /* chime fired for the urgent fixture notification */
    int chimed = 0;
    for (int i = 0; i < nevents; i++) if (!strcmp(events[i], "chime")) chimed = 1;
    CHECK(chimed);
    /* transcript backfilled into the sidecar */
    sidecar_t back; sidecar_load(&st, "c-up", &back);
    CHECK_EQ_STR(back.transcript, "backfilled words");
    CHECK_EQ_STR(back.state, "done");
    /* kv updated */
    char v[64];
    CHECK_EQ_INT(kv.get(kv.ctx, "dash_rev", v, sizeof v), 0);
    CHECK_EQ_STR(v, "44818d09");
    CHECK_EQ_INT(kv.get(kv.ctx, "sync_s", v, sizeof v), 0);
    CHECK_EQ_STR(v, "600");
    /* server_time 1000000 differs wildly from epoch → RTC corrected */
    CHECK(rtc_set_to != 0);
}

static void test_unchanged_dashboard_no_render(void) {
    fresh();
    epoch_now = 1000000LL;   /* fixtures carry server_time 1000000; no drift here */
    kv.set(kv.ctx, "dash_rev", "1f6aab9c");
    ft_push(&ft, 0, 200, "{\"server_time\":1000000,\"notifications\":[]}");
    ft_push_fixture(&ft, 200, "dashboard_unchanged.json");
    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    CHECK_EQ_INT(rep.dashboard_changed, 0);
    for (int i = 0; i < nevents; i++) CHECK(strcmp(events[i], "dashboard") != 0);
    /* no notifications → no ack request, no backfill needed → 2 requests total */
    CHECK_EQ_INT(ft.req_count, 2);
    /* rev passed on the request line */
    CHECK(strstr(ft.req[1].path, "rev=1f6aab9c") != NULL);
    /* server_time within 2 s of RTC → no correction */
    CHECK_EQ_INT(rtc_set_to, 0);
}

static void test_failed_render_not_acked(void) {
    fresh();
    sx.render_notifications = rend_notif_fail;
    ft_push_fixture(&ft, 200, "notifications_get.json");
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    CHECK_EQ_INT(rep.notifs_acked, 0);
    for (int i = 0; i < ft.req_count; i++)
        CHECK(strstr(ft.req[i].path, "/ack") == NULL);
}

static void test_acked_ring_dedup_no_rechime(void) {
    fresh();
    kv.set(kv.ctx, "acked", "n-1,n-9");
    ft_push_fixture(&ft, 200, "notifications_get.json");   /* redelivers n-1 */
    ft_push_fixture(&ft, 200, "dashboard_get.json");
    ft_push_fixture(&ft, 200, "ack_post.json");            /* still re-acked */
    sync_report_t rep;
    CHECK_EQ_INT(sync_cycle(&sx, &rep), 0);
    for (int i = 0; i < nevents; i++) CHECK(strcmp(events[i], "chime") != 0);
    CHECK_EQ_INT(rep.notifs_acked, 1);
}

int main(void) {
    test_full_cycle_order();
    test_unchanged_dashboard_no_render();
    test_failed_render_not_acked();
    test_acked_ring_dedup_no_rechime();
    return HARNESS_REPORT();
}
```

CMake: add `src/sync.c`; `host_test(test_sync)`.

- [ ] **Step 2: Run to verify failure** — missing `sync.h`.

- [ ] **Step 3: Implement `sync.c`** — direct transcription of the ordering contract:

1. `out->uploads_retried = capture_retry_pending(cx->capture);`
2. `htp_get_notifications`; on success: chime if any item has `urgent && !in_acked_ring`; call `render_notifications`; remember whether it returned 0.
3. `htp_get_dashboard(client, kv_get("dash_rev") or "", &d)`; when `!unchanged`: `render_dashboard`, kv-set `dash_rev`; always: kv-set `sync_s` from `d.sync_interval`, `out->sync_interval_s = d.sync_interval`; RTC drift check against `d.server_time` (`llabs(diff) > 2` → `set_rtc`).
4. Backfill: walk `rec_index_list` (cap 32), collect ids whose sidecar state is `uploaded` (i.e. non-terminal, transcript may be missing); if any, one `htp_poll_captures` call; write transcript + terminal state into each sidecar; a status of `HTP_ST_UNKNOWN` sets sidecar state back to `not_uploaded`.
5. If render succeeded and notifications were fetched: `htp_ack_notifications` with all fetched ids; on success append ids to the kv `"acked"` ring (keep the last 8 ids, comma-joined, 64-byte budget: drop oldest entries until it fits).
Return -1 only when *every* network call reported `HTP_ERR_NETWORK`; individual failures degrade gracefully (report fields 0).

- [ ] **Step 4: Run tests to verify they pass** — `ctest` 10/10 PASS.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): scheduled sync cycle with ack-after-render"`

---

### Task 8: UI foundation — 1-bpp framebuffer and font blitter

**Files:**
- Create: `firmware/components/ui/include/ui_fb.h`, `firmware/components/ui/src/fb.c`
- Create: `firmware/components/ui/fonts/font8x8_basic.h` (vendored, public domain)
- Create: `firmware/components/ui/CMakeLists.txt`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_fb.c`

**Interfaces:**
- Consumes: nothing (leaf component).
- Produces:

```c
// ui_fb.h — 200x200, 1 bit per pixel, bit set = WHITE (SSD1681 RAM convention),
// row-major, 25 bytes per row, MSB = leftmost pixel.
#define UI_W 200
#define UI_H 200
#define UI_STRIDE (UI_W / 8)
typedef struct { uint8_t px[UI_STRIDE * UI_H]; } ui_fb_t;

void fb_clear(ui_fb_t *f);                                  /* all white */
void fb_pixel(ui_fb_t *f, int x, int y, int black);         /* clips silently */
int  fb_get(const ui_fb_t *f, int x, int y);                /* 1 = black; 0 outside */
void fb_hline(ui_fb_t *f, int x, int y, int w, int black);
void fb_rect(ui_fb_t *f, int x, int y, int w, int h, int black);      /* outline */
void fb_fill(ui_fb_t *f, int x, int y, int w, int h, int black);
void fb_invert(ui_fb_t *f, int x, int y, int w, int h);
void fb_text(ui_fb_t *f, int x, int y, const char *s, int scale, int black);
int  fb_text_width(const char *s, int scale);               /* 8 * scale * chars */
int  fb_count_black(const ui_fb_t *f);
```

- [ ] **Step 1: Vendor the font**

```bash
curl -fsSL -o firmware/components/ui/fonts/font8x8_basic.h \
  https://raw.githubusercontent.com/dhepper/font8x8/master/font8x8_basic.h
head -12 firmware/components/ui/fonts/font8x8_basic.h   # confirm "Public Domain" header
```

`firmware/components/ui/CMakeLists.txt`:

```cmake
if(ESP_PLATFORM)
  idf_component_register(
    SRCS "src/fb.c" "src/widgets.c"
    INCLUDE_DIRS "include" "fonts")
endif()
```

- [ ] **Step 2: Write the failing test**

`firmware/tests/host/test_fb.c` (property tests — no hand-transcribed glyph bitmaps):

```c
#include "harness.h"
#include "ui_fb.h"
#include <string.h>

int main(void) {
    ui_fb_t fb;
    fb_clear(&fb);
    CHECK_EQ_INT(fb_count_black(&fb), 0);
    CHECK_EQ_INT((int)sizeof fb.px, 5000);

    fb_pixel(&fb, 0, 0, 1);
    fb_pixel(&fb, 199, 199, 1);
    CHECK_EQ_INT(fb_get(&fb, 0, 0), 1);
    CHECK_EQ_INT(fb_get(&fb, 199, 199), 1);
    CHECK_EQ_INT(fb_count_black(&fb), 2);
    CHECK_EQ_INT((fb.px[0] & 0x80) == 0, 1);       /* bit cleared = black, MSB first */

    fb_pixel(&fb, -1, 0, 1); fb_pixel(&fb, 0, 200, 1); fb_pixel(&fb, 200, 0, 1);
    CHECK_EQ_INT(fb_count_black(&fb), 2);           /* clipping is silent */
    CHECK_EQ_INT(fb_get(&fb, -5, 300), 0);

    fb_pixel(&fb, 0, 0, 0); fb_pixel(&fb, 199, 199, 0);
    fb_fill(&fb, 10, 10, 20, 5, 1);
    CHECK_EQ_INT(fb_count_black(&fb), 100);
    fb_invert(&fb, 10, 10, 20, 5);
    CHECK_EQ_INT(fb_count_black(&fb), 0);
    fb_invert(&fb, 0, 0, 8, 1);
    CHECK_EQ_INT(fb_count_black(&fb), 8);
    fb_clear(&fb);

    fb_rect(&fb, 50, 50, 10, 10, 1);                /* outline = perimeter */
    CHECK_EQ_INT(fb_count_black(&fb), 36);
    fb_clear(&fb);

    /* Text: renders ink, width is deterministic, 2x scale = 4x the ink */
    fb_text(&fb, 0, 0, "HTP", 1, 1);
    int ink1 = fb_count_black(&fb);
    CHECK(ink1 > 20);
    CHECK_EQ_INT(fb_text_width("HTP", 1), 24);
    CHECK_EQ_INT(fb_text_width("HTP", 2), 48);
    fb_clear(&fb);
    fb_text(&fb, 0, 0, "HTP", 2, 1);
    CHECK_EQ_INT(fb_count_black(&fb), ink1 * 4);
    fb_clear(&fb);

    /* Inverse text (black=0 on filled bg) and edge clipping don't crash */
    fb_fill(&fb, 0, 0, 200, 16, 1);
    fb_text(&fb, 2, 4, "banner", 1, 0);
    CHECK(fb_count_black(&fb) < 200 * 16 / 8 * 8);
    fb_text(&fb, 190, 190, "clipped text far off screen", 2, 1);
    return HARNESS_REPORT();
}
```

CMake: add `${FW}/components/ui/src/fb.c` to `fw_components`; `host_test(test_fb)`.

- [ ] **Step 3: Run to verify failure** — missing `ui_fb.h`.

- [ ] **Step 4: Implement `fb.c`**

```c
#include "ui_fb.h"
#include <string.h>
#include "font8x8_basic.h"

void fb_clear(ui_fb_t *f) { memset(f->px, 0xff, sizeof f->px); }

void fb_pixel(ui_fb_t *f, int x, int y, int black) {
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) return;
    uint8_t *b = &f->px[y * UI_STRIDE + x / 8];
    uint8_t mask = (uint8_t)(0x80 >> (x & 7));
    if (black) *b &= (uint8_t)~mask; else *b |= mask;
}

int fb_get(const ui_fb_t *f, int x, int y) {
    if (x < 0 || x >= UI_W || y < 0 || y >= UI_H) return 0;
    return !(f->px[y * UI_STRIDE + x / 8] & (0x80 >> (x & 7)));
}

void fb_hline(ui_fb_t *f, int x, int y, int w, int black) {
    for (int i = 0; i < w; i++) fb_pixel(f, x + i, y, black);
}

void fb_fill(ui_fb_t *f, int x, int y, int w, int h, int black) {
    for (int j = 0; j < h; j++) fb_hline(f, x, y + j, w, black);
}

void fb_rect(ui_fb_t *f, int x, int y, int w, int h, int black) {
    fb_hline(f, x, y, w, black); fb_hline(f, x, y + h - 1, w, black);
    for (int j = 1; j < h - 1; j++) { fb_pixel(f, x, y + j, black); fb_pixel(f, x + w - 1, y + j, black); }
}

void fb_invert(ui_fb_t *f, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int xx = x + i, yy = y + j;
            if (xx < 0 || xx >= UI_W || yy < 0 || yy >= UI_H) continue;
            f->px[yy * UI_STRIDE + xx / 8] ^= (uint8_t)(0x80 >> (xx & 7));
        }
}

void fb_text(ui_fb_t *f, int x, int y, const char *s, int scale, int black) {
    for (; *s; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch > 127) ch = '?';
        const unsigned char *glyph = (const unsigned char *)font8x8_basic[ch];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (glyph[gy] & (1 << gx))          /* font8x8: LSB = leftmost */
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            fb_pixel(f, x + gx * scale + sx, y + gy * scale + sy, black);
        x += 8 * scale;
    }
}

int fb_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

int fb_count_black(const ui_fb_t *f) {
    int n = 0;
    for (int y = 0; y < UI_H; y++)
        for (int x = 0; x < UI_W; x++) n += fb_get(f, x, y);
    return n;
}
```

Note the bit convention (set bit = white) matches the SSD1681 "write RAM" format, so
Task 13 sends `fb.px` to the panel without conversion.

- [ ] **Step 5: Run tests to verify they pass** — `ctest` 11/11 PASS.

- [ ] **Step 6: Commit** — `git add firmware/ && git commit -m "feat(firmware): 1-bpp framebuffer and font blitter"`

---

### Task 9: Widgets and the two-button gesture machine

**Files:**
- Create: `firmware/components/ui/include/ui_widgets.h`, `firmware/components/ui/src/widgets.c`
- Create: `firmware/components/app_core/include/gesture.h`, `firmware/components/app_core/src/gesture.c`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_widgets.c`, `firmware/tests/host/test_gesture.c`

**Interfaces:**
- Consumes: `ui_fb_t` and all `fb_*` (Task 8).
- Produces:

```c
// ui_widgets.h — layout constants are part of the contract
#define UI_STATUS_H   16      /* status line: y 0..15 */
#define UI_ROW_H      14      /* list rows, scale-1 text with 3px padding */
#define UI_BANNER_H   32      /* notification banner: bottom rows */
#define UI_LIST_ROWS  10      /* visible rows between status line and banner */

typedef struct {
    int battery_pct;          /* -1 hides */
    int wifi_ok;              /* 0/1 */
    int pending_uploads;
    char clock_hhmm[6];       /* "" hides */
} ui_status_t;
void widget_status_line(ui_fb_t *f, const ui_status_t *st);

typedef struct { char text[64]; int done; int dim; } ui_row_t;
typedef struct {
    char title[48];
    ui_row_t rows[32]; int row_count;
    int cursor;               /* absolute index; widget scrolls the window */
} ui_list_t;
void widget_list(ui_fb_t *f, const ui_list_t *l);

/* Renders one page of wrapped text (chars-per-line 24 at scale 1).
 * Returns total page count for the given text. page is 0-based. */
int widget_text_page(ui_fb_t *f, const char *text, int page);

void widget_banner(ui_fb_t *f, const char *text);   /* inverted strip at bottom */
void ui_ellipsize(char *dst, size_t cap, const char *src, int max_chars);

// gesture.h
typedef enum { GEST_NONE, GEST_REC_HOLD_START, GEST_REC_SHORT,
               GEST_PWR_SHORT, GEST_PWR_LONG, GEST_PWR_DOUBLE, GEST_PWR_OFF } gesture_t;
#define GEST_REC_HOLD_MS   350
#define GEST_PWR_LONG_MS   600
#define GEST_PWR_DOUBLE_MS 250
#define GEST_PWR_OFF_MS    5000
typedef struct {
    int rec_down, pwr_down;
    unsigned rec_t0, pwr_t0, pwr_up_t;
    int rec_hold_fired, pwr_off_fired, pwr_pending_short;
} gesture_fsm_t;
void gesture_init(gesture_fsm_t *g);
/* Feed sampled button levels (1 = pressed) + monotonic ms; returns at most one gesture. */
gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now_ms);
```

Gesture semantics (from the design §7.2): REC held ≥350 ms → `GEST_REC_HOLD_START` fires
**while held** (recording starts immediately); REC released before 350 ms →
`GEST_REC_SHORT`. PWR released before 600 ms → wait `GEST_PWR_DOUBLE_MS` for a second
press: second press-down within the window → `GEST_PWR_DOUBLE`, window lapses →
`GEST_PWR_SHORT`. PWR released after ≥600 ms → `GEST_PWR_LONG` (on release). PWR still
held at 5000 ms → `GEST_PWR_OFF` (while held).

- [ ] **Step 1: Write the failing tests**

`firmware/tests/host/test_gesture.c`:

```c
#include "harness.h"
#include "gesture.h"

/* helper: run the fsm through (rec,pwr,t) samples, return the first non-NONE gesture */
static gesture_t run(gesture_fsm_t *g, const int (*seq)[3], int n) {
    gesture_t got = GEST_NONE;
    for (int i = 0; i < n; i++) {
        gesture_t r = gesture_feed(g, seq[i][0], seq[i][1], (unsigned)seq[i][2]);
        if (r != GEST_NONE && got == GEST_NONE) got = r;
    }
    return got;
}

int main(void) {
    gesture_fsm_t g;

    /* REC hold fires at 350 ms while still held */
    gesture_init(&g);
    const int hold[][3] = { {1,0,0}, {1,0,100}, {1,0,349}, {1,0,351} };
    CHECK_EQ_INT(run(&g, hold, 4), GEST_REC_HOLD_START);
    /* ...and does not fire again, nor emit SHORT on release */
    CHECK_EQ_INT(gesture_feed(&g, 1, 0, 400), GEST_NONE);
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 500), GEST_NONE);

    /* REC quick tap = SHORT on release */
    gesture_init(&g);
    const int tap[][3] = { {1,0,0}, {1,0,100}, {0,0,200} };
    CHECK_EQ_INT(run(&g, tap, 3), GEST_REC_SHORT);

    /* PWR short: release <600, no second press within 250 → SHORT after window */
    gesture_init(&g);
    const int pshort[][3] = { {0,1,0}, {0,1,100}, {0,0,200}, {0,0,300}, {0,0,460} };
    CHECK_EQ_INT(run(&g, pshort, 5), GEST_PWR_SHORT);

    /* PWR double: second press-down within 250 of release */
    gesture_init(&g);
    const int pdouble[][3] = { {0,1,0}, {0,0,150}, {0,1,300}, {0,0,380} };
    CHECK_EQ_INT(run(&g, pdouble, 4), GEST_PWR_DOUBLE);

    /* PWR long: release after 600 */
    gesture_init(&g);
    const int plong[][3] = { {0,1,0}, {0,1,650}, {0,0,700} };
    CHECK_EQ_INT(run(&g, plong, 3), GEST_PWR_LONG);

    /* PWR off: still held at 5000, fires while held */
    gesture_init(&g);
    const int poff[][3] = { {0,1,0}, {0,1,2000}, {0,1,4999}, {0,1,5001} };
    CHECK_EQ_INT(run(&g, poff, 4), GEST_PWR_OFF);
    /* release afterwards emits nothing */
    CHECK_EQ_INT(gesture_feed(&g, 0, 0, 5200), GEST_NONE);
    return HARNESS_REPORT();
}
```

`firmware/tests/host/test_widgets.c`:

```c
#include "harness.h"
#include "ui_fb.h"
#include "ui_widgets.h"
#include <string.h>

static ui_fb_t fb;

static int region_ink(int x, int y, int w, int h) {
    int n = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) n += fb_get(&fb, x + i, y + j);
    return n;
}

int main(void) {
    /* ellipsize */
    char e[64];
    ui_ellipsize(e, sizeof e, "short", 10);
    CHECK_EQ_STR(e, "short");
    ui_ellipsize(e, sizeof e, "twelve chars!", 10);
    CHECK_EQ_INT((int)strlen(e), 10);
    CHECK_EQ_STR(e + 7, "...");

    /* status line renders in its strip only */
    fb_clear(&fb);
    ui_status_t st = { .battery_pct = 78, .wifi_ok = 1, .pending_uploads = 2,
                       .clock_hhmm = "10:15" };
    widget_status_line(&fb, &st);
    CHECK(region_ink(0, 0, UI_W, UI_STATUS_H) > 0);
    CHECK_EQ_INT(region_ink(0, UI_STATUS_H, UI_W, UI_H - UI_STATUS_H), 0);

    /* list: title, rows at fixed positions, cursor row inverted (heavy ink) */
    fb_clear(&fb);
    ui_list_t l = { .row_count = 3, .cursor = 1 };
    strcpy(l.title, "Today");
    strcpy(l.rows[0].text, "Buy milk");
    strcpy(l.rows[1].text, "Call dentist");
    strcpy(l.rows[2].text, "Water plants"); l.rows[2].done = 1;
    widget_list(&fb, &l);
    int row0 = region_ink(0, UI_STATUS_H + UI_ROW_H, UI_W, UI_ROW_H);      /* row 0 after title row */
    int row1 = region_ink(0, UI_STATUS_H + 2 * UI_ROW_H, UI_W, UI_ROW_H);  /* cursor row */
    CHECK(row0 > 0);
    CHECK(row1 > row0 * 3);          /* inversion floods the cursor row with ink */

    /* scrolling: cursor 15 of 20 keeps the cursor row visible */
    ui_list_t big = { .row_count = 20, .cursor = 15 };
    strcpy(big.title, "T");
    for (int i = 0; i < 20; i++) snprintf(big.rows[i].text, 64, "item %d", i);
    fb_clear(&fb);
    widget_list(&fb, &big);
    CHECK(fb_count_black(&fb) > 0);   /* rendered without crash; window math in unit below */

    /* text page: pagination is deterministic at 24 chars x 12 lines per page */
    fb_clear(&fb);
    char longtext[2048];
    for (int i = 0; i < 2000; i++) longtext[i] = (i % 50 == 49) ? ' ' : 'a' + (i % 26);
    longtext[2000] = 0;
    int pages = widget_text_page(&fb, longtext, 0);
    CHECK(pages >= 7);                /* 2000 chars / (24*12) ≈ 7 pages */
    CHECK(fb_count_black(&fb) > 0);
    fb_clear(&fb);
    int pages2 = widget_text_page(&fb, longtext, pages - 1);
    CHECK_EQ_INT(pages, pages2);

    /* banner: inverted strip at the bottom */
    fb_clear(&fb);
    widget_banner(&fb, "Meeting at 10:00");
    int strip = region_ink(0, UI_H - UI_BANNER_H, UI_W, UI_BANNER_H);
    CHECK(strip > UI_W * UI_BANNER_H / 2);   /* mostly black (inverted) */
    CHECK_EQ_INT(region_ink(0, 0, UI_W, UI_H - UI_BANNER_H), 0);
    return HARNESS_REPORT();
}
```

CMake: add `src/widgets.c` (ui) and `src/gesture.c` (app_core) to `fw_components`; `host_test(test_widgets)`, `host_test(test_gesture)`.

- [ ] **Step 2: Run to verify failure** — missing headers.

- [ ] **Step 3: Implement**

`gesture.c` (complete):

```c
#include "gesture.h"
#include <string.h>

void gesture_init(gesture_fsm_t *g) { memset(g, 0, sizeof *g); }

gesture_t gesture_feed(gesture_fsm_t *g, int rec, int pwr, unsigned now) {
    gesture_t out = GEST_NONE;

    /* record button */
    if (rec && !g->rec_down) { g->rec_down = 1; g->rec_t0 = now; g->rec_hold_fired = 0; }
    else if (rec && g->rec_down && !g->rec_hold_fired && now - g->rec_t0 >= GEST_REC_HOLD_MS) {
        g->rec_hold_fired = 1; out = GEST_REC_HOLD_START;
    } else if (!rec && g->rec_down) {
        g->rec_down = 0;
        if (!g->rec_hold_fired && out == GEST_NONE) out = GEST_REC_SHORT;
    }

    /* power button */
    if (pwr && !g->pwr_down) {
        g->pwr_down = 1; g->pwr_t0 = now; g->pwr_off_fired = 0;
        if (g->pwr_pending_short && now - g->pwr_up_t <= GEST_PWR_DOUBLE_MS) {
            g->pwr_pending_short = 0;
            if (out == GEST_NONE) out = GEST_PWR_DOUBLE;
        }
    } else if (pwr && g->pwr_down && !g->pwr_off_fired && now - g->pwr_t0 >= GEST_PWR_OFF_MS) {
        g->pwr_off_fired = 1;
        if (out == GEST_NONE) out = GEST_PWR_OFF;
    } else if (!pwr && g->pwr_down) {
        g->pwr_down = 0;
        if (!g->pwr_off_fired) {
            if (now - g->pwr_t0 >= GEST_PWR_LONG_MS) { if (out == GEST_NONE) out = GEST_PWR_LONG; }
            else { g->pwr_pending_short = 1; g->pwr_up_t = now; }
        }
    } else if (!pwr && g->pwr_pending_short && now - g->pwr_up_t > GEST_PWR_DOUBLE_MS) {
        g->pwr_pending_short = 0;
        if (out == GEST_NONE) out = GEST_PWR_SHORT;
    }
    return out;
}
```

`widgets.c`: `ui_ellipsize` copies up to `max_chars` (when longer, `max_chars-3` chars +
`"..."`). `widget_status_line`: scale-1 text at y=4 — left: `"%d%%"` battery, `"W"` when
`wifi_ok`, `"^%d"` when `pending_uploads > 0`; right-aligned clock via `fb_text_width`;
`fb_hline` at y=15. `widget_list`: title bold (scale 1, drawn twice at x and x+1) on the
first row below the status line; visible window = `UI_LIST_ROWS` rows starting at
`max(0, min(cursor - UI_LIST_ROWS + 1, row_count - UI_LIST_ROWS))`; each row: prefix
`"[x] "` when done, text ellipsized to 22 chars, dim rows rendered with a leading `"·"`;
cursor row `fb_invert`ed across its full row rect. `widget_text_page`: greedy
word-wrap into 24-char lines — a word longer than 24 chars is hard-broken mid-word
(the pagination test uses 49-char runs) — 12 lines per page starting at y=UI_STATUS_H+4, page
count = ceil(total_lines/12); render only lines `[page*12, page*12+12)`.
`widget_banner`: `fb_fill` black strip at `y = UI_H - UI_BANNER_H`, two wrapped lines of
inverse text (`black=0`), ellipsized at 24 chars/line.

- [ ] **Step 4: Run tests to verify they pass** — `ctest` 13/13 PASS.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): widgets and two-button gesture machine"`

---

### Task 10: Screen controller (`ui_flow`)

**Files:**
- Create: `firmware/components/app_core/include/ui_flow.h`, `firmware/components/app_core/src/ui_flow.c`
- Modify: `firmware/tests/host/CMakeLists.txt`
- Test: `firmware/tests/host/test_ui_flow.c`

**Interfaces:**
- Consumes: `htp_client_t`/`htp_dashboard_t`/`htp_complete_item` (Task 3), `sidecar_*`/`rec_index_list` (Task 5), gestures (Task 9), widgets (Task 9), `port_storage_t`/`port_kv_t`.
- Produces:

```c
// ui_flow.h
typedef enum { SCR_DASHBOARD, SCR_RECORDINGS, SCR_ENTRY, SCR_SETTINGS } ui_screen_t;
typedef enum { UIF_NONE, UIF_REDRAW_PARTIAL, UIF_REDRAW_FULL,
               UIF_START_CAPTURE, UIF_PLAY_WAV, UIF_POWER_OFF } ui_action_t;

typedef struct {
    char mac[18]; char fw_version[16]; char bridge_host[64]; int sync_interval_s;
} ui_settings_info_t;

typedef struct {
    htp_client_t *client;
    port_storage_t *storage;
    port_kv_t *kv;
    ui_screen_t screen;
    int cursor;                  /* per-screen cursor/page */
    htp_dashboard_t dash;        /* current dashboard model (loaded by caller) */
    char banner[200];            /* "" = none */
    ui_settings_info_t info;
    ui_status_t status;
    int partial_count;           /* full refresh every 8 partials */
    char entry_id[64];           /* open recording */
    int entry_page;
} ui_flow_t;

void ui_flow_init(ui_flow_t *u, htp_client_t *c, port_storage_t *st, port_kv_t *kv);
void ui_flow_render(ui_flow_t *u, ui_fb_t *fb);   /* draws current screen into fb */
/* Applies one gesture. out_path receives the WAV path for UIF_PLAY_WAV. */
ui_action_t ui_flow_gesture(ui_flow_t *u, gesture_t g, char out_path[96]);
/* Refresh-discipline helper: call with the action; returns 1 when the caller
 * should do a FULL refresh (screen change or 8 partials elapsed). */
int ui_flow_wants_full(ui_flow_t *u, ui_action_t a);
```

Behavior under test: PWR_SHORT moves the cursor (wraps) → `UIF_REDRAW_PARTIAL`.
PWR_DOUBLE cycles Dashboard → Recordings → Settings → Dashboard → `UIF_REDRAW_FULL`.
REC_SHORT on Dashboard completes the item under the cursor (`htp_complete_item`), marks
it done locally on success. REC_SHORT with a banner visible dismisses the banner
instead. PWR_LONG on Recordings opens the entry (`SCR_ENTRY`, loads sidecar); PWR_LONG
on Entry goes back. REC_SHORT on Entry returns `UIF_PLAY_WAV` + the WAV path.
REC_HOLD_START anywhere returns `UIF_START_CAPTURE`. PWR_OFF returns `UIF_POWER_OFF`.
Recordings rows show ellipsized transcript, `"(pending)"` when uploaded-but-no-transcript,
`"(not uploaded)"` when not uploaded.

- [ ] **Step 1: Write the failing test**

`firmware/tests/host/test_ui_flow.c`:

```c
#include "harness.h"
#include "ui_flow.h"
#include "ui_fb.h"
#include "sidecar.h"
#include "rec_index.h"
#include "fakes/fake_transport.h"
#include "fakes/fake_storage.h"
#include "fakes/fake_kv.h"
#include <string.h>

size_t str_copy(char *, size_t, const char *);

static fake_transport_t ft; static htp_transport_t tr; static htp_client_t cl;
static fake_storage_t fs; static port_storage_t st;
static fake_kv_t fk; static port_kv_t kv;
static ui_flow_t u; static ui_fb_t fb; static char path[96];

static void fresh(void) {
    ft_init(&ft, &tr); htp_client_init(&cl, &tr, "tok");
    fstore_init(&fs, &st); fkv_init(&fk, &kv);
    ui_flow_init(&u, &cl, &st, &kv);
    /* seed a dashboard model as the wake path would */
    u.dash.item_count = 2;
    str_copy(u.dash.title, sizeof u.dash.title, "Today");
    str_copy(u.dash.items[0].id, 32, "t-9f2");  str_copy(u.dash.items[0].text, 64, "Buy milk");
    str_copy(u.dash.items[1].id, 32, "t-c41");  str_copy(u.dash.items[1].text, 64, "Call dentist");
    /* seed two recordings */
    sidecar_t a; sidecar_init(&a, "c-old");
    str_copy(a.state, sizeof a.state, "done");
    str_copy(a.transcript, sizeof a.transcript, "the older recording words");
    sidecar_save(&st, &a); rec_index_append(&st, "c-old");
    sidecar_t b; sidecar_init(&b, "c-new");        /* not uploaded, no transcript */
    sidecar_save(&st, &b); rec_index_append(&st, "c-new");
}

int main(void) {
    fresh();
    CHECK_EQ_INT(u.screen, SCR_DASHBOARD);

    /* cursor moves and wraps; actions are partial redraws */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 1);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.cursor, 0);

    /* complete under cursor */
    ft_push_fixture(&ft, 200, "complete_post.json");
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_INT(u.dash.items[0].done, 1);
    CHECK(strstr(ft.req[0].body, "t-9f2") != NULL);

    /* banner dismiss takes precedence over complete */
    str_copy(u.banner, sizeof u.banner, "Meeting at 10:00");
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_REDRAW_PARTIAL);
    CHECK_EQ_STR(u.banner, "");
    CHECK_EQ_INT(u.dash.items[0].done, 1);    /* unchanged, no second complete call */
    CHECK_EQ_INT(ft.req_count, 1);

    /* screen cycling is a full refresh */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_DOUBLE, path), UIF_REDRAW_FULL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);
    CHECK_EQ_INT(u.cursor, 0);

    /* recordings render newest-first with placeholders */
    ui_flow_render(&u, &fb);
    CHECK(fb_count_black(&fb) > 0);

    /* open entry (cursor 0 = c-new: not uploaded) */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_FULL);
    CHECK_EQ_INT(u.screen, SCR_ENTRY);
    CHECK_EQ_STR(u.entry_id, "c-new");
    /* play from entry */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_SHORT, path), UIF_PLAY_WAV);
    CHECK_EQ_STR(path, "/rec/c-new.wav");
    /* back out */
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_LONG, path), UIF_REDRAW_FULL);
    CHECK_EQ_INT(u.screen, SCR_RECORDINGS);

    /* settings via cycle; capture and power-off pass through from anywhere */
    ui_flow_gesture(&u, GEST_PWR_DOUBLE, path);
    CHECK_EQ_INT(u.screen, SCR_SETTINGS);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_REC_HOLD_START, path), UIF_START_CAPTURE);
    CHECK_EQ_INT(ui_flow_gesture(&u, GEST_PWR_OFF, path), UIF_POWER_OFF);

    /* refresh discipline: 8 partials force a full */
    int fulls = 0;
    for (int i = 0; i < 9; i++)
        if (ui_flow_wants_full(&u, UIF_REDRAW_PARTIAL)) fulls++;
    CHECK_EQ_INT(fulls, 1);
    CHECK(ui_flow_wants_full(&u, UIF_REDRAW_FULL));
    return HARNESS_REPORT();
}
```

CMake: add `src/ui_flow.c`; `host_test(test_ui_flow)`.

- [ ] **Step 2: Run to verify failure** — missing `ui_flow.h`.

- [ ] **Step 3: Implement `ui_flow.c`** — a switch over `(screen, gesture)`:
`GEST_REC_HOLD_START` → `UIF_START_CAPTURE`; `GEST_PWR_OFF` → `UIF_POWER_OFF` (both
before any screen logic). `GEST_REC_SHORT`: banner non-empty → clear banner, partial;
`SCR_DASHBOARD` → `htp_complete_item(client, dash.items[cursor].id, rev)`; on `HTP_OK`
set `done=1` and kv-set `"dash_rev"` to the returned rev, partial; `SCR_ENTRY` →
`sidecar_wav_path(out_path, entry_id)`, return `UIF_PLAY_WAV`. `GEST_PWR_SHORT`:
cursor = `(cursor+1) % rows` for the active screen (dashboard items / recordings list /
entry pages via `entry_page`), partial. `GEST_PWR_DOUBLE`: screen = next in
Dashboard→Recordings→Settings cycle (ENTRY counts as Recordings), cursor reset, full.
`GEST_PWR_LONG`: on Recordings → load `rec_index_list` entry at cursor into `entry_id`,
`entry_page=0`, screen=ENTRY, full; on ENTRY → back to Recordings, full.
`ui_flow_render`: status line always; then per-screen — dashboard: `widget_list` from
`dash` (+ `widget_banner` when banner set); recordings: `widget_list` built from
`rec_index_list` (cap `UI_LIST_ROWS*3`) with per-row text = transcript ellipsized /
`"(pending)"` (state uploaded, empty transcript) / `"(not uploaded)"`; entry:
`widget_text_page(fb, sidecar.transcript[0] ? transcript : "(no transcript yet)",
entry_page)`; settings: four scale-1 text lines from `info` (MAC, fw_version,
bridge_host, sync interval). `ui_flow_wants_full`: increment `partial_count` on
`UIF_REDRAW_PARTIAL`, return 1 + reset when it reaches 8 or when action is
`UIF_REDRAW_FULL`.

- [ ] **Step 4: Run tests to verify they pass** — `ctest` 14/14 PASS.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): screen controller with refresh discipline"`

---

### Task 11: Host integration against `bridge --mock`

**Files:**
- Create: `firmware/tests/host/fakes/posix_transport.c`, `firmware/tests/host/fakes/posix_transport.h`
- Create: `firmware/tools/run-mock.sh`
- Create: `firmware/tests/host/integration/test_mock_bridge.c`
- Modify: `firmware/tests/host/CMakeLists.txt`

**Interfaces:**
- Consumes: the full `htp_client_t` API (Task 3), `capture_run`/`capture_retry_pending` (Task 6), `sync_cycle` (Task 7), `wav.h` (Task 2).
- Produces: `posix_transport_init(posix_transport_t *pt, htp_transport_t *out, const char *host, int port)` — a real-socket implementation of `htp_transport_t` used ONLY by host tests (the device uses Task 16's `idf_transport`).

- [ ] **Step 1: Write the POSIX transport**

`firmware/tests/host/fakes/posix_transport.h`:

```c
#ifndef POSIX_TRANSPORT_H
#define POSIX_TRANSPORT_H
#include "htp_client.h"
typedef struct { char host[64]; int port; char body[262144]; } posix_transport_t;
void posix_transport_init(posix_transport_t *pt, htp_transport_t *out,
                          const char *host, int port);
#endif
```

`firmware/tests/host/fakes/posix_transport.c` — minimal HTTP/1.1 over a blocking
socket with `SO_RCVTIMEO`/`SO_SNDTIMEO` from `req->timeout_ms`. Complete outline
(implementer fills only the standard socket boilerplate marked `/* socket(), connect() */`):

```c
#include "posix_transport.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int read_all(int fd, char *buf, size_t cap) {
    size_t got = 0; ssize_t n;
    while (got < cap - 1 && (n = read(fd, buf + got, cap - 1 - got)) > 0) got += (size_t)n;
    buf[got] = 0; return (int)got;
}

static int pt_perform(void *ctx, const htp_request_t *q, htp_response_t *r) {
    posix_transport_t *pt = ctx;
    memset(r, 0, sizeof *r);
    int fd = -1;              /* socket(AF_INET, SOCK_STREAM, 0) + SO_RCVTIMEO/SNDTIMEO  */
                              /* from q->timeout_ms + connect() to pt->host:pt->port;    */
                              /* on any failure: r->transport_err = 1; return -1;        */
    /* --- request head --- */
    char head[2048]; int hl = 0;
    /* body from file? load it (uploads are ≤ 3.8 MB; tests use tiny WAVs) */
    char *fbody = NULL; size_t fblen = 0;
    if (q->body_file) {
        FILE *f = fopen(q->body_file, "rb");
        if (!f) { close(fd); r->transport_err = 1; return -1; }
        fseek(f, 0, SEEK_END); fblen = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        fbody = malloc(fblen ? fblen : 1);
        fread(fbody, 1, fblen, f); fclose(f);
    }
    size_t blen = q->body_file ? fblen : q->body_len;
    hl += snprintf(head + hl, sizeof head - hl, "%s %s HTTP/1.1\r\nHost: %s\r\n",
                   q->method, q->path, pt->host);
    for (int i = 0; i < q->header_count; i++)
        hl += snprintf(head + hl, sizeof head - hl, "%s: %s\r\n",
                       q->headers[i].name, q->headers[i].value);
    if (q->content_type)
        hl += snprintf(head + hl, sizeof head - hl, "Content-Type: %s\r\n", q->content_type);
    hl += snprintf(head + hl, sizeof head - hl,
                   "Content-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    write(fd, head, (size_t)hl);
    if (blen) write(fd, q->body_file ? fbody : (const char *)q->body, blen);
    free(fbody);

    /* --- response --- */
    int total = read_all(fd, pt->body, sizeof pt->body);
    close(fd);
    if (total <= 0 || sscanf(pt->body, "HTTP/1.%*c %d", &r->status) != 1) {
        r->transport_err = 1; return -1;
    }
    char *sep = strstr(pt->body, "\r\n\r\n");
    if (!sep) { r->transport_err = 1; return -1; }
    r->body = sep + 4;
    r->body_len = (size_t)(total - (sep + 4 - pt->body));
    if (q->sink_file && r->status == 200) {
        FILE *f = fopen(q->sink_file, "wb");
        if (f) { fwrite(r->body, 1, r->body_len, f); fclose(f); }
    }
    return 0;
}

void posix_transport_init(posix_transport_t *pt, htp_transport_t *out,
                          const char *host, int port) {
    snprintf(pt->host, sizeof pt->host, "%s", host);
    pt->port = port;
    out->perform = pt_perform; out->ctx = pt;
}
```

(uvicorn honors `Connection: close`, so read-until-EOF is a valid body framing here;
this transport is test-only by design.)

`firmware/tools/run-mock.sh`:

```bash
#!/usr/bin/env bash
# Starts bridge --mock for host integration tests. Usage: run-mock.sh [port]
set -euo pipefail
PORT="${1:-18787}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
"$REPO/bridge/.venv/bin/htp-bridge" --mock --port "$PORT" &
MOCK_PID=$!
trap 'kill $MOCK_PID 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
  if curl -sf -H "Authorization: Bearer x" "http://127.0.0.1:$PORT/htp/v1/dashboard" >/dev/null; then
    echo "mock ready on :$PORT"
    wait $MOCK_PID
    exit 0
  fi
  sleep 0.2
done
echo "mock failed to start" >&2
exit 1
```

`chmod +x firmware/tools/run-mock.sh`. The CTest wrapper instead starts/stops the mock
around the one integration binary; add to `firmware/tests/host/CMakeLists.txt`:

```cmake
option(HTP_INTEGRATION "run integration tests against bridge --mock" ON)
if(HTP_INTEGRATION)
  add_executable(test_mock_bridge integration/test_mock_bridge.c
                 fakes/posix_transport.c ${FAKES})
  target_link_libraries(test_mock_bridge fw_components)
  target_include_directories(test_mock_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME test_mock_bridge
           COMMAND bash -c "${FW}/../bridge/.venv/bin/htp-bridge --mock --port 18787 & P=$!; \
                            trap 'kill $P' EXIT; sleep 1.5; $<TARGET_FILE:test_mock_bridge>")
endif()
```

- [ ] **Step 2: Write the integration test**

`firmware/tests/host/integration/test_mock_bridge.c` (the mock accepts any bearer token,
always reports `reply_ready`, serves a silent WAV, dashboard rev `mockrev1`):

```c
#include "harness.h"
#include "htp_client.h"
#include "capture_flow.h"
#include "sidecar.h"
#include "rec_index.h"
#include "wav.h"
#include "fakes/posix_transport.h"
#include "fakes/fake_storage.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

size_t str_copy(char *, size_t, const char *);

static long long clk_epoch(void *c) { (void)c; return 1785838502LL; }
static unsigned clk_mono(void *c) { (void)c; static unsigned t; return t += 50; }
static void clk_sleep(void *c, unsigned ms) { (void)c;
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

int main(void) {
    posix_transport_t pt; htp_transport_t tr;
    posix_transport_init(&pt, &tr, "127.0.0.1", 18787);
    htp_client_t cl; htp_client_init(&cl, &tr, "any-token-works-on-mock");
    cl.battery_pct = 55;

    /* a real (tiny) WAV on the real filesystem */
    uint8_t hdr[44]; wav_write_header(hdr, 16000, 16, 1, 320);
    uint8_t wav[364]; memcpy(wav, hdr, 44); memset(wav + 44, 0, 320);
    FILE *f = fopen("/tmp/htp_it.wav", "wb"); fwrite(wav, 1, sizeof wav, f); fclose(f);

    /* direct client calls against the live mock */
    htp_upload_params_t p = { .capture_id = "c-it-0001", .wav_path = "/tmp/htp_it.wav",
                              .recorded_at = 1785838502LL };
    CHECK_EQ_INT(htp_upload_capture(&cl, &p), HTP_OK);

    const char *ids[] = { "c-it-0001" };
    htp_capture_status_t st[1]; long long t;
    CHECK_EQ_INT(htp_poll_captures(&cl, ids, 1, st, 1, &t), 1);
    CHECK_EQ_INT(st[0].state, HTP_ST_REPLY_READY);       /* mock always replies */
    CHECK_EQ_STR(st[0].conversation_id, "v-mock");

    CHECK_EQ_INT(htp_download_reply(&cl, "c-it-0001", "/tmp/htp_it_reply.wav"), HTP_OK);
    FILE *rf = fopen("/tmp/htp_it_reply.wav", "rb");
    CHECK(rf != NULL);
    uint8_t rh[64]; size_t rn = fread(rh, 1, sizeof rh, rf); fclose(rf);
    wav_info_t inf;
    CHECK_EQ_INT(wav_parse_header(rh, rn, &inf), 0);     /* mock WAV is playable */

    htp_dashboard_t d;
    CHECK_EQ_INT(htp_get_dashboard(&cl, "", &d), HTP_OK);
    CHECK_EQ_STR(d.rev, "mockrev1");
    CHECK_EQ_INT(d.item_count, 2);
    CHECK_EQ_INT(d.sync_interval, 600);
    CHECK_EQ_INT(htp_get_dashboard(&cl, "mockrev1", &d), HTP_OK);
    CHECK_EQ_INT(d.unchanged, 1);

    htp_notifications_t nn;
    CHECK_EQ_INT(htp_get_notifications(&cl, &nn), HTP_OK);
    char rev[24];
    CHECK_EQ_INT(htp_complete_item(&cl, "t-9f2", rev), HTP_OK);

    /* the full capture flow, fake storage + real HTTP */
    fake_storage_t fs; port_storage_t stg; fstore_init(&fs, &stg);
    port_clock_t ck = { NULL, clk_epoch, clk_mono, clk_sleep };
    /* flow uploads sidecar_wav_path("/rec/<id>.wav") — the posix transport reads
     * the REAL fs, so pre-create the file there for this one test */
    FILE *g = fopen("/rec/c-it-flow.wav", "wb");
    int have_rec_dir = g != NULL;
    if (have_rec_dir) { fwrite(wav, 1, sizeof wav, g); fclose(g); }
    if (have_rec_dir) {
        capture_ctx_t cx = { .client = &cl, .storage = &stg, .clock = &ck,
            .poll_interval_ms = 100, .poll_window_ms = 10000,
            .reply_path = "/tmp/htp_it_reply2.wav" };
        sidecar_t sc; sidecar_init(&sc, "c-it-flow");
        sc.recorded_at = 1785838502LL; sidecar_save(&stg, &sc);
        CHECK_EQ_INT(capture_run(&cx, &sc), CAPTURE_REPLY_READY);
    } else {
        fprintf(stderr, "note: /rec not writable on this host, flow leg skipped\n");
    }
    return HARNESS_REPORT();
}
```

- [ ] **Step 3: Run it**

```bash
cmake -S firmware/tests/host -B firmware/tests/host/build && cmake --build firmware/tests/host/build -j2
ctest --test-dir firmware/tests/host/build -R test_mock_bridge --output-on-failure
```

Expected: PASS against the live mock. Then the full suite: `ctest --test-dir firmware/tests/host/build --output-on-failure` — 15/15 PASS.

- [ ] **Step 4: Commit** — `git add firmware/ && git commit -m "test(firmware): host integration against bridge --mock"`

**Stage A complete.** Everything protocol- and logic-shaped is now proven on the host. The remaining tasks put it on hardware.

---

### Task 12: ESP-IDF project scaffold, buttons, power rails — Checkpoint C1

**Files:**
- Create: `firmware/tools/setup-idf.sh`, `firmware/tools/pack-flash.sh`, `firmware/tools/FLASHING.md`
- Create: `firmware/CMakeLists.txt`, `firmware/sdkconfig.defaults`, `firmware/partitions.csv`
- Create: `firmware/main/CMakeLists.txt`, `firmware/main/main.c`
- Create: `firmware/components/board/include/board.h`, `firmware/components/board/src/power.c`, `firmware/components/board/src/buttons.c`, `firmware/components/board/CMakeLists.txt`
- Modify: `.gitignore` (add `firmware/sdkconfig`, `firmware/dependencies.lock`, `firmware/managed_components/`)

**Interfaces:**
- Consumes: pin map from the design §2 (Record GPIO 0, Power GPIO 18, rails: EPD 6 / audio 42 active-low, VBAT latch 17 active-high, EXT1 any-low wake on both buttons).
- Produces:

```c
// board.h (grows over Tasks 13-18; this task's slice)
void board_early_init(void);       /* rail GPIO config + VBAT latch high; call first */
void board_rail_epd(int on);       /* active-low gate handled inside */
void board_rail_audio(int on);
int  board_btn_rec(void);          /* 1 = pressed (level low) */
int  board_btn_pwr(void);
typedef enum { WAKE_COLD, WAKE_REC_BUTTON, WAKE_PWR_BUTTON, WAKE_TIMER } wake_cause_t;
wake_cause_t board_wake_cause(void);
void board_deep_sleep(unsigned seconds);   /* arms EXT1 both-buttons + timer, latches VBAT, never returns */
void board_power_off(void);                /* releases VBAT latch */
```

- [ ] **Step 1: Install the toolchain (once)**

`firmware/tools/setup-idf.sh`:

```bash
#!/usr/bin/env bash
# One-time ESP-IDF install for the build host. Pin: v5.5 (record any change here).
set -euo pipefail
IDF_DIR="${IDF_DIR:-$HOME/esp/esp-idf}"
if [ ! -d "$IDF_DIR" ]; then
  mkdir -p "$(dirname "$IDF_DIR")"
  git clone --depth 1 --branch v5.5 --recursive https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi
"$IDF_DIR/install.sh" esp32s3
echo "Done. Activate with: source $IDF_DIR/export.sh"
```

Run it: `bash firmware/tools/setup-idf.sh` (≈2 GB download; several minutes). If the
`v5.5` branch does not exist, use the latest `v5.x` release branch and update the pin
comment + `firmware/README.md` note in Task 19.

- [ ] **Step 2: Project files**

`firmware/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(htp_terminal)
```

`firmware/sdkconfig.defaults`:

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y
```

(If the build later reports flash/PSRAM mismatches at boot, adjust
`FLASHSIZE`/`SPIRAM_MODE` to what the chip reports and note it in `firmware/README.md`
— the reference board family ships in 16 MB flash + quad PSRAM and octal variants.)

`firmware/partitions.csv` (factory-only now; a future OTA layout re-partitions over USB):

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000
phy_init, data, phy,     0xf000,  0x1000
factory,  app,  factory, 0x10000, 0x300000
```

`firmware/main/CMakeLists.txt`:

```cmake
idf_component_register(
  SRCS "main.c"
  INCLUDE_DIRS "."
  REQUIRES board app_core htp_client ui vendor_cjson esp_wifi esp_http_client
           nvs_flash fatfs sdmmc esp_adc driver)
```

`firmware/components/board/CMakeLists.txt`:

```cmake
idf_component_register(
  SRCS "src/power.c" "src/buttons.c"
  INCLUDE_DIRS "include"
  REQUIRES driver esp_adc nvs_flash fatfs sdmmc)
```

(Sources grow in Tasks 13–18: `epd_ssd1681.c`, `sdcard.c`, `audio.c`, `battery.c`, `rtc_pcf8563.c`.)

- [ ] **Step 3: Implement power + buttons**

`firmware/components/board/src/power.c`:

```c
#include "board.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

#define PIN_EPD_PWR   6    /* active low  */
#define PIN_AUDIO_PWR 42   /* active low  */
#define PIN_VBAT_HOLD 17   /* active high */
#define PIN_BTN_REC   0
#define PIN_BTN_PWR   18

void board_early_init(void) {
    gpio_config_t out = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_PWR) | (1ULL << PIN_AUDIO_PWR) | (1ULL << PIN_VBAT_HOLD) };
    gpio_config(&out);
    gpio_hold_dis(PIN_VBAT_HOLD);
    gpio_set_level(PIN_VBAT_HOLD, 1);          /* keep the board alive */
    gpio_set_level(PIN_EPD_PWR, 1);            /* rails off until needed */
    gpio_set_level(PIN_AUDIO_PWR, 1);
    gpio_config_t in = { .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .pin_bit_mask = (1ULL << PIN_BTN_REC) | (1ULL << PIN_BTN_PWR) };
    gpio_config(&in);
}

void board_rail_epd(int on)   { gpio_set_level(PIN_EPD_PWR, !on); }
void board_rail_audio(int on) { gpio_set_level(PIN_AUDIO_PWR, !on); }

wake_cause_t board_wake_cause(void) {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_EXT1: {
        uint64_t pins = esp_sleep_get_ext1_wakeup_status();
        if (pins & (1ULL << PIN_BTN_REC)) return WAKE_REC_BUTTON;
        return WAKE_PWR_BUTTON;
    }
    case ESP_SLEEP_WAKEUP_TIMER: return WAKE_TIMER;
    default: return WAKE_COLD;
    }
}

void board_deep_sleep(unsigned seconds) {
    gpio_set_level(PIN_VBAT_HOLD, 1);
    gpio_hold_en(PIN_VBAT_HOLD);               /* survives deep sleep */
    esp_sleep_enable_ext1_wakeup((1ULL << PIN_BTN_REC) | (1ULL << PIN_BTN_PWR),
                                 ESP_EXT1_WAKEUP_ANY_LOW);
    if (seconds) esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    esp_deep_sleep_start();
}

void board_power_off(void) {
    gpio_hold_dis(PIN_VBAT_HOLD);
    gpio_set_level(PIN_VBAT_HOLD, 0);          /* hardware powers down */
    while (1) { }
}
```

`firmware/components/board/src/buttons.c`:

```c
#include "board.h"
#include "driver/gpio.h"
int board_btn_rec(void) { return gpio_get_level(0) == 0; }
int board_btn_pwr(void) { return gpio_get_level(18) == 0; }
```

`firmware/main/main.c` (C1 bring-up harness):

```c
#include <stdio.h>
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "htp";

void app_main(void) {
    board_early_init();
    const char *cause[] = { "cold", "rec-button", "pwr-button", "timer" };
    ESP_LOGI(TAG, "HTP terminal bring-up C1, wake=%s", cause[board_wake_cause()]);
    for (int i = 0; i < 100; i++) {            /* 10 s of button echo */
        ESP_LOGI(TAG, "rec=%d pwr=%d", board_btn_rec(), board_btn_pwr());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "sleeping; press either button to wake (timer in 30 s)");
    board_deep_sleep(30);
}
```

- [ ] **Step 4: Build on this host**

```bash
source ~/esp/esp-idf/export.sh
idf.py -C firmware build
```

Expected: `Project build complete`, binaries under `firmware/build/`.

- [ ] **Step 5: Flash tooling for the operator's PC**

`firmware/tools/pack-flash.sh`:

```bash
#!/usr/bin/env bash
# Zip the flashable artifacts for a machine with USB access to the device.
set -euo pipefail
B="$(dirname "$0")/../build"
OUT="$B/flash-pack.zip"
rm -f "$OUT"
python3 - "$B" <<'EOF2'
import json, os, sys, zipfile
b = sys.argv[1]
args = json.load(open(os.path.join(b, "flasher_args.json")))
files = [v for _, v in sorted(args["flash_files"].items())]
cmd = "python -m esptool --chip esp32s3 -b 460800 write_flash " + " ".join(
    f"{off} {os.path.basename(path)}" for off, path in sorted(args["flash_files"].items()))
with zipfile.ZipFile(os.path.join(b, "flash-pack.zip"), "w") as z:
    for f in files:
        z.write(os.path.join(b, f), os.path.basename(f))
    z.writestr("FLASH-COMMAND.txt", cmd + "\n")
print("wrote", os.path.join(b, "flash-pack.zip"))
EOF2
```

`firmware/tools/FLASHING.md`:

```markdown
# Flashing from a PC with USB access

1. `pip install esptool pyserial` (once).
2. Fetch `firmware/build/flash-pack.zip` from the build host and unzip it.
3. Put the device in download mode if needed (hold the Record/BOOT button while
   plugging in USB), find the serial port (Windows: Device Manager COMx;
   macOS/Linux: `ls /dev/tty.*` or `/dev/ttyACM*`).
4. Run the command in `FLASH-COMMAND.txt`, adding `--port <PORT>`.
5. Serial monitor: `python -m serial.tools.miniterm <PORT> 115200`
   (exit: Ctrl-]). Copy/paste the log lines back into the session.
```

- [ ] **Step 6: USER CHECKPOINT C1 — stop and wait**

Run `bash firmware/tools/pack-flash.sh`, tell the operator where the zip is, and ask
them to flash and report. **Pass criteria (all four):**
1. Boot banner `HTP terminal bring-up C1, wake=cold` appears at 115200 baud.
2. Pressing each button flips the matching `rec=`/`pwr=` field in the log.
3. After 10 s the device sleeps; pressing Record wakes it with `wake=rec-button`, Power with `wake=pwr-button`.
4. Left alone, it wakes with `wake=timer` after 30 s.

- [ ] **Step 7: Commit** — `git add firmware/ .gitignore && git commit -m "feat(firmware): IDF scaffold, power rails, buttons, deep-sleep wake (C1)"`

---

### Task 13: SSD1681 e-paper driver — Checkpoint C2

**Files:**
- Create: `firmware/components/board/src/epd_ssd1681.c`
- Modify: `firmware/components/board/include/board.h`, `firmware/components/board/CMakeLists.txt` (add source), `firmware/main/main.c` (C2 harness)

**Interfaces:**
- Consumes: `ui_fb_t` (Task 8; `fb.px` is already in SSD1681 RAM format), `board_rail_epd` (Task 12). Pins: DC 10, CS 11, SCK 12, MOSI 13, RST 9, BUSY 8, SPI2.
- Produces (append to `board.h`):

```c
int  epd_init(void);                       /* rail on, HW init; 0 ok */
void epd_full(const uint8_t *fb5000);      /* full refresh (~1.5 s, flashes) */
void epd_partial(const uint8_t *fb5000);   /* partial refresh (fast, may ghost) */
void epd_sleep(void);                      /* deep-sleep cmd + rail off */
```

- [ ] **Step 1: Implement the driver**

`epd_ssd1681.c` — SPI at 10 MHz via `spi_bus_initialize`/`spi_bus_add_device` (mode 0),
DC/RST/BUSY as plain GPIOs, `busy_wait()` polls BUSY low with a 3 s timeout. Command
sequence (standard SSD1681 bring-up; **cross-check opcodes and ordering against
`reference/pala_note/src/display/epaper_driver_bsp.cpp` — consult only, copy nothing**):

```c
/* init */
reset_pulse();                    /* RST low 10 ms, high, wait busy */
cmd(0x12); busy_wait();           /* SWRESET */
cmd(0x01); data3(0xC7, 0x00, 0x00);   /* driver output: 200 lines */
cmd(0x11); data1(0x03);           /* data entry: x+ y+ */
cmd(0x44); data2(0x00, 0x18);     /* x window: 0..24 (25 bytes) */
cmd(0x45); data4(0x00, 0x00, 0xC7, 0x00);  /* y window: 0..199 */
cmd(0x3C); data1(0x05);           /* border waveform */
cmd(0x18); data1(0x80);           /* temp sensor: internal */
busy_wait();

/* frame write (both full and partial) */
cmd(0x4E); data1(0x00); cmd(0x4F); data2(0x00, 0x00);   /* RAM counters */
cmd(0x24); dataN(fb5000, 5000);   /* B/W RAM */

/* full refresh: OTP waveform — no LUT upload, nothing licensed */
cmd(0x22); data1(0xF7); cmd(0x20); busy_wait();

/* partial refresh: display mode 2 (ping-pong) */
cmd(0x22); data1(0xFF); cmd(0x20); busy_wait();

/* sleep */
cmd(0x10); data1(0x01);           /* deep sleep mode 1 */
```

`epd_full` = init-window + frame write + `0xF7` update; also write the same buffer to
RAM `0x26` (the "red"/previous buffer) before a full refresh so the controller's
ping-pong base is coherent for subsequent partials. `epd_partial` = frame write to
`0x24` + `0xFF` update. If Mode-2 partial ghosts unacceptably at C2, the documented
fallback (design §11.2) is a datasheet-derived partial LUT — write it from the SSD1681
datasheet timing tables, not from the reference firmware's `WF_PARTIAL_1IN54_0` array.

- [ ] **Step 2: C2 harness in `main.c`**

Replace the button-echo loop body with:

```c
    board_early_init();
    ESP_LOGI(TAG, "C2 display test");
    static ui_fb_t fb;
    if (epd_init() != 0) { ESP_LOGE(TAG, "epd_init failed"); board_deep_sleep(0); }
    fb_clear(&fb);
    fb_text(&fb, 20, 20, "HTP C2", 2, 1);
    fb_rect(&fb, 5, 5, 190, 190, 1);
    for (int x = 0; x < 200; x += 10) for (int y = 100; y < 120; y += 4)
        fb_fill(&fb, x, y, 5, 2, 1);
    epd_full(fb.px);
    for (int i = 1; i <= 9; i++) {          /* 9 partials exercise the ghosting rule */
        char n[16]; snprintf(n, sizeof n, "partial %d", i);
        fb_fill(&fb, 20, 60, 160, 20, 0);
        fb_text(&fb, 20, 60, n, 1, 1);
        epd_partial(fb.px);
        vTaskDelay(pdMS_TO_TICKS(800));
    }
    epd_full(fb.px);                        /* ghost-clearing full */
    epd_sleep();
    ESP_LOGI(TAG, "C2 done, sleeping");
    board_deep_sleep(0);
```

- [ ] **Step 3: Build** — `idf.py -C firmware build` clean.

- [ ] **Step 4: USER CHECKPOINT C2 — stop and wait.** Pack, flash, observe. **Pass:** (1) full refresh shows the bordered "HTP C2" pattern crisply; (2) the nine partial updates change only the counter line, quickly, without a full-screen flash; (3) the final full refresh clears any accumulated ghosting; (4) serial shows `C2 done`. Record in the commit message whether Mode-2 partial was acceptable or the datasheet-LUT fallback was needed.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): SSD1681 e-paper driver, full+partial refresh (C2)"`

---

### Task 14: SD card, NVS, on-device config — Checkpoint C3

**Files:**
- Create: `firmware/components/board/src/sdcard.c`
- Create: `firmware/main/idf_ports.c`, `firmware/main/idf_ports.h`
- Modify: `firmware/components/board/include/board.h`, `firmware/components/board/CMakeLists.txt`, `firmware/main/CMakeLists.txt` (add `idf_ports.c`), `firmware/main/main.c` (C3 harness)

**Interfaces:**
- Consumes: `port_storage_t`/`port_kv_t`/`port_clock_t`/`port_rng_t` (Task 1), `app_config_parse`/`wifi_profiles_parse` (Task 4).
- Produces:

```c
// board.h additions
int board_sd_mount(void);          /* SD-MMC 1-bit CLK 39 CMD 41 D0 40 -> VFS at /sdcard */

// idf_ports.h
void idf_ports_init(port_storage_t *st, port_kv_t *kv, port_clock_t *ck, port_rng_t *rng);
/* storage: paths map "/x" -> "/sdcard/x"; write = tmp file + rename (atomic);
 * kv: NVS namespace "htp";
 * clock: epoch from settimeofday-backed time() ONCE Task 17 wires the RTC —
 *        until then epoch_s() returns 0 (clockless path); mono from esp_timer;
 * rng: esp_fill_random */
```

- [ ] **Step 1: Implement**

`sdcard.c`: `sdmmc_host_t host = SDMMC_HOST_DEFAULT()` with `host.flags =
SDMMC_HOST_FLAG_1BIT`; `sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT()` with
`.width = 1, .clk = 39, .cmd = 41, .d0 = 40`; `esp_vfs_fat_sdmmc_mount("/sdcard", ...)`
with `max_files = 8`. Create `/sdcard/rec` if missing (`mkdir`). Return -1 on mount
failure (main renders the SD-error screen).

`idf_ports.c`: storage functions over `stdio` (`fopen`, `fread`, `fwrite`, `rename`,
`remove`, `stat`, `opendir`/`readdir` for `list`); `free_bytes` via
`esp_vfs_fat_info("/sdcard", &total, &free)`; every path prefixed with `/sdcard`.
kv over `nvs_open("htp", NVS_READWRITE, ...)` + `nvs_get_str`/`nvs_set_str` + commit.
clock: `epoch_s` returns `time(NULL)` when `> 1600000000` else `0`; `mono_ms` =
`esp_timer_get_time()/1000`; `sleep_ms` = `vTaskDelay`. rng = `esp_fill_random`.

- [ ] **Step 2: C3 harness in `main.c`** — mount SD, load `/config.json` and
`/wifi.json` through the port + Task 4 parsers, print (token masked to first 4 chars +
length), print `free_bytes`, list `/rec`, write + read back a kv key, then show a
"Config OK" screen via the display driver and sleep:

```c
    board_early_init();
    if (board_sd_mount() != 0) { ESP_LOGE(TAG, "SD mount failed"); board_deep_sleep(0); }
    port_storage_t st; port_kv_t kv; port_clock_t ck; port_rng_t rng;
    idf_ports_init(&st, &kv, &ck, &rng);
    char buf[2048]; size_t len;
    app_config_t cfg;
    if (st.read(st.ctx, "/config.json", buf, sizeof buf, &len) != 0 ||
        app_config_parse(buf, len, &cfg) != 0) {
        ESP_LOGE(TAG, "config.json missing or invalid"); board_deep_sleep(0);
    }
    ESP_LOGI(TAG, "bridge=%s token=%.4s...(%d) sync=%d",
             cfg.bridge_url, cfg.token, (int)strlen(cfg.token), cfg.sync_interval_s);
    wifi_profiles_t wp;
    if (st.read(st.ctx, "/wifi.json", buf, sizeof buf, &len) == 0 &&
        wifi_profiles_parse(buf, len, &wp) == 0)
        ESP_LOGI(TAG, "wifi profiles: %d (first: %s)", wp.count, wp.nets[0].ssid);
    ESP_LOGI(TAG, "sd free: %lld bytes", st.free_bytes(st.ctx));
    kv.set(kv.ctx, "c3", "ok"); char v[8];
    ESP_LOGI(TAG, "nvs roundtrip: %s", kv.get(kv.ctx, "c3", v, sizeof v) == 0 ? v : "FAIL");
```

- [ ] **Step 3: Build** — `idf.py -C firmware build` clean.

- [ ] **Step 4: USER CHECKPOINT C3 — stop and wait.** Operator prepares the SD card
(FAT32): `/config.json` with the real bridge URL + token, `/wifi.json` with real
networks (these never leave the card), inserts it, flashes, reports serial. **Pass:**
config echoed with masked token, wifi profile count correct, free-bytes sane, NVS
round-trip `ok`, "Config OK" on the panel.

- [ ] **Step 5: Commit** — `git add firmware/ && git commit -m "feat(firmware): SD-MMC storage, NVS kv, on-device config load (C3)"`

---

### Task 15: ES8311 audio — record to SD, play from SD — Checkpoint C4

**Files:**
- Create: `firmware/components/board/src/audio.c`, `firmware/components/board/idf_component.yml`
- Modify: `firmware/components/board/include/board.h`, `firmware/components/board/CMakeLists.txt` (add source; `REQUIRES` gains the managed component), `firmware/main/main.c` (C4 harness)

**Interfaces:**
- Consumes: `wav_write_header`/`wav_parse_header` (Task 2), `board_rail_audio` (Task 12), SD VFS (Task 14). Pins: I2C SDA 47 / SCL 48 (codec control), I2S MCLK 14 / BCLK 15 / WS 38 / DOUT 45 / DIN 16, PA enable GPIO 46.
- Produces (append to `board.h`):

```c
int  audio_init(void);   /* rail on, I2C + I2S + esp_codec_dev up, 16 kHz ready */
/* Record 16 kHz/16-bit/mono WAV to path until keep_going() returns 0 or max_ms
 * elapses. Streams to SD as it goes; patches the header on stop.
 * Returns data bytes written, or -1 (SD write failure -> explicit error). */
long audio_record_to(const char *path, int (*keep_going)(void *), void *ctx, unsigned max_ms);
/* Plays a WAV from SD; sample rate/channels come from ITS header (16 k uploads,
 * 24 k replies both work). stop_now() polled between chunks. */
int  audio_play_wav(const char *path, int (*stop_now)(void *), void *ctx);
void audio_beep(void);   /* short 1 kHz chime, generated, no asset */
void audio_deinit(void); /* PA off, codec closed, rail off */
```

- [ ] **Step 1: Declare the managed component**

`firmware/components/board/idf_component.yml`:

```yaml
dependencies:
  espressif/esp_codec_dev: "^1.3"
  idf: ">=5.0"
```

(`idf.py build` fetches it into `firmware/managed_components/` — gitignored, license-clean from upstream.)

- [ ] **Step 2: Implement `audio.c`**

Init (mirrors the codec bring-up proven by the reference firmware — ES8311 in
combined ADC+DAC mode, I2S std, MCLK from GPIO 14):

- I2C master bus on SDA 47 / SCL 48, 100 kHz; probe address `0x18` (ES8311 default).
- I2S channel: `i2s_new_channel` (std mode, 16 kHz, 16-bit, stereo slots) with the pin set above.
- `esp_codec_dev`: `audio_codec_new_i2c_ctrl` + `es8311_codec_new` (`.codec_mode =
  ESP_CODEC_DEV_WORK_MODE_BOTH`, `.pa_pin = 46`, `.use_mclk = true`, `.pa_reverted =
  false`) + `audio_codec_new_i2s_data` → `esp_codec_dev_new`. Open with
  `esp_codec_dev_sample_info_t{ .sample_rate = 16000, .channel = 2, .bits_per_sample = 16 }`.
  Set `esp_codec_dev_set_in_gain(dev, 30.0)` and `esp_codec_dev_set_out_vol(dev, 85)`.

`audio_record_to`: open file, write 44 zero bytes, loop: `esp_codec_dev_read(dev, sbuf,
8192)` (stereo), keep every other sample (left channel) into a 4096-byte mono buffer,
`fwrite`; on `fwrite` short-count → close, return -1. On stop: `fseek(0)` and
`wav_write_header(hdr, 16000, 16, 1, total)`, rewrite, close, return total. Enforce
`max_ms` via `esp_timer_get_time()`.

`audio_play_wav`: read first 512 bytes, `wav_parse_header`; `esp_codec_dev_close` +
re-open at the parsed rate; `fseek(data_offset)`; loop 2048-byte mono chunks → duplicate
to stereo → `esp_codec_dev_write`; poll `stop_now` between chunks. `audio_beep`:
generate 200 ms of 1 kHz square wave at low amplitude into a stack buffer and write it.

- [ ] **Step 3: C4 harness in `main.c`** — after SD mount + `audio_init`:

```c
    ESP_LOGI(TAG, "C4: hold REC and speak; release to stop");
    while (!board_btn_rec()) vTaskDelay(pdMS_TO_TICKS(20));
    long n = audio_record_to("/sdcard/rec/c4-test.wav", (int (*)(void *))board_btn_rec,
                             NULL, 120000);
    ESP_LOGI(TAG, "recorded %ld bytes", n);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "playback...");
    audio_play_wav("/sdcard/rec/c4-test.wav", NULL, NULL);
    audio_beep();
    audio_deinit();
    ESP_LOGI(TAG, "C4 done");
```

(`keep_going` = `board_btn_rec` works because pressed == 1.)

- [ ] **Step 4: Build** — `idf.py -C firmware build` (first run fetches `esp_codec_dev`).

- [ ] **Step 5: USER CHECKPOINT C4 — stop and wait.** **Pass:** (1) recording starts
promptly on press, stops on release, byte count ≈ 32,000 × seconds spoken; (2) playback
through the speaker is intelligible speech at correct pitch/speed (wrong rate sounds
chipmunk/slow-motion); (3) the chime is audible; (4) operator pulls the SD card and
verifies `c4-test.wav` plays in a desktop player (validates the header end-to-end).

- [ ] **Step 6: Commit** — `git add firmware/ && git commit -m "feat(firmware): ES8311 record/playback via esp_codec_dev (C4)"`

---

### Task 16: Wi-Fi, real transport, battery header — Checkpoint C5

**Files:**
- Create: `firmware/main/idf_wifi.c`, `firmware/main/idf_wifi.h`, `firmware/main/idf_transport.c`, `firmware/main/idf_transport.h`
- Create: `firmware/components/board/src/battery.c`
- Modify: `firmware/main/CMakeLists.txt` (add sources), `firmware/components/board/include/board.h` (`int board_battery_pct(void);`), `firmware/components/board/CMakeLists.txt`, `firmware/main/main.c` (C5 harness)

**Interfaces:**
- Consumes: `wifi_profiles_t`/`wifi_select_profile`/`wifi_fast_join_*` (Task 4), `htp_transport_t` contract (Task 3), kv port (Task 14).
- Produces:

```c
// idf_wifi.h
int idf_wifi_connect(const wifi_profiles_t *p, port_kv_t *kv, unsigned timeout_ms);
    /* BSSID fast join from kv when cached; else scan + wifi_select_profile.
     * On success stores bssid+channel back to kv. 0 ok, -1 no network. */
int idf_wifi_start_connect_async(const wifi_profiles_t *p, port_kv_t *kv);
    /* begins the join without blocking (used while recording); pair with: */
int idf_wifi_wait_connected(unsigned timeout_ms);
void idf_wifi_stop(void);

// idf_transport.h
void idf_transport_init(htp_transport_t *out, const char *base_url);
    /* esp_http_client implementation of the Task 3 contract:
     * - base_url + req->path; https:// uses esp_crt_bundle_attach, http:// is
     *   honored as-is (LAN dev mode)
     * - body_file streamed from SD in 4 KB chunks (esp_http_client_open/write)
     * - sink_file: response streamed to SD, "<sink>.part" then rename
     * - req->timeout_ms mandatory; response body (JSON) buffered up to 16 KB in
     *   a static buffer, NUL-terminated */

// board.h addition
int board_battery_pct(void);   /* ADC1 ch3 (GPIO 4), oneshot + curve; clamped 0..100 */
```

- [ ] **Step 1: Implement `idf_wifi.c`** — `esp_netif_init` + default station;
`start_connect_async`: if kv has a cached BSSID+channel, `esp_wifi_set_config` with
`.sta.bssid_set = 1`, `.sta.channel = ch` and connect (the ~1 s fast path); else
`esp_wifi_scan_start` (active, all channels), build `wifi_scan_hit_t[]` from the AP
records, `wifi_select_profile`, connect to the winner (with static IP via
`esp_netif_set_ip_info` when the profile carries one). On `IP_EVENT_STA_GOT_IP`, store
BSSID+channel via `wifi_fast_join_store`. Fast-join failure falls back to scan once
before giving up (design §5.5). `idf_wifi_connect` = async + wait.

- [ ] **Step 2: Implement `idf_transport.c`** — one static 16 KB response buffer;
`perform()`: build URL `snprintf("%s%s", base_url, req->path)`;
`esp_http_client_config_t{ .url, .timeout_ms = req->timeout_ms, .crt_bundle_attach =
esp_crt_bundle_attach when https }`; set headers; for `body_file`:
`esp_http_client_open(client, file_size)` then 4 KB `fread`/`esp_http_client_write`
loop; for `sink_file`: `esp_http_client_read` loop into `<sink>.part`, `rename` on
success (a torn download never masquerades as a whole reply). Map transport-level
`ESP_FAIL`/timeout to `resp->transport_err = 1`; otherwise fill `status`/`body`.

- [ ] **Step 3: Implement `battery.c`** — `adc_oneshot` on ADC1 channel 3, 12-bit,
`ADC_ATTEN_DB_12`; average 8 samples; assume a 2:1 divider (reference hardware powers
the sense path via the VBAT rail): `mv = raw_mv * 2`; linear map 3300 mV → 0, 4200 mV →
100, clamped. Marked for calibration at Task 19 (design §11.3).

- [ ] **Step 4: C5 harness in `main.c`** — mount SD, load config/wifi, connect, then run
the *host-proven* client against the *mock on the LAN*:

```c
    idf_transport_init(&tr, cfg.bridge_url);
    htp_client_t cl; htp_client_init(&cl, &tr, cfg.token);
    cl.battery_pct = board_battery_pct();
    htp_dashboard_t d;
    int r = htp_get_dashboard(&cl, "", &d);
    ESP_LOGI(TAG, "dashboard r=%d rev=%s items=%d sync=%d", r, d.rev, d.item_count, d.sync_interval);
    /* upload the C4 recording if present, then poll it */
    htp_upload_params_t p = { .capture_id = "c-c5-0001", .wav_path = "/rec/c4-test.wav",
                              .recorded_at = 0 };
    ESP_LOGI(TAG, "upload r=%d", htp_upload_capture(&cl, &p));
    const char *ids[1] = { "c-c5-0001" };
    htp_capture_status_t stt[1]; long long t;
    ESP_LOGI(TAG, "poll n=%d state=%d transcript=%s",
             htp_poll_captures(&cl, ids, 1, stt, 1, &t), stt[0].state, stt[0].transcript);
    ESP_LOGI(TAG, "reply dl r=%d", htp_download_reply(&cl, "c-c5-0001", "/reply.tmp.wav"));
    audio_init();
    audio_play_wav("/sdcard/reply.tmp.wav", NULL, NULL);
    ESP_LOGI(TAG, "battery=%d%%", board_battery_pct());
```

Note the port-path convention: `htp_upload_capture` gets `/rec/...` (transport reads
through the same `/sdcard` prefixing as `idf_ports` — implement `idf_transport`'s file
access via the storage port to keep one path convention).

- [ ] **Step 5: Build; start the mock reachable from the LAN**

```bash
idf.py -C firmware build && bash firmware/tools/pack-flash.sh
# on this host, in another shell (LAN-reachable mock for the device):
bridge/.venv/bin/htp-bridge --mock --host 0.0.0.0 --port 8787
```

Operator sets `/config.json` `bridge_url` to `http://<build-host-LAN-IP>:8787` (real IP
stays on the card, never in the repo).

- [ ] **Step 6: USER CHECKPOINT C5 — stop and wait.** **Pass:** (1) Wi-Fi joins (fast
join on the second boot — serial shows the cached-BSSID path); (2) dashboard fetch
returns `rev=mockrev1 items=2 sync=600`; (3) upload returns 0 and the poll shows
`reply_ready` with the mock transcript; (4) `reply.tmp.wav` downloads and plays
(~1 s of silence is the expected mock audio); (5) battery percent is plausible.

- [ ] **Step 7: Commit** — `git add firmware/ && git commit -m "feat(firmware): Wi-Fi fast-join, esp_http_client transport, battery ADC (C5)"`

---

### Task 17: Wake dispatch and the full capture session — Checkpoint C6

**Files:**
- Create: `firmware/components/board/src/rtc_pcf8563.c`
- Modify: `firmware/components/board/include/board.h`, `firmware/components/board/CMakeLists.txt`, `firmware/main/main.c` (final structure), `firmware/main/idf_ports.c` (epoch from RTC)

**Interfaces:**
- Consumes: everything — this is the wiring task. `capture_run` (Task 6), `sync_cycle` (Task 7), `htp_make_capture_id` (Task 1), audio (Task 15), Wi-Fi/transport (Task 16), EPD + ui (Tasks 8/9/13).
- Produces (append to `board.h`):

```c
long long board_rtc_get(void);         /* PCF8563 @ I2C 0x51 -> epoch; 0 if unset  */
void      board_rtc_set(long long epoch);
```

`main.c` final shape (replaces all bring-up harnesses; UI-session and timer paths are
stubs until Task 18):

```c
void app_main(void) {
    board_early_init();
    wake_cause_t wake = board_wake_cause();
    if (wake == WAKE_REC_BUTTON) { capture_session(); }     /* fast path first */
    else if (wake == WAKE_PWR_BUTTON) { ui_session_stub(); }
    else { sync_session_stub(); }                            /* timer + cold    */
    board_deep_sleep(next_sync_interval());                  /* kv "sync_s", default 600 */
}
```

- [ ] **Step 1: RTC driver** — PCF8563 at I2C address 0x51 (shared bus with the codec —
initialize the I2C master bus once, in a `board_i2c_bus()` accessor both drivers use):
`board_rtc_get` reads registers 0x02–0x08 (BCD seconds..years, VL bit ⇒ return 0),
converts to epoch via the inverse of Task 1's civil_from_days (implement
`days_from_civil` in `rtc_pcf8563.c`); `board_rtc_set` writes BCD back. Wire
`idf_ports`'s `epoch_s` to prefer `board_rtc_get()`.

- [ ] **Step 2: Implement `capture_session()`** (in `main.c`; sequential, matching design §5.2):

```c
static void capture_session(void) {
    /* 1. capture-critical init only: audio rail, SD */
    if (board_sd_mount() != 0) { screen_fatal("SD card error"); return; }
    idf_ports_init(&st, &kv, &ck, &rng);
    if (load_config(&cfg) != 0) { screen_fatal("Config error"); return; }
    if (audio_init() != 0) { screen_fatal("Audio error"); return; }

    /* 2. capture id + files, recording glyph, Wi-Fi join in parallel */
    uint8_t r2[2]; rng.fill(rng.ctx, r2, 2);
    char id[64];
    htp_make_capture_id(id, board_rtc_get(), boot_count_bump(&kv), ck.mono_ms(ck.ctx), r2);
    load_wifi_profiles(&wp);
    idf_wifi_start_connect_async(&wp, &kv);
    epd_init(); draw_recording_glyph();      /* small centered dot + "REC" */

    char wav[96]; sidecar_wav_path(wav, id);
    long bytes = audio_record_to(wav, (int (*)(void *))board_btn_rec, NULL, 120000);
    if (bytes < 0) { screen_fatal("SD full"); return; }
    if (bytes < 8000) { screen_status("Too short"); goto sleep_path; }  /* < 250 ms */

    /* 3. durable: sidecar + index */
    sidecar_t sc; sidecar_init(&sc, id);
    sc.recorded_at = board_rtc_get();
    sidecar_save(&stor, &sc); rec_index_append(&stor, id);

    /* 4. network path */
    if (idf_wifi_wait_connected(8000) != 0) { screen_status("Saved, will upload later"); goto sleep_path; }
    idf_transport_init(&tr, cfg.bridge_url);
    htp_client_init(&cl, &tr, cfg.token); cl.battery_pct = board_battery_pct();
    capture_ctx_t cx = { .client = &cl, .storage = &stor, .clock = &ck,
        .on_status = screen_status_cb, .poll_interval_ms = 1000,
        .poll_window_ms = 60000, .reply_path = "/reply.tmp.wav" };
    capture_outcome_t out = capture_run(&cx, &sc);

    switch (out) {
    case CAPTURE_DONE:        screen_status_transcript("Noted", sc.transcript); break;
    case CAPTURE_REPLY_READY: play_reply_and_follow_up(&sc, &cx); break;
    case CAPTURE_FAILED:      screen_status("Failed - saved on card"); break;
    case CAPTURE_OFFLINE:     screen_status("Saved, will upload later"); break;
    case CAPTURE_TIMEOUT:     screen_status("Still working - check later"); break;
    case CAPTURE_AUTH_ERROR:  screen_status("Auth error - check token"); break;
    }
    run_sync(&cl);                            /* design §5.4: sync after every session */
sleep_path:
    epd_sleep(); audio_deinit();
}
```

`play_reply_and_follow_up`: `audio_play_wav("/sdcard/reply.tmp.wav", btn_rec_stop,
NULL)`, delete the temp file, then a 30 s window polling `gesture_feed`; on
`GEST_REC_HOLD_START`, record a new capture with `sc.conversation_id` copied into the
new sidecar before `capture_run` (loop back). `screen_status`: status line + big scale-2
message via `fb_text`, `epd_partial`. `run_sync`: build `sync_ctx_t` with render
callbacks that draw dashboard/banner into the framebuffer and `epd_partial`/`epd_full`
per `ui_flow_wants_full`, chime = `audio_beep`, `set_rtc = board_rtc_set` — then
`sync_cycle`.

Timing guard for the 250 ms target: `audio_record_to` must be running before any
display or Wi-Fi work; the order in step 2 above is normative. Log
`esp_timer_get_time()` at boot-entry and first I2S read; C6 verifies < 400 ms
(the EXT1 boot itself costs ~100 ms).

- [ ] **Step 3: `sync_session_stub` / `ui_session_stub`** — both: mount, config, Wi-Fi,
one `sync_cycle` with epd rendering, sleep. (Task 18 replaces the UI stub.)

- [ ] **Step 4: Build** — `idf.py -C firmware build` clean; run host suite too (`ctest`) — still green.

- [ ] **Step 5: USER CHECKPOINT C6 — stop and wait.** Two runs:

*Against the LAN mock* (as C5): hold-record-speak-release → "Uploaded" → mock says
`reply_ready` → silent reply plays → follow-up window opens → device sleeps.

*Against the real bridge* (operator points `bridge_url` at the real bridge's LAN
address; bridge must be reachable from the device's network): (1) plain note — speak
without the salutation → "Noted" + opening transcript words on the panel within a few
seconds; (2) conversation — "Hey Hermes, …" → spoken TTS reply from the speaker →
follow-up hold-to-talk continues the same conversation (serial shows the echoed
`X-Conversation-Id`); (3) airplane-mode test — take the AP down, record → "Saved, will
upload later"; AP back up, press power (any wake) → sync retries and the note lands
(serial + bridge logs agree); (4) serial timing line shows record-start < 400 ms from
boot.

- [ ] **Step 6: Commit** — `git add firmware/ && git commit -m "feat(firmware): wake dispatch and full capture session (C6)"`

---

### Task 18: UI sessions, silent sync, power policy — Checkpoint C7

**Files:**
- Modify: `firmware/main/main.c` (replace both stubs; add the awake watchdog and power policy)

**Interfaces:**
- Consumes: `ui_flow_*` (Task 10), `gesture_*` (Task 9), everything wired in Task 17.
- Produces: the finished `main.c`. No new public interfaces.

- [ ] **Step 1: Replace `ui_session_stub` with `ui_session()`**

```c
static void ui_session(void) {
    if (common_init() != 0) return;            /* mount, config, ports (factored from Task 17) */
    epd_init();
    ui_flow_t u; ui_flow_init(&u, &cl, &stor, &kv);
    populate_settings_info(&u.info);           /* esp_read_mac -> "AA:BB:..", FW_VERSION,
                                                  host part of cfg.bridge_url, kv "sync_s" */
    load_cached_dashboard(&u);                 /* kv "dash_rev" + last snapshot json on SD:
                                                  write /dash.json in run_sync's render cb,
                                                  read it here so the panel is instant */
    ui_fb_t fb; ui_flow_render(&u, &fb); epd_full(fb.px);

    /* connect in the background; run a sync once up, then keep serving gestures */
    load_wifi_profiles(&wp); idf_wifi_start_connect_async(&wp, &kv);
    int synced = 0;
    gesture_fsm_t g; gesture_init(&g);
    unsigned idle_t0 = ck.mono_ms(ck.ctx);
    while (ck.mono_ms(ck.ctx) - idle_t0 < 30000) {          /* 30 s idle -> sleep */
        if (!synced && idf_wifi_wait_connected(0) == 0)      /* non-blocking probe */
            { run_sync_into_flow(&u); synced = 1; ui_flow_render(&u, &fb); epd_partial(fb.px); }
        gesture_t ge = gesture_feed(&g, board_btn_rec(), board_btn_pwr(), ck.mono_ms(ck.ctx));
        if (ge == GEST_NONE) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        idle_t0 = ck.mono_ms(ck.ctx);
        char path[96];
        ui_action_t a = ui_flow_gesture(&u, ge, path);
        switch (a) {
        case UIF_START_CAPTURE: capture_session(); return;
        case UIF_PLAY_WAV:
            audio_init();
            audio_play_wav(sdpath(path), btn_rec_stop, NULL);   /* "/rec/x" -> "/sdcard/rec/x" */
            audio_deinit();
            break;
        case UIF_POWER_OFF:
            screen_status("Powering off");     /* the 5 s hold already showed countdown */
            board_power_off();
        case UIF_REDRAW_PARTIAL:
        case UIF_REDRAW_FULL: {
            ui_flow_render(&u, &fb);
            if (ui_flow_wants_full(&u, a)) epd_full(fb.px); else epd_partial(fb.px);
            break; }
        default: break;
        }
    }
    epd_sleep();
}
```

Power-off countdown: while PWR is held past 2 s, `screen_status("Hold to power off...")`
once (partial refresh) — the `GEST_PWR_OFF` at 5 s then executes it. Implement by
checking `g.pwr_down && now - g.pwr_t0 > 2000` in the loop.

- [ ] **Step 2: Replace `sync_session_stub` with `sync_session()`** — the silent path:

```c
static void sync_session(void) {
    if (common_init() != 0) return;
    load_wifi_profiles(&wp);
    if (idf_wifi_connect(&wp, &kv, 12000) != 0) return;      /* no network: back to sleep */
    idf_transport_init(&tr, cfg.bridge_url);
    htp_client_init(&cl, &tr, cfg.token);
    cl.battery_pct = board_battery_pct();
    /* battery policy (design §8): <15% -> 4x interval; <5% -> skip timer syncs */
    if (cl.battery_pct < 5 && board_wake_cause() == WAKE_TIMER) return;
    sync_report_t rep;
    run_sync_report(&cl, &rep);      /* renders ONLY when something changed:      */
                                     /* render cbs lazily epd_init() on first call */
    ESP_LOGI(TAG, "sync: up=%d notif=%d acked=%d dash=%d next=%ds",
             rep.uploads_retried, rep.notifs_fetched, rep.notifs_acked,
             rep.dashboard_changed, rep.sync_interval_s);
}
```

and `next_sync_interval()` returns kv `"sync_s"` (×4 when battery < 15%), default 600.

- [ ] **Step 3: Awake watchdog** — in `app_main`, before dispatch, start a one-shot
`esp_timer` at `90 s + (wake==WAKE_REC_BUTTON ? 120 s : 0)` whose callback logs
`"awake cap hit"` and calls `board_deep_sleep(next_sync_interval())`. Any legitimate
session (max recording 120 s + upload + poll window 60 s < cap for capture; 30 s idle
< 90 s for UI) finishes first; a wedged one can no longer drain the battery (design §8).
Cancel the timer just before the normal `board_deep_sleep` call.

- [ ] **Step 4: Build + host suite** — `idf.py -C firmware build` and `ctest` both green.

- [ ] **Step 5: USER CHECKPOINT C7 — stop and wait.** Against the real bridge:
1. Power-button wake → dashboard appears instantly from cache, then updates once Wi-Fi syncs (partial refresh only if changed).
2. Cursor moves with PWR-short; REC-short completes an item → strike-through immediately; verify Hermes reflects it (agent memory updated via the bridge).
3. PWR-double cycles Dashboard → Recordings → Settings; Recordings shows newest-first with transcripts; open an entry (PWR-long), page with PWR-short, play it (REC-short), stop with REC press.
4. Settings shows MAC / fw version / bridge host / interval.
5. Queue a test notification through Hermes ("Hermes, remind me: test banner at <time>") → within one sync interval the banner appears (chime only if urgent); REC-short dismisses; it does not reappear next sync (acked).
6. Idle 30 s → sleeps. Timer wake with nothing changed → serial shows the sync report, display never flashes.
7. Hold PWR 5 s → countdown then power-off; PWR press boots it back.

- [ ] **Step 6: Commit** — `git add firmware/ && git commit -m "feat(firmware): UI sessions, silent sync, power policy (C7)"`

---

### Task 19: Reliability checklist, provisioning docs, wrap-up

**Files:**
- Create: `firmware/README.md`
- Modify: `docs/superpowers/specs/2026-08-05-htp-firmware-design.md` (only if C-stage findings changed a documented behavior — record what and why in the commit)

**Interfaces:** none new. This task closes design §10/§11.

- [ ] **Step 1: Write `firmware/README.md`** — sections: what this is (one paragraph +
pointer to the design doc); build (`setup-idf.sh`, `idf.py -C firmware build`); host
tests (`cmake`/`ctest` lines from Task 1); flashing (pointer to `tools/FLASHING.md`);
SD provisioning — full worked examples of `/config.json` and `/wifi.json` with
placeholder values (`https://htp.example.net`, `<64-char-token>`, `<your-ssid>`) and
the note that these files never leave the card; troubleshooting table (SD error screen,
auth error screen, "Saved, will upload later", chipmunk audio = wrong-rate header,
ghosting = partial-refresh accumulation). Also record the final IDF version pin and the
C2 partial-refresh outcome (Mode-2 vs datasheet LUT) and the C4/C19 battery calibration
values.

- [ ] **Step 2: Battery calibration (design §11.3)** — operator reports the device's
serial `battery=%d%%` at full charge and (later, opportunistically) near-empty; adjust
the two endpoints in `battery.c` accordingly and note the measured values in README.
If a proper discharge measurement is impractical now, note the defaults as provisional.

- [ ] **Step 3: Execute the design §10 hardware checklist** (each row: operator action →
expected outcome from design §9's fault table; record PASS/FAIL inline here):

- [ ] Power removed mid-recording → on reboot the partial WAV exists; sidecar says `not_uploaded`; next wake uploads it (a truncated-but-valid-header file is fine because the header is patched only on clean stop — a torn file with a zeroed header must be *refused* by upload with a clear serial note, not crash: verify).
- [ ] Wi-Fi killed mid-upload → "Saved, will upload later"; AP restored → next wake delivers it exactly once (bridge log shows one ingestion — idempotency).
- [ ] Bridge stopped mid-conversation → device reports the timeout path ("Still working - check later"); bridge restarted → reply arrives as a notification on next sync (bridge redirect behavior).
- [ ] SD card filled (operator fills it with junk files) → recording refused with the explicit SD-full screen; nothing overwritten.
- [ ] Battery depleted overnight with a pending upload → on charge + boot, capture uploads; RTC either survived (PCF8563 backup) or capture IDs fall back to boot-counter form — both acceptable, note which.
- [ ] Device moved between both configured Wi-Fi networks → joins the right one at each location; second visit uses the fast join (serial confirms).

- [ ] **Step 4: Final green run** — `ctest --test-dir firmware/tests/host/build --output-on-failure` (all pass) and `idf.py -C firmware build` (clean).

- [ ] **Step 5: Commit** — `git add firmware/ docs/ && git commit -m "feat(firmware): provisioning docs and reliability checklist"`

- [ ] **Step 6: Use the superpowers:finishing-a-development-branch skill** to decide how this integrates (this plan was executed on a branch/worktree per the execution skill's setup).

---

## Plan Self-Review Record

Checked against `docs/superpowers/specs/2026-08-05-htp-firmware-design.md`:

- **Spec coverage:** §2 hardware (Tasks 12–17); §3 platform/licensing (Tasks 1, 8, 12, 15 — vendoring steps verify license headers); §4 architecture + purity (Task 1 layout, purity grep in commit steps); §5.1 wake dispatch (17), §5.2 capture flow (6, 17 — including the <400 ms record-start check and "Too short" floor), §5.3 reply download-then-play with header-driven rate (6, 15, 16 — `.part`+rename), §5.4 sync order + server-controlled interval (7), §5.5 network/fast-join/backoff (4, 16, 2); §6 storage/config/IDs (1, 4, 5, 14 — clockless upload omits X-Recorded-At, tested in Task 3); §7 UI screens/gestures/notifications/refresh discipline (8, 9, 10, 18); §8 power (12, 17, 18 — rails, EXT1+timer, idle/awake caps, battery policy); §9 error handling (fault paths distributed: 3 error mapping, 5 SD-full, 6 unknown-recovery, 14 fatal screens, 19 checklist); §10 testing (host suite Tasks 1–10, mock integration 11, checkpoints C1–C7, E2E checklist 19); §11 open items (IDF pin Task 12, partial-refresh confirmation C2, battery calibration 19, silence timeout stays config-gated default-off).
- **Known deliberate gap:** the design's optional `silence_timeout_s` is parsed (Task 4) but not acted on in `audio_record_to` — MVP ships release-to-stop only, matching design §11.4. Wiring it later is a ~10-line change in `audio_record_to`.
- **Type consistency:** `htp_client_t`/`port_*` signatures restated identically in Tasks 3, 6, 7, 10, 11, 16; `capture_run`/`capture_retry_pending` names match across 6/7/17; `ui_flow_*` matches across 10/18; `fb.px` format contract (bit set = white, MSB left) stated in both Task 8 and Task 13.
- **Placeholder scan:** the only intentionally-open implementation blank is the socket boilerplate comment in Task 11's POSIX transport (marked, standard POSIX), and Task 3's "remaining endpoints, same pattern" list — each of those five entries specifies its path, body, and parse fields explicitly.

