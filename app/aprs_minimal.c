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
#include "../functions.h"
#include "../radio.h"
#include "../settings.h"
#include "../driver/bk4819.h"
#include "../driver/system.h"

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

#define HDLC_BUF_SIZE    128u  // encoded bitstream, bytes
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

static void APRS_TransmitHardcoded_TA1JS(void)
{
    // Hardcoded APRS/AX.25 UI frame: TA1JS>APRS,WIDE1-1,WIDE2-1 at 40.993293N 27.599608E.
    static const uint8_t INFO[] = "!4059.60N/02735.98E>UV-K5 APRS";

    uint8_t frame[7 * 4 + 2 + (sizeof(INFO) - 1) + 2];
    uint16_t idx = 0;

    AX25_EncodeAddress("APRS",  0, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("TA1JS", 0, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("WIDE1", 1, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("WIDE2", 1, true,  &frame[idx]); idx += 7;
    frame[idx++] = 0x03; // Control: UI frame
    frame[idx++] = 0xF0; // PID: no layer 3
    memcpy(&frame[idx], INFO, sizeof(INFO) - 1);
    idx += (uint16_t)(sizeof(INFO) - 1);

    const uint16_t fcs = AX25_CalculateFCS(frame, idx);
    frame[idx++] = (uint8_t)(fcs & 0xFF);
    frame[idx++] = (uint8_t)((fcs >> 8) & 0xFF);

    APRS_TransmitBell202(frame, idx);
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

    // Hardcoded MVP: transmit a minimal, decodable APRS/AX.25 frame from TA1JS.
    APRS_TransmitHardcoded_TA1JS();

    gAPRSState = APRS_STATE_WAITING;
}

// Main APRS task (called from app loop)
void APRS_Task(void)
{
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
