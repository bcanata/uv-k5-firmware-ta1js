// Host-side verification of the firmware's HDLC/AX.25 bitstream generation.
// Encodes exactly like app/aprs_minimal.c, then decodes like a real TNC
// (NRZI decode -> flag hunt -> bit destuff -> LSB-first bytes -> FCS check).
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// ---- copied from firmware ----
#define HDLC_BUF_SIZE    128u
#define HDLC_LEAD_FLAGS  32u
#define HDLC_TAIL_FLAGS  3u

typedef struct {
    uint8_t *buf;
    uint16_t bits;
    uint8_t  ones;
    uint8_t  level;
} hdlc_writer_t;

static void HDLC_PutBit(hdlc_writer_t *w, bool bit)
{
    if (w->bits >= HDLC_BUF_SIZE * 8u) return;
    if (!bit) w->level ^= 1;
    if (w->level) w->buf[w->bits >> 3] |= 0x80u >> (w->bits & 7u);
    w->bits++;
}

static void HDLC_PutByte(hdlc_writer_t *w, uint8_t b, bool stuff)
{
    for (uint8_t i = 0; i < 8; i++) {
        const bool bit = (b >> i) & 1u;
        HDLC_PutBit(w, bit);
        if (stuff && bit) {
            if (++w->ones == 5) { HDLC_PutBit(w, false); w->ones = 0; }
        } else {
            w->ones = 0;
        }
    }
}

static uint16_t APRS_CalculateCRC(const uint8_t *data, uint16_t length)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (uint8_t j = 0; j < 8; j++)
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}
static uint16_t AX25_CalculateFCS(const uint8_t *d, uint16_t l) { return (uint16_t)~APRS_CalculateCRC(d, l); }

static void AX25_EncodeAddress(const char *callsign, uint8_t ssid, bool last, uint8_t out7[7])
{
    bool end = false;
    for (uint8_t i = 0; i < 6; i++) {
        char c = ' ';
        if (!end) {
            if (callsign[i] == 0) end = true;
            else c = callsign[i];
        }
        out7[i] = (uint8_t)(c << 1);
    }
    out7[6] = (uint8_t)(0x60 | ((ssid & 0x0F) << 1) | (last ? 0x01 : 0x00));
}

