/* Copyright 2024 UV-K5 Firmware Custom
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "aprs_minimal.h"
#include "app.h"
#include "../audio.h"
#include "../functions.h"
#include "../misc.h"
#include "../radio.h"
#include "../settings.h"
#include "../driver/bk4819.h"
#include "../driver/system.h"
#ifdef ENABLE_UART
    #include "../driver/uart.h"
#endif

// External references
extern volatile uint32_t gGlobalSysTickCounter;
extern void SETTINGS_SaveAPRS(void);

// CRC-16 for AX.25 - minimal calculation (no lookup table to save space)
static uint16_t APRS_CalculateCRC(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8408;  // Reversed polynomial
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint16_t AX25_CalculateFCS(const uint8_t *data, uint16_t length)
{
    // AX.25/HDLC FCS: init 0xFFFF, poly 0x8408 (reflected), final XOR 0xFFFF.
    return (uint16_t)(~APRS_CalculateCRC(data, length));
}

static void AX25_EncodeAddress(const char *callsign, uint8_t ssid, bool last, uint8_t out7[7])
{
    bool end = false;
    for (uint8_t i = 0; i < 6; i++) {
        char c = ' ';
        if (!end) {
            if (callsign[i] == 0)
                end = true;  // stop reading past the terminator
            else
                c = callsign[i];
        }
        out7[i] = (uint8_t)(c << 1);
    }
    out7[6] = (uint8_t)(0x60 | ((ssid & 0x0F) << 1) | (last ? 0x01 : 0x00));
}

// ---------------------------------------------------------------------------
// Bell 202 TX via the BK4819 hardware FSK engine.
//
// The modem clocks the bits out itself (no software bit timing), but it knows
// nothing about AX.25/HDLC, so the full on-air bitstream - flags, bit
// stuffing, NRZI - is prepared in software and pushed through the FSK FIFO
// as raw bytes (engine CRC and scrambler disabled).
//
// Register recipe: this repo's proven MDC1200 TX sequence + the Bell 202
// tone setup from gvJaime/uv-k5-firmware-aprs (tone1 forced to 2200 Hz on
// top of the FFSK 1200/1800 TX mode) + kamilsss655's deviation and
// TX-filter measurements.
// ---------------------------------------------------------------------------

#define HDLC_BUF_SIZE    240u  // TX: encoded bitstream; RX: raw capture (shared, never simultaneous)
#define HDLC_LEAD_FLAGS  32u   // ~213 ms of 0x7E preamble
#define HDLC_TAIL_FLAGS  3u

typedef struct {
    uint8_t *buf;
    uint16_t bits;
    uint8_t  ones;
    uint8_t  level;  // current NRZI line level
} hdlc_writer_t;

static void HDLC_PutBit(hdlc_writer_t *w, bool bit)
{
    if (w->bits >= HDLC_BUF_SIZE * 8u)
        return;
    if (!bit)
        w->level ^= 1;  // NRZI: 0 = transition, 1 = no change
    if (w->level)
        w->buf[w->bits >> 3] |= 0x80u >> (w->bits & 7u);  // modem shifts MSB first
    w->bits++;
}

static void HDLC_PutByte(hdlc_writer_t *w, uint8_t b, bool stuff)
{
    for (uint8_t i = 0; i < 8; i++) {  // AX.25 sends bits LSB first
        const bool bit = (b >> i) & 1u;
        HDLC_PutBit(w, bit);
        if (stuff && bit) {
            if (++w->ones == 5) {
                HDLC_PutBit(w, false);  // stuff a 0 after 5 consecutive 1s
                w->ones = 0;
            }
        } else {
            w->ones = 0;
        }
    }
}

static uint8_t gHdlcBuf[HDLC_BUF_SIZE];

static void APRS_TransmitBell202(const uint8_t *frame, uint16_t frame_len)
{
    uint16_t i;

    memset(gHdlcBuf, 0, sizeof(gHdlcBuf));
    hdlc_writer_t w = { gHdlcBuf, 0, 0, 1 };

    for (i = 0; i < HDLC_LEAD_FLAGS; i++)
        HDLC_PutByte(&w, 0x7E, false);
    for (i = 0; i < frame_len; i++)
        HDLC_PutByte(&w, frame[i], true);
    for (i = 0; i < HDLC_TAIL_FLAGS; i++)
        HDLC_PutByte(&w, 0x7E, false);

    uint16_t nbytes = (w.bits + 7u) / 8u;
    if (nbytes & 1u)
        nbytes++;  // FIFO takes 16-bit words

    // Key TX using the normal radio TX path (PA, frequency, timers).
    RADIO_PrepareTX();
    if (gCurrentFunction != FUNCTION_TRANSMIT) {
        return;
    }

    BK4819_SetAF(BK4819_AF_MUTE);

    const uint16_t css_val  = BK4819_ReadRegister(BK4819_REG_51);
    const uint16_t dev_val  = BK4819_ReadRegister(BK4819_REG_40);
    const uint16_t filt_val = BK4819_ReadRegister(BK4819_REG_2B);

    BK4819_WriteRegister(BK4819_REG_51, 0);                            // CTCSS/CDCSS off
    BK4819_WriteRegister(BK4819_REG_40, (dev_val & 0xF000u) | 1200u);  // tone deviation ~3 kHz
    BK4819_WriteRegister(BK4819_REG_2B, (1u << 2) | (1u << 0));        // TX HPF + pre-emphasis off

    BK4819_WriteRegister(BK4819_REG_70,          // both tone generators on, gain 96
        (1u << 15) | (1u << 7) | (96u << 0));
    BK4819_WriteRegister(BK4819_REG_71, 22714);  // tone1: 2200 Hz (space)
    BK4819_WriteRegister(BK4819_REG_72, 12389);  // tone2: 1200 Hz (mark / bit clock)

    BK4819_WriteRegister(BK4819_REG_58,
        (1u << 13) |   // FSK TX mode: FFSK 1200/1800, tones overridden above
        (7u << 10) |   // FSK RX mode (unused while transmitting)
        (3u <<  8) |   // FSK RX gain
        (3u <<  6) |   // FSK enable (TRM REG_58<7:6> = 11; all Beken examples set this)
        (1u <<  1) |   // FSK RX bandwidth FFSK 1200/1800
        (1u <<  0));   // FSK enable

    BK4819_WriteRegister(BK4819_REG_5A, 0xAAAA);  // sync bytes double as extra
    BK4819_WriteRegister(BK4819_REG_5B, 0xAAAA);  // clock training before the flags
    BK4819_WriteRegister(BK4819_REG_5C, 0xAA30);  // engine CRC off
    BK4819_WriteRegister(BK4819_REG_5D, (uint16_t)((nbytes - 1u) << 8));

    BK4819_WriteRegister(BK4819_REG_59, 0x8068);  // clear TX FIFO; 4 sync bytes, 6 byte preamble
    BK4819_WriteRegister(BK4819_REG_59, 0x0068);

    for (i = 0; i < nbytes; i += 2)
        BK4819_WriteRegister(BK4819_REG_5F,
            (uint16_t)(gHdlcBuf[i + 1] << 8) | gHdlcBuf[i]);

    SYSTEM_DelayMs(20);

    BK4819_WriteRegister(BK4819_REG_59, 0x0868);  // FSK TX enable

    // 1200 baud -> 6.67 ms/byte; preamble(6) + sync(4) + payload, plus margin.
    SYSTEM_DelayMs(((10u + (uint32_t)nbytes) * 20u) / 3u + 100u);

    // Tear down the modem and unkey.
    BK4819_WriteRegister(BK4819_REG_59, 0x0068);
    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    BK4819_WriteRegister(BK4819_REG_58, 0x0000);
    BK4819_WriteRegister(BK4819_REG_40, dev_val);
    BK4819_WriteRegister(BK4819_REG_2B, filt_val);
    BK4819_WriteRegister(BK4819_REG_51, css_val);

    APP_EndTransmission();
    FUNCTION_Select(FUNCTION_FOREGROUND);
}

// Message compose fields (edited via the menu, RAM only)
char gAPRS_MsgTo[10];
char gAPRS_MsgText[31];

static uint16_t APRS_BuildHeader(uint8_t *frame)
{
    // APRS,WIDE1-1,WIDE2-1 path, source callsign/SSID from the menu settings.
    const char *src = gEeprom.APRS_CALLSIGN;
    if (src[0] <= ' ')
        src = "N0CALL";

    uint16_t idx = 0;
    AX25_EncodeAddress("APRS",  0, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress(src, gEeprom.APRS_SSID & 0x0F, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("WIDE1", 1, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("WIDE2", 1, true,  &frame[idx]); idx += 7;
    frame[idx++] = 0x03; // Control: UI frame
    frame[idx++] = 0xF0; // PID: no layer 3
    return idx;
}

static void APRS_TxFrame(uint8_t *frame, uint16_t idx)
{
    const uint16_t fcs = AX25_CalculateFCS(frame, idx);
    frame[idx++] = (uint8_t)(fcs & 0xFF);
    frame[idx++] = (uint8_t)((fcs >> 8) & 0xFF);
    APRS_TransmitBell202(frame, idx);
}

static uint16_t APRS_PosDigits(uint8_t *out, uint32_t v, uint8_t deg3)
{
    // micro-degrees -> "DDMM.mm" / "DDDMM.mm"
    const uint32_t deg    = v / 1000000u;
    const uint32_t min100 = ((v % 1000000u) * 6u) / 1000u;
    uint16_t n = 0;
    if (deg3)
        out[n++] = (uint8_t)('0' + (deg / 100u) % 10u);
    out[n++] = (uint8_t)('0' + (deg / 10u) % 10u);
    out[n++] = (uint8_t)('0' + deg % 10u);
    out[n++] = (uint8_t)('0' + (min100 / 1000u) % 10u);
    out[n++] = (uint8_t)('0' + (min100 / 100u) % 10u);
    out[n++] = '.';
    out[n++] = (uint8_t)('0' + (min100 / 10u) % 10u);
    out[n++] = (uint8_t)('0' + min100 % 10u);
    return n;
}

static void APRS_TransmitBeacon(void)
{
    // Position and comment come from the menu settings (Loc / Cmnt).
    uint8_t frame[7 * 4 + 2 + 21 + 31 + 2];
    uint16_t idx = APRS_BuildHeader(frame);
    int32_t v;
    char hemi;

    frame[idx++] = '!';
    v = gEeprom.APRS_LATITUDE;
    hemi = 'N';
    if (v < 0) { v = -v; hemi = 'S'; }
    idx += APRS_PosDigits(&frame[idx], (uint32_t)v, 0);
    frame[idx++] = (uint8_t)hemi;
    frame[idx++] = '/';
    v = gEeprom.APRS_LONGITUDE;
    hemi = 'E';
    if (v < 0) { v = -v; hemi = 'W'; }
    idx += APRS_PosDigits(&frame[idx], (uint32_t)v, 1);
    frame[idx++] = (uint8_t)hemi;
    frame[idx++] = '>';
    for (uint8_t i = 0; i < 31 && gEeprom.APRS_COMMENT[i]; i++)
        frame[idx++] = (uint8_t)gEeprom.APRS_COMMENT[i];

    APRS_TxFrame(frame, idx);
}

// ---------------------------------------------------------------------------
// APRS RX (listen mode, enabled by the "APRS" menu item).
//
// The FSK engine's sync detector is pointed at the NRZI encoding of a run of
// AX.25 0x7E flags: seven bits of one level then one of the other, i.e. the
// repeating pattern 0xFE (or its complement - the engine detects both
// polarities as SyncP/SyncN). After sync the engine streams raw line bits
// into the FIFO; software NRZI-decodes, hunts flags, destuffs and checks the
// FCS. A valid frame is shown on the main screen's centre line.
// ---------------------------------------------------------------------------

#define APRS_RX_CAPTURE_BYTES 240u  // 1.6 s of raw bits: rest of preamble + frame + flag

char    gAPRS_RxDisplay[44];
uint8_t gAPRS_RxDisplayTimer;       // 500 ms ticks
bool    gAPRS_RxSticky;             // direct message: stays until dismissed

static bool     gRxArmed;
static bool     gRxCapturing;
static uint8_t  gRxStuckTicks;
static uint16_t gRxCount;
static uint8_t  gRxFrame[80];

#define APRS_RX_IRQ_MASK (BK4819_REG_02_FSK_RX_FINISHED | \
                          BK4819_REG_02_FSK_FIFO_ALMOST_FULL | \
                          BK4819_REG_02_FSK_RX_SYNC)

static void APRS_RxArm(void)
{
    BK4819_WriteRegister(BK4819_REG_70, (1u << 15) | (1u << 7) | (96u << 0));
    BK4819_WriteRegister(BK4819_REG_71, 22714);   // 2200 Hz
    BK4819_WriteRegister(BK4819_REG_72, 12389);   // 1200 Hz / bit clock

    BK4819_WriteRegister(BK4819_REG_58,
        (1u << 13) |   // TX mode (unused while receiving)
        (7u << 10) |   // FSK RX mode: FFSK 1200/1800
        (3u <<  8) |   // FSK RX gain
        (3u <<  6) |   // FSK enable
        (1u <<  1) |   // FSK RX bandwidth FFSK 1200/1800
        (1u <<  0));   // FSK enable

    BK4819_WriteRegister(BK4819_REG_5A, 0xFEFE);  // NRZI'd flag-run pattern
    BK4819_WriteRegister(BK4819_REG_5C, 0xAA30);  // engine CRC off
    BK4819_WriteRegister(BK4819_REG_5D, (uint16_t)((APRS_RX_CAPTURE_BYTES - 1u) << 8));
    BK4819_WriteRegister(BK4819_REG_5E, (64u << 3) | (4u << 0));  // RX FIFO almost-full: 4 words

    const uint16_t mask = BK4819_ReadRegister(BK4819_REG_3F);
    BK4819_WriteRegister(BK4819_REG_3F, mask | APRS_RX_IRQ_MASK);

    BK4819_WriteRegister(BK4819_REG_59, 0x4000);  // clear RX FIFO (2-byte sync)
    BK4819_WriteRegister(BK4819_REG_59, 0x1000);  // FSK RX enable

    gRxCount      = 0;
    gRxCapturing  = false;
    gRxStuckTicks = 0;
    gRxArmed      = true;
}

static void APRS_RxDrainFifo(uint8_t words)
{
    while (words-- > 0) {
        const uint16_t w = BK4819_ReadRegister(BK4819_REG_5F);
        if (gRxCount < HDLC_BUF_SIZE) gHdlcBuf[gRxCount++] = (uint8_t)w;
        if (gRxCount < HDLC_BUF_SIZE) gHdlcBuf[gRxCount++] = (uint8_t)(w >> 8);
    }
}

// EXIT on the main screen clears a sticky message; returns true if consumed
bool APRS_DismissMessage(void)
{
    if (!gAPRS_RxSticky)
        return false;
    gAPRS_RxSticky = false;
    gAPRS_RxDisplay[0] = 0;
    gAPRS_RxDisplayTimer = 0;
    gUpdateDisplay = true;
    return true;
}

void APRS_StopListening(void)
{
    if (!gRxArmed)
        return;
    BK4819_WriteRegister(BK4819_REG_59, 0x0000);
    BK4819_WriteRegister(BK4819_REG_58, 0x0000);
    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    gRxArmed     = false;
    gRxCapturing = false;
}

// ---- APRS position parsers (uncompressed / Mic-E / base-91 compressed) ----
// Positions are held as micro-degrees (degrees * 1000000), south/west negative.
// Mic-E and compressed decoding follow the APRS 1.01 spec, cross-checked
// against F4JTV/aprs_decoder.

static uint8_t MIN100_TO_MICRO(uint32_t deg, uint32_t min100, int32_t *out)
{
    // deg + (min100 / 100) minutes -> micro-degrees. 1' = 1000000/60 udeg.
    if (deg > 180u || min100 >= 6000u)
        return 0;
    *out = (int32_t)(deg * 1000000u + (min100 * 500u) / 3u);
    return 1;
}

static uint8_t APRS_ParseUncompressed(const uint8_t *p, uint16_t len, int32_t *lat, int32_t *lon)
{
    // "ddmm.hhN/dddmm.hhE" - position-ambiguity spaces count as zeros
    if (len < 19)
        return 0;
    const uint8_t *q = p;
    uint32_t latd, latm;
    uint32_t lond, lonm;
    #define DIGIT(c) (((c) == ' ') ? 0u : (uint32_t)((c) - '0'))
    #define ISDIG(c) (((c) >= '0' && (c) <= '9') || (c) == ' ')
    if (!ISDIG(q[0]) || !ISDIG(q[1]) || !ISDIG(q[2]) || !ISDIG(q[3]) ||
        q[4] != '.' || !ISDIG(q[5]) || !ISDIG(q[6]))
        return 0;
    latd = DIGIT(q[0]) * 10u + DIGIT(q[1]);
    latm = DIGIT(q[2]) * 1000u + DIGIT(q[3]) * 100u + DIGIT(q[5]) * 10u + DIGIT(q[6]);
    if (!MIN100_TO_MICRO(latd, latm, lat))
        return 0;
    if (q[7] == 'S')
        *lat = -*lat;
    else if (q[7] != 'N')
        return 0;
    q += 9;  // skip hemisphere + symbol table char
    if (!ISDIG(q[0]) || !ISDIG(q[1]) || !ISDIG(q[2]) || !ISDIG(q[3]) ||
        !ISDIG(q[4]) || q[5] != '.' || !ISDIG(q[6]) || !ISDIG(q[7]))
        return 0;
    lond = DIGIT(q[0]) * 100u + DIGIT(q[1]) * 10u + DIGIT(q[2]);
    lonm = DIGIT(q[3]) * 1000u + DIGIT(q[4]) * 100u + DIGIT(q[6]) * 10u + DIGIT(q[7]);
    if (!MIN100_TO_MICRO(lond, lonm, lon))
        return 0;
    if (q[8] == 'W')
        *lon = -*lon;
    else if (q[8] != 'E')
        return 0;
    #undef DIGIT
    #undef ISDIG
    return 1;
}

static uint8_t APRS_ParseMicE(const uint8_t *frame, const uint8_t *info, uint16_t ilen, int32_t *lat, int32_t *lon)
{
    const uint8_t t = info[0];
    if (t != 0x60 && t != 0x27 && t != 0x1C && t != 0x1D)  // ` ' and old GPS types
        return 0;
    if (ilen < 9)
        return 0;

    // latitude digits + flags live in the AX.25 destination address
    uint8_t  dig[6], bit[6];
    for (uint8_t i = 0; i < 6; i++) {
        const char c = (char)(frame[i] >> 1);
        if (c >= '0' && c <= '9')      { dig[i] = (uint8_t)(c - '0'); bit[i] = 0; }
        else if (c >= 'A' && c <= 'J') { dig[i] = (uint8_t)(c - 'A'); bit[i] = 1; }
        else if (c >= 'P' && c <= 'Y') { dig[i] = (uint8_t)(c - 'P'); bit[i] = 1; }
        else if (c == 'L')             { dig[i] = 0; bit[i] = 0; }  // ambiguity space
        else if (c == 'K' || c == 'Z') { dig[i] = 0; bit[i] = 1; }
        else return 0;
    }
    const uint32_t latd = (uint32_t)dig[0] * 10u + dig[1];
    const uint32_t latm = (uint32_t)dig[2] * 1000u + (uint32_t)dig[3] * 100u
                        + (uint32_t)dig[4] * 10u + dig[5];
    if (latd > 90u || !MIN100_TO_MICRO(latd, latm, lat))
        return 0;
    if (!bit[3])  // 1 = north
        *lat = -*lat;

    // longitude from info bytes 1..3
    int32_t d = (int32_t)info[1] - 28;
    int32_t m = (int32_t)info[2] - 28;
    int32_t h = (int32_t)info[3] - 28;
    if (d < 0 || m < 0 || h < 0 || h > 99)
        return 0;
    if (bit[4])
        d += 100;
    if (d >= 180 && d <= 189) d -= 80;
    else if (d >= 190 && d <= 199) d -= 190;
    if (m >= 60)
        m -= 60;
    if (d > 179 || m > 59)
        return 0;
    if (!MIN100_TO_MICRO((uint32_t)d, (uint32_t)(m * 100 + h), lon))
        return 0;
    if (bit[5])  // 1 = west
        *lon = -*lon;
    return 1;
}

static uint8_t APRS_ParseCompressed(const uint8_t *p, uint16_t len, int32_t *lat, int32_t *lon)
{
    // symbol-table char + 4 base-91 lat chars + 4 base-91 lon chars
    if (len < 10)
        return 0;
    for (uint8_t i = 1; i <= 8; i++)
        if (p[i] < '!' || p[i] > '{')
            return 0;
    uint32_t y = 0, x = 0;
    for (uint8_t i = 1; i <= 4; i++)
        y = y * 91u + (uint32_t)(p[i] - 33);
    for (uint8_t i = 5; i <= 8; i++)
        x = x * 91u + (uint32_t)(p[i] - 33);
    // lat = 90 - y/380926, lon = -180 + x/190463 (fraction via ~21/8 and ~21/4)
    {
        const uint32_t dd = y / 380926u, rr = y % 380926u;
        if (dd > 180u) return 0;
        *lat = 90000000 - (int32_t)(dd * 1000000u + (rr * 21u) / 8u);
    }
    {
        const uint32_t dd = x / 190463u, rr = x % 190463u;
        if (dd > 360u) return 0;
        *lon = -180000000 + (int32_t)(dd * 1000000u + (rr * 21u) / 4u);
    }
    return 1;
}

static char *APRS_FmtCoord(char *p, int32_t micro, char pos, char neg)
{
    char hemi = pos;
    uint32_t v = (uint32_t)micro;
    if (micro < 0) {
        v = (uint32_t)-micro;
        hemi = neg;
    }
    const uint32_t deg  = v / 1000000u;
    const uint32_t frac = (v % 1000000u) / 10000u;  // 2 decimals
    if (deg >= 100u)
        *p++ = (char)('0' + (deg / 100u) % 10u);
    *p++ = (char)('0' + (deg / 10u) % 10u);
    *p++ = (char)('0' + deg % 10u);
    *p++ = '.';
    *p++ = (char)('0' + (frac / 10u) % 10u);
    *p++ = (char)('0' + frac % 10u);
    *p++ = hemi;
    return p;
}

// Push a decoded packet out the UART as a plain "APRS:<text>\r\n" line so a PC
// can monitor traffic (see utils/aprs_pc.py). No-op if UART is disabled.
static void APRS_EmitMonitor(void)
{
#ifdef ENABLE_UART
    UART_Send("APRS:", 5);
    UART_Send(gAPRS_RxDisplay, (uint32_t)strlen(gAPRS_RxDisplay));
    UART_Send("\r\n", 2);
#endif
}

// "41.15N\n27.84E" for the Loc menu display (two lines)
void APRS_FormatLatLon(char *out)
{
    char *p = APRS_FmtCoord(out, gEeprom.APRS_LATITUDE, 'N', 'S');
    *p++ = '\n';
    p = APRS_FmtCoord(p, gEeprom.APRS_LONGITUDE, 'E', 'W');
    *p = 0;
}

static void APRS_ShowFrame(const uint8_t *frame, uint16_t len)
{
    // find the last address field (bit0 of the SSID byte set)
    uint16_t a = 6;
    while (a + 7 < len && (frame[a] & 1u) == 0)
        a += 7;
    const uint16_t info = a + 3;  // past last SSID byte + control + PID
    if (a < 13 || info >= len - 2u)
        return;

    const uint8_t *ip   = &frame[info];
    const uint16_t ilen = (uint16_t)(len - 2u - info);
    int32_t lat = 0, lon = 0;
    uint8_t have = 0;

    // APRS message ":ADDRESSEE:text" addressed to my callsign-SSID?
    uint8_t tome = 0;
    if (ilen >= 11 && ip[0] == ':' && ip[10] == ':') {
        char me[10];
        uint8_t k = 0;
        for (uint8_t i = 0; i < 6 && gEeprom.APRS_CALLSIGN[i] > ' '; i++)
            me[k++] = gEeprom.APRS_CALLSIGN[i];
        const uint8_t myssid = gEeprom.APRS_SSID & 0x0F;
        if (myssid > 0) {
            me[k++] = '-';
            if (myssid >= 10)
                me[k++] = '1';
            me[k++] = (char)('0' + (myssid % 10));
        }
        const uint8_t used = k;
        while (k < 9)
            me[k++] = ' ';
        tome = (used > 0 && memcmp(&ip[1], me, 9) == 0);
    }

    // an undismissed direct message may only be replaced by a newer one
    if (gAPRS_RxSticky && !tome)
        return;

    // source callsign is the second address field
    uint8_t o = 0;
    for (uint8_t i = 7; i < 13; i++) {
        const char c = (char)(frame[i] >> 1);
        if (c > ' ')
            gAPRS_RxDisplay[o++] = c;
    }
    const uint8_t ssid = (frame[13] >> 1) & 0x0F;
    if (ssid > 0) {
        gAPRS_RxDisplay[o++] = '-';
        if (ssid >= 10)
            gAPRS_RxDisplay[o++] = '1';
        gAPRS_RxDisplay[o++] = (char)('0' + (ssid % 10));
    }

    if (tome) {
        // keep on screen until dismissed with EXIT
        gAPRS_RxDisplay[o++] = '>';
        for (uint16_t i = 11; i < ilen && o < sizeof(gAPRS_RxDisplay) - 1; i++) {
            const char c = (char)ip[i];
            gAPRS_RxDisplay[o++] = (c >= 32 && c < 127) ? c : '.';
        }
        gAPRS_RxDisplay[o] = 0;
        gAPRS_RxSticky = true;
        gUpdateDisplay = true;
        APRS_EmitMonitor();
        AUDIO_PlayBeep(BEEP_880HZ_60MS_DOUBLE_BEEP);
        return;
    }

    if (ilen >= 2) {
        const uint8_t t = ip[0];
        if (t == '!' || t == '=' || t == '@' || t == '/') {
            const uint8_t *p2 = ip + 1;
            uint16_t l2 = (uint16_t)(ilen - 1);
            if ((t == '@' || t == '/') && l2 > 7) {  // skip timestamp
                p2 += 7;
                l2 -= 7;
            }
            if (l2 >= 1 && ((p2[0] >= '0' && p2[0] <= '9') || p2[0] == ' '))
                have = APRS_ParseUncompressed(p2, l2, &lat, &lon);
            else
                have = APRS_ParseCompressed(p2, l2, &lat, &lon);
        } else {
            have = APRS_ParseMicE(frame, ip, ilen, &lat, &lon);
        }
    }

    if (have) {
        char *p = &gAPRS_RxDisplay[o];
        *p++ = ' ';
        p = APRS_FmtCoord(p, lat, 'N', 'S');
        *p++ = ' ';
        p = APRS_FmtCoord(p, lon, 'E', 'W');
        *p = 0;
    } else {
        gAPRS_RxDisplay[o++] = ':';
        for (uint16_t i = info; i < len - 2u && o < sizeof(gAPRS_RxDisplay) - 1; i++) {
            const char c = (char)frame[i];
            gAPRS_RxDisplay[o++] = (c >= 32 && c < 127) ? c : '.';
        }
        gAPRS_RxDisplay[o] = 0;
    }

    gAPRS_RxDisplayTimer = 60;  // 30 s
    gUpdateDisplay = true;
    APRS_EmitMonitor();
    AUDIO_PlayBeep(BEEP_1KHZ_60MS_OPTIONAL);
}

static bool APRS_DecodeCapture(void)
{
    uint8_t  prev = 0, ones = 0;
    uint16_t nbits = 0xFFFF;  // not synced until the first flag

    memset(gRxFrame, 0, sizeof(gRxFrame));

    for (uint32_t i = 0; i < (uint32_t)gRxCount * 8u; i++) {
        const uint8_t level = (gHdlcBuf[i >> 3] >> (7u - (i & 7u))) & 1u;
        const uint8_t bit   = (level == prev) ? 1u : 0u;  // NRZI: no change = 1
        prev = level;

        if (bit) {
            if (ones < 7)
                ones++;
            if (ones >= 7) {          // 7+ ones: invalid, wait for the next flag
                nbits = 0xFFFF;
                continue;
            }
            if (nbits != 0xFFFF) {    // collect (closing flag bits stripped later)
                if (nbits < sizeof(gRxFrame) * 8u)
                    gRxFrame[nbits >> 3] |= (uint8_t)(1u << (nbits & 7u));  // LSB first
                nbits++;
            }
            continue;
        }

        // bit == 0
        if (ones == 5) {              // stuffed zero - drop it
            ones = 0;
            continue;
        }
        if (ones == 6) {              // flag 01111110 completed
            if (nbits != 0xFFFF && nbits >= 7u) {
                const uint16_t fb = nbits - 7u;  // strip the flag's leading 0111111
                if ((fb & 7u) == 0 && fb >= 17u * 8u && (fb >> 3) <= sizeof(gRxFrame)) {
                    const uint16_t len = fb >> 3;
                    const uint16_t fcs = AX25_CalculateFCS(gRxFrame, len - 2u);
                    if (fcs == (uint16_t)(gRxFrame[len - 2] | (gRxFrame[len - 1] << 8))) {
                        APRS_ShowFrame(gRxFrame, len);
                        return true;
                    }
                }
            }
            memset(gRxFrame, 0, sizeof(gRxFrame));
            nbits = 0;
            ones  = 0;
            continue;
        }
        if (nbits != 0xFFFF) {
            if (nbits < sizeof(gRxFrame) * 8u)
                nbits++;              // 0 bit: buffer already zeroed
            else
                nbits = 0xFFFF;       // frame too long - resync
        }
        ones = 0;
    }
    return false;
}

void APRS_HandleRxInterrupts(uint16_t interrupt_bits)
{
    if (!gRxArmed)
        return;

    if (interrupt_bits & BK4819_REG_02_FSK_RX_SYNC) {
        gRxCapturing  = true;
        gRxStuckTicks = 0;
        gRxCount      = 0;
    }

    if (interrupt_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL) {
        gRxStuckTicks = 0;
        APRS_RxDrainFifo(4);
    }

    if (interrupt_bits & BK4819_REG_02_FSK_RX_FINISHED) {
        APRS_RxDrainFifo(8);  // drain whatever is left
        APRS_DecodeCapture();
        APRS_RxArm();  // clear FIFO and wait for the next packet
    }
}

// Convert callsign to AX.25 address format
static void APRS_CallsignToAX25(const char *callsign, uint8_t ssid, uint8_t *output)
{
    uint8_t i;

    // Copy callsign and pad with spaces (shifted left by 1)
    for (i = 0; i < 6; i++) {
        if (callsign[i] != 0) {
            output[i] = callsign[i] << 1;
        } else {
            output[i] = ' ' << 1;
        }
    }

    // Add SSID byte (last address, so no "has more" bit)
    output[6] = ((ssid & 0x0F) << 1) | 0x60;
}

// Build position report with integer coordinates only
// lat_int: latitude in millionths of degrees (e.g., 40712800 = 40.7128°)
// lon_int: longitude in millionths of degrees (e.g., -7400600 = -74.006°)
static uint16_t APRS_BuildPositionReport(int32_t lat_int, int32_t lon_int,
                                          const char *comment, uint8_t *output)
{
    uint16_t idx = 0;
    uint8_t lat_deg, lon_deg;
    uint16_t lat_min, lon_min;
    char lat_hem, lon_hem;
    uint32_t abs_lat, abs_lon;

    // Determine hemisphere and get absolute value
    if (lat_int < 0) {
        lat_hem = 'S';
        abs_lat = -lat_int;
    } else {
        lat_hem = 'N';
        abs_lat = lat_int;
    }

    if (lon_int < 0) {
        lon_hem = 'W';
        abs_lon = -lon_int;
    } else {
        lon_hem = 'E';
        abs_lon = lon_int;
    }

    // Extract degrees and decimal minutes using integer arithmetic
    // Degrees: integer part
    lat_deg = abs_lat / 1000000;
    lon_deg = abs_lon / 1000000;

    // Minutes: (fractional part) * 60 / 10000 = remainder * 60 / 1000000
    // This gives minutes in hundredths (2 decimal places)
    lat_min = ((abs_lat % 1000000) * 60) / 10000;
    lon_min = ((abs_lon % 1000000) * 60) / 10000;

    // Build position report: !DDMM.mmN/DDDMM.mmE#comment
    output[idx++] = '!';  // Position without timestamp

    // Latitude: 2 digits degrees
    output[idx++] = '0' + (lat_deg / 10);
    output[idx++] = '0' + (lat_deg % 10);

    // Latitude: 2 digits minutes integer
    output[idx++] = '0' + (lat_min / 1000);
    output[idx++] = '0' + ((lat_min / 100) % 10);

    output[idx++] = '.';

    // Latitude: 2 digits minutes fractional
    output[idx++] = '0' + ((lat_min / 10) % 10);
    output[idx++] = '0' + (lat_min % 10);

    output[idx++] = lat_hem;

    // Symbol: /j (primary table, house)
    output[idx++] = '/';
    output[idx++] = 'j';

    // Longitude: 3 digits degrees
    output[idx++] = '0' + (lon_deg / 100);
    output[idx++] = '0' + ((lon_deg / 10) % 10);
    output[idx++] = '0' + (lon_deg % 10);

    // Longitude: 2 digits minutes integer
    output[idx++] = '0' + (lon_min / 1000);
    output[idx++] = '0' + ((lon_min / 100) % 10);

    output[idx++] = '.';

    // Longitude: 2 digits minutes fractional
    output[idx++] = '0' + ((lon_min / 10) % 10);
    output[idx++] = '0' + (lon_min % 10);

    output[idx++] = lon_hem;

    // Comment (truncated if needed)
    if (comment != NULL) {
        uint8_t max_len = APRS_MAX_PACKET_SIZE - idx - 5;  // Leave room for AX.25 overhead
        for (uint8_t i = 0; i < max_len && comment[i] != 0; i++) {
            output[idx++] = comment[i];
        }
    }

    return idx;
}

// Build complete AX.25 frame
__attribute__((unused)) static uint16_t APRS_BuildPacket(const char *callsign, uint8_t ssid,
                                   int32_t lat_int, int32_t lon_int,
                                   const char *comment, uint8_t *output)
{
    uint16_t idx = 0;
    uint16_t info_len;
    uint16_t crc;

    // Start with flag
    output[idx++] = 0x7E;

    // Destination address (APRS, SSID 0)
    APRS_CallsignToAX25("APRS", 0, &output[idx]);
    idx += 7;

    // Source address (user's callsign and SSID)
    APRS_CallsignToAX25(callsign, ssid, &output[idx]);
    idx += 7;

    // Control field (UI frame)
    output[idx++] = 0x03;

    // PID field (no layer 3)
    output[idx++] = 0xF0;

    // Build info field (position report)
    info_len = APRS_BuildPositionReport(lat_int, lon_int, comment, &output[idx]);
    idx += info_len;

    // Calculate FCS (CRC) over everything from after first flag to end of info
    crc = APRS_CalculateCRC(&output[1], idx - 1);

    // Append FCS
    output[idx++] = crc & 0xFF;
    output[idx++] = (crc >> 8) & 0xFF;

    // End flag
    output[idx++] = 0x7E;

    return idx;
}

// APRS state
typedef enum {
    APRS_STATE_IDLE,
    APRS_STATE_TRANSMITTING,
    APRS_STATE_WAITING
} aprs_state_t;

static aprs_state_t gAPRSState = APRS_STATE_IDLE;

// Initialize APRS system
void APRS_Init(void)
{
    // Validate callsign
    if (gEeprom.APRS_CALLSIGN[0] == 0 || gEeprom.APRS_CALLSIGN[0] == ' ') {
        // No valid callsign, set default
        gEeprom.APRS_CALLSIGN[0] = 'N';
        gEeprom.APRS_CALLSIGN[1] = '0';
        gEeprom.APRS_CALLSIGN[2] = 'C';
        gEeprom.APRS_CALLSIGN[3] = 'A';
        gEeprom.APRS_CALLSIGN[4] = 'L';
        gEeprom.APRS_CALLSIGN[5] = 'L';
        gEeprom.APRS_CALLSIGN[6] = 0;
    }

    gAPRSState = APRS_STATE_IDLE;
}

// Check if APRS is currently transmitting
bool APRS_IsTransmitting(void)
{
    return (gAPRSState == APRS_STATE_TRANSMITTING);
}

// Manual transmission trigger (menu TX item). Automatic beaconing is
// intentionally not implemented - transmissions happen only on user action.
void APRS_TransmitNow(void)
{
    if (gAPRSState != APRS_STATE_IDLE)
        return;

    gAPRSState = APRS_STATE_TRANSMITTING;

    APRS_StopListening();  // TX reuses the modem and the capture buffer

    APRS_TransmitBeacon();

    gAPRSState = APRS_STATE_WAITING;  // APRS_Task re-arms RX afterwards
}

// Send the composed text message to gAPRS_MsgTo (menu "Send" item)
void APRS_SendMessage(void)
{
    if (gAPRSState != APRS_STATE_IDLE || gAPRS_MsgTo[0] == 0 || gAPRS_MsgText[0] == 0)
        return;

    gAPRSState = APRS_STATE_TRANSMITTING;
    APRS_StopListening();

    uint8_t frame[7 * 4 + 2 + 1 + 9 + 1 + 30 + 2];
    uint16_t idx = APRS_BuildHeader(frame);
    frame[idx++] = ':';
    {
        uint8_t end = 0;
        for (uint8_t i = 0; i < 9; i++) {
            if (!end && gAPRS_MsgTo[i] == 0)
                end = 1;
            frame[idx++] = end ? ' ' : (uint8_t)gAPRS_MsgTo[i];
        }
    }
    frame[idx++] = ':';
    for (uint8_t i = 0; i < 30 && gAPRS_MsgText[i]; i++)
        frame[idx++] = (uint8_t)gAPRS_MsgText[i];
    APRS_TxFrame(frame, idx);

    gAPRSState = APRS_STATE_WAITING;
}

// Main APRS task (called from the 500ms app loop while APRS is ON)
void APRS_Task(void)
{
    // Expire the RX packet display (sticky messages wait for EXIT instead)
    if (!gAPRS_RxSticky &&
        gAPRS_RxDisplayTimer > 0 && --gAPRS_RxDisplayTimer == 0) {
        gAPRS_RxDisplay[0] = 0;
        gUpdateDisplay = true;
    }

    // Keep the receiver armed while idle. The engine's byte counter stalls
    // when the carrier drops (bit clock lock is lost), so RX_FINISHED often
    // never fires for real packets: once a capture has been idle for ~1.5 s,
    // decode whatever was collected and re-arm.
    if (gAPRSState == APRS_STATE_IDLE && gCurrentFunction != FUNCTION_TRANSMIT) {
        if (!gRxArmed) {
            APRS_RxArm();
        } else if (gRxCapturing && ++gRxStuckTicks > 2) {
            APRS_RxDrainFifo(8);  // pull the frame tail out of the FIFO
            APRS_DecodeCapture();
            APRS_RxArm();
        }
    }

    if (gRxArmed) {
        // Other code paths (RADIO_SetupRegisters etc.) rewrite the interrupt
        // mask; make sure the FSK RX bits stay enabled.
        const uint16_t mask = BK4819_ReadRegister(BK4819_REG_3F);
        if ((mask & APRS_RX_IRQ_MASK) != APRS_RX_IRQ_MASK)
            BK4819_WriteRegister(BK4819_REG_3F, mask | APRS_RX_IRQ_MASK);
    }

    // Handle waiting state (cooldown after transmission)
    if (gAPRSState == APRS_STATE_WAITING) {
        // Wait a bit before allowing next transmission (1 second)
        static uint32_t last_tx = 0;
        if (last_tx == 0) {
            last_tx = gGlobalSysTickCounter;
        } else if (gGlobalSysTickCounter - last_tx > 100) {  // 1 second
            gAPRSState = APRS_STATE_IDLE;
            last_tx = 0;
        }
    }
}