// ---- decoder (independent implementation, TNC-style) ----
int main(void)
{
    // Build the frame exactly like APRS_TransmitHardcoded_TA1JS
    static const uint8_t INFO[] = "!4059.60N/02735.98E>UV-K5 APRS";
    uint8_t frame[7 * 4 + 2 + (sizeof(INFO) - 1) + 2];
    uint16_t idx = 0;
    AX25_EncodeAddress("APRS",  0, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("TA1JS", 0, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("WIDE1", 1, false, &frame[idx]); idx += 7;
    AX25_EncodeAddress("WIDE2", 1, true,  &frame[idx]); idx += 7;
    frame[idx++] = 0x03;
    frame[idx++] = 0xF0;
    memcpy(&frame[idx], INFO, sizeof(INFO) - 1);
    idx += sizeof(INFO) - 1;
    uint16_t fcs = AX25_CalculateFCS(frame, idx);
    frame[idx++] = fcs & 0xFF;
    frame[idx++] = fcs >> 8;

    // Encode to on-air bitstream
    static uint8_t hdlcbuf[HDLC_BUF_SIZE];
    hdlc_writer_t w = { hdlcbuf, 0, 0, 1 };
    for (int i = 0; i < (int)HDLC_LEAD_FLAGS; i++) HDLC_PutByte(&w, 0x7E, false);
    for (int i = 0; i < idx; i++)                  HDLC_PutByte(&w, frame[i], true);
    for (int i = 0; i < (int)HDLC_TAIL_FLAGS; i++) HDLC_PutByte(&w, 0x7E, false);
    printf("frame=%u bytes, stream=%u bits (%u bytes)\n", idx, w.bits, (w.bits + 7) / 8);

    // 1) unpack MSB-first to line levels; 2) NRZI decode (transition=0)
    static uint8_t nrz[HDLC_BUF_SIZE * 8];
    uint8_t prev = 1;  // arbitrary receiver start state
    for (uint32_t i = 0; i < w.bits; i++) {
        uint8_t level = (hdlcbuf[i >> 3] >> (7 - (i & 7))) & 1;
        nrz[i] = (level == prev) ? 1 : 0;
        prev = level;
    }

    // 3) flag hunt + destuff, collecting frames between flags
    uint8_t rxframe[128];
    int best_len = 0;
    uint32_t i = 0;
    while (i + 8 <= w.bits) {
        // match flag 0x7E = bits 0,1,1,1,1,1,1,0 (LSB first)
        static const uint8_t flagbits[8] = {0,1,1,1,1,1,1,0};
        bool is_flag = true;
        for (int k = 0; k < 8; k++) if (nrz[i + k] != flagbits[k]) { is_flag = false; break; }
        if (!is_flag) { i++; continue; }
        // found flag; skip consecutive flags
        uint32_t j = i + 8;
        while (j + 8 <= w.bits) {
            bool again = true;
            for (int k = 0; k < 8; k++) if (nrz[j + k] != flagbits[k]) { again = false; break; }
            if (!again) break;
            j += 8;
        }
        // collect destuffed bits until next flag
        uint8_t bitbuf[2048]; int nb = 0; int ones = 0; bool closed = false;
        uint32_t p = j;
        while (p < w.bits) {
            // check for closing flag at p
            if (p + 8 <= w.bits) {
                bool f = true;
                for (int k = 0; k < 8; k++) if (nrz[p + k] != flagbits[k]) { f = false; break; }
                if (f) { closed = true; break; }
            }
            uint8_t b = nrz[p++];
            if (ones == 5) { // stuffed zero expected
                if (b == 0) { ones = 0; continue; }   // drop stuffed bit
                else break;                            // 6 ones = abort/flag part
            }
            if (nb < 2048) bitbuf[nb++] = b;
            ones = b ? ones + 1 : 0;
        }
        if (closed && nb >= 136 && (nb % 8) == 0) {
            int len = nb / 8;
            for (int k = 0; k < len; k++) {
                uint8_t byte = 0;
                for (int m = 0; m < 8; m++) byte |= bitbuf[k * 8 + m] << m;  // LSB first
                rxframe[k] = byte;
            }
            best_len = len;
            break;
        }
        i = j;
    }

    if (!best_len) { printf("FAIL: no frame decoded\n"); return 1; }
    printf("decoded frame: %d bytes\n", best_len);

    // FCS check
    uint16_t want = AX25_CalculateFCS(rxframe, best_len - 2);
    uint16_t got  = rxframe[best_len - 2] | (rxframe[best_len - 1] << 8);
    printf("FCS: calc=%04X rx=%04X -> %s\n", want, got, want == got ? "OK" : "FAIL");

    // parse addresses
    char dst[8] = {0}, src[8] = {0}, via[8] = {0}, via2[8] = {0};
    for (int k = 0; k < 6; k++) { dst[k] = rxframe[k] >> 1; src[k] = rxframe[7 + k] >> 1; via[k] = rxframe[14 + k] >> 1; via2[k] = rxframe[21 + k] >> 1; }
    printf("dst=%.6s ssid=%d  src=%.6s ssid=%d  via=%.6s-%d(l%d)  via2=%.6s-%d(l%d)\n",
        dst, (rxframe[6] >> 1) & 0xF, src, (rxframe[13] >> 1) & 0xF,
        via, (rxframe[20] >> 1) & 0xF, rxframe[20] & 1,
        via2, (rxframe[27] >> 1) & 0xF, rxframe[27] & 1);
    printf("ctrl=%02X pid=%02X\n", rxframe[28], rxframe[29]);
    printf("info=\"%.*s\"\n", best_len - 2 - 30, &rxframe[30]);

    bool ok = (want == got) && rxframe[28] == 0x03 && rxframe[29] == 0xF0 &&
              !memcmp(dst, "APRS  ", 6) && !memcmp(src, "TA1JS ", 6) &&
              !memcmp(via, "WIDE1 ", 6) && !memcmp(via2, "WIDE2 ", 6) &&
              (rxframe[20] & 1) == 0 && (rxframe[27] & 1) == 1;
    printf(ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    return ok ? 0 : 1;
}
