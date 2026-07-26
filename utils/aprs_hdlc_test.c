// Host-side verification of the firmware's HDLC/AX.25 bitstream generation.
// Encodes exactly like app/aprs_minimal.c, then decodes like a real TNC
// (NRZI decode -> flag hunt -> bit destuff -> LSB-first bytes -> FCS check).
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
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

// ---- position parsers (verbatim copies from app/aprs_minimal.c) ----
static uint8_t MIN100_TO_MICRO(uint32_t deg, uint32_t min100, int32_t *out)
{
    if (deg > 180u || min100 >= 6000u)
        return 0;
    *out = (int32_t)(deg * 1000000u + (min100 * 500u) / 3u);
    return 1;
}

static uint8_t APRS_ParseUncompressed(const uint8_t *p, uint16_t len, int32_t *lat, int32_t *lon)
{
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
    q += 9;
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
    if (t != 0x60 && t != 0x27 && t != 0x1C && t != 0x1D)
        return 0;
    if (ilen < 9)
        return 0;
    uint8_t  dig[6], bit[6];
    for (uint8_t i = 0; i < 6; i++) {
        const char c = (char)(frame[i] >> 1);
        if (c >= '0' && c <= '9')      { dig[i] = (uint8_t)(c - '0'); bit[i] = 0; }
        else if (c >= 'A' && c <= 'J') { dig[i] = (uint8_t)(c - 'A'); bit[i] = 1; }
        else if (c >= 'P' && c <= 'Y') { dig[i] = (uint8_t)(c - 'P'); bit[i] = 1; }
        else if (c == 'L')             { dig[i] = 0; bit[i] = 0; }
        else if (c == 'K' || c == 'Z') { dig[i] = 0; bit[i] = 1; }
        else return 0;
    }
    const uint32_t latd = (uint32_t)dig[0] * 10u + dig[1];
    const uint32_t latm = (uint32_t)dig[2] * 1000u + (uint32_t)dig[3] * 100u
                        + (uint32_t)dig[4] * 10u + dig[5];
    if (latd > 90u || !MIN100_TO_MICRO(latd, latm, lat))
        return 0;
    if (!bit[3])
        *lat = -*lat;
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
    if (bit[5])
        *lon = -*lon;
    return 1;
}

static uint8_t APRS_ParseCompressed(const uint8_t *p, uint16_t len, int32_t *lat, int32_t *lon)
{
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

// ---- decoder (independent implementation, TNC-style) ----
// Mirror of the third-party unwrap + addressee match in APRS_ShowFrame.
// Takes the raw info field (bytes after the AX.25 addresses); on a "}"-prefixed
// gated packet it re-points payload/paylen at the encapsulated info and reports
// the true originator.  Returns 1 if the (unwrapped) payload is a message
// addressed to mycall-myssid.
static int TP_UnwrapAndMatch(const char *infostr, const char *mycall, int myssid,
                             const char **payload, int *paylen, char *srcout)
{
    const uint8_t *ip = (const uint8_t *)infostr;
    uint16_t ilen = (uint16_t)strlen(infostr);
    const char *tpsrc = 0;
    uint8_t     tpsrclen = 0;
    if (ilen >= 2 && ip[0] == '}') {
        uint16_t h = 1;
        while (h < ilen && ip[h] != '>')  h++;
        tpsrclen = (uint8_t)(h - 1);
        while (h < ilen && ip[h] != ':')  h++;
        if (tpsrclen > 0 && h + 1u < ilen && ip[h] == ':') {
            tpsrc = (const char *)&ip[1];
            ip    = &ip[h + 1u];
            ilen  = (uint16_t)(ilen - (h + 1u));
        }
    }
    srcout[0] = 0;
    if (tpsrc) {
        int n = tpsrclen > 9 ? 9 : tpsrclen;
        memcpy(srcout, tpsrc, n);
        srcout[n] = 0;
    }
    *payload = (const char *)ip;
    *paylen  = (int)ilen;

    int tome = 0;
    if (ilen >= 11 && ip[0] == ':' && ip[10] == ':') {
        char me[10];
        int k = 0;
        for (int i = 0; i < 6 && mycall[i] > ' '; i++) me[k++] = mycall[i];
        if (myssid > 0) { me[k++] = '-'; if (myssid >= 10) me[k++] = '1'; me[k++] = (char)('0' + (myssid % 10)); }
        int used = k;
        while (k < 9) me[k++] = ' ';
        tome = (used > 0 && memcmp(&ip[1], me, 9) == 0);
    }
    return tome;
}

// ---- fill-in digipeater (mirror of APRS_DigiConsider in app/aprs_minimal.c) ----
#define DIGI_SEEN 4u
#define DIGI_TTL  60u
static uint8_t  gDigiFrame[80];
static uint16_t gDigiLen;
static uint16_t gDigiSeen[DIGI_SEEN];
static uint8_t  gDigiAge[DIGI_SEEN];
static char     gMyCall[7] = "TA1JS";
static uint8_t  gMySsid    = 7;
static bool     gAPRS_DIGI = true;

static bool DigiDuplicate(const uint8_t *frame, uint16_t len, uint16_t info)
{
    const uint16_t key = (uint16_t)(APRS_CalculateCRC(&frame[7], 7) ^
                                    APRS_CalculateCRC(&frame[info], (uint16_t)(len - 2u - info)));
    uint8_t oldest = 0;
    for (uint8_t i = 0; i < DIGI_SEEN; i++) {
        if (gDigiAge[i] > 0 && gDigiSeen[i] == key) return true;
        if (gDigiAge[i] < gDigiAge[oldest]) oldest = i;
    }
    gDigiSeen[oldest] = key;
    gDigiAge[oldest]  = DIGI_TTL;
    return false;
}

static void DigiConsider(const uint8_t *frame, uint16_t len)
{
    if (!gAPRS_DIGI || gDigiLen != 0 || len < 23u) return;
    if (frame[13] & 1u) return;

    uint8_t me[7];
    AX25_EncodeAddress(gMyCall, gMySsid, false, me);
    if (memcmp(&frame[7], me, 6) == 0) return;

    uint16_t s = 6;
    while (s + 7u < len && (frame[s] & 1u) == 0) s += 7u;
    const uint16_t info = s + 3u;
    if (info >= len - 2u) return;

    for (uint16_t a = 20; a <= s; a += 7u) {
        if (frame[a] & 0x80u) { if (frame[a] & 1u) break; continue; }
        if (((frame[a] >> 1) & 0x0Fu) != 1u) return;
        static const char WIDE1[6] = { 'W','I','D','E','1',' ' };
        for (uint8_t k = 0; k < 6; k++)
            if ((char)(frame[a - 6u + k] >> 1) != WIDE1[k]) return;
        if (DigiDuplicate(frame, len, info)) return;
        memcpy(gDigiFrame, frame, len);
        memcpy(&gDigiFrame[a - 6u], me, 6);
        gDigiFrame[a] = (uint8_t)(0x80u | 0x60u | ((gMySsid & 0x0Fu) << 1) | (frame[a] & 1u));
        gDigiLen = len;
        return;
    }
}

// build "SRC>APOVK5,<via1>,<via2>:info" as an AX.25 frame (with a dummy FCS)
static uint16_t MkFrame(uint8_t *f, const char *src, uint8_t sssid,
                        const char *v1, uint8_t v1s, bool v1h,
                        const char *v2, uint8_t v2s, bool v2h, const char *info)
{
    uint16_t i = 0;
    AX25_EncodeAddress("APOVK5", 0, false, &f[i]); i += 7;
    AX25_EncodeAddress(src, sssid, false, &f[i]);  i += 7;
    if (v1) { AX25_EncodeAddress(v1, v1s, v2 == NULL, &f[i]); if (v1h) f[i+6] |= 0x80; i += 7; }
    if (v2) { AX25_EncodeAddress(v2, v2s, true,       &f[i]); if (v2h) f[i+6] |= 0x80; i += 7; }
    f[i++] = 0x03; f[i++] = 0xF0;
    const size_t n = strlen(info);
    memcpy(&f[i], info, n); i += (uint16_t)n;
    const uint16_t fcs = AX25_CalculateFCS(f, i);
    f[i++] = fcs & 0xFF; f[i++] = fcs >> 8;
    return i;
}

static int test_digi(void)
{
    uint8_t f[80];
    int fail = 0;
    const char *INFO = "!4059.60N/02735.98E>digi test";

    // (A) WIDE1-1 unused -> substituted with MYCALL*, same length
    memset(gDigiAge, 0, sizeof(gDigiAge));
    gDigiLen = 0;
    uint16_t len = MkFrame(f, "TB1AAW", 0, "WIDE1", 1, false, "WIDE2", 1, false, INFO);
    DigiConsider(f, len);
    if (gDigiLen != len) { printf("digi A: not queued (len=%u)\n", gDigiLen); fail = 1; }
    else {
        char call[7] = {0};
        for (int k = 0; k < 6; k++) call[k] = (char)(gDigiFrame[14 + k] >> 1);
        const uint8_t sb = gDigiFrame[20];
        if (strncmp(call, "TA1JS ", 6) != 0 || !(sb & 0x80) || ((sb >> 1) & 0x0F) != 7 || (sb & 1)) {
            printf("digi A: bad substitution call='%.6s' ssidbyte=%02X\n", call, sb); fail = 1;
        }
        // the rest of the frame must be untouched, and WIDE2-1 still unused
        if (memcmp(&gDigiFrame[21], &f[21], len - 21 - 2) != 0) { printf("digi A: tail changed\n"); fail = 1; }
        if (gDigiFrame[27] & 0x80) { printf("digi A: WIDE2 wrongly marked used\n"); fail = 1; }
    }

    // (B) same packet again inside the dedupe window -> ignored
    gDigiLen = 0;
    DigiConsider(f, len);
    if (gDigiLen != 0) { printf("digi B: duplicate was repeated\n"); fail = 1; }

    // (C) WIDE1-1 already used by another digi -> not ours
    memset(gDigiAge, 0, sizeof(gDigiAge)); gDigiLen = 0;
    len = MkFrame(f, "TB1AAW", 0, "WIDE1", 1, true, "WIDE2", 1, false, INFO);
    DigiConsider(f, len);
    if (gDigiLen != 0) { printf("digi C: repeated an already-used hop\n"); fail = 1; }

    // (D) first unused hop is WIDE2-2 -> fill-in role declines
    memset(gDigiAge, 0, sizeof(gDigiAge)); gDigiLen = 0;
    len = MkFrame(f, "TB1AAW", 0, "WIDE2", 2, false, NULL, 0, false, INFO);
    DigiConsider(f, len);
    if (gDigiLen != 0) { printf("digi D: repeated a WIDE2-2 hop\n"); fail = 1; }

    // (E) our own transmission -> never repeated
    memset(gDigiAge, 0, sizeof(gDigiAge)); gDigiLen = 0;
    len = MkFrame(f, "TA1JS", 7, "WIDE1", 1, false, NULL, 0, false, INFO);
    DigiConsider(f, len);
    if (gDigiLen != 0) { printf("digi E: repeated our own frame\n"); fail = 1; }

    // (F) no digipeater path at all -> nothing to do
    memset(gDigiAge, 0, sizeof(gDigiAge)); gDigiLen = 0;
    len = MkFrame(f, "TB1AAW", 0, NULL, 0, false, NULL, 0, false, INFO);
    DigiConsider(f, len);
    if (gDigiLen != 0) { printf("digi F: repeated a pathless frame\n"); fail = 1; }

    // (G) a different packet from the same station still gets through
    memset(gDigiAge, 0, sizeof(gDigiAge)); gDigiLen = 0;
    len = MkFrame(f, "TB1AAW", 0, "WIDE1", 1, false, NULL, 0, false, INFO);
    DigiConsider(f, len);
    uint16_t first = gDigiLen; gDigiLen = 0;
    len = MkFrame(f, "TB1AAW", 0, "WIDE1", 1, false, NULL, 0, false, "!4059.60N/02735.98E>moved");
    DigiConsider(f, len);
    if (first == 0 || gDigiLen == 0) { printf("digi G: distinct payload was suppressed\n"); fail = 1; }

    printf(fail ? "DIGIPEATER FAILED\n"
                : "DIGIPEATER PASSED (substitution, dedupe, used-hop, WIDE2, self, pathless, distinct)\n");
    return fail;
}

int main(void)
{
    // Build the frame exactly like APRS_TransmitHardcoded_N0CALL
    static const uint8_t INFO[] = "!1000.00N/02000.00E>UV-K5 APRS";
    uint8_t frame[7 * 4 + 2 + (sizeof(INFO) - 1) + 2];
    uint16_t idx = 0;
    AX25_EncodeAddress("APOVK5", 0, false, &frame[idx]); idx += 7;  // mirrors APRS_TOCALL
    AX25_EncodeAddress("N0CALL", 0, false, &frame[idx]); idx += 7;
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
              !memcmp(dst, "APOVK5", 6) && !memcmp(src, "N0CALL", 6) &&
              !memcmp(via, "WIDE1 ", 6) && !memcmp(via2, "WIDE2 ", 6) &&
              (rxframe[20] & 1) == 0 && (rxframe[27] & 1) == 1;
    printf(ok ? "ALL CHECKS PASSED\n" : "CHECKS FAILED\n");
    if (!ok) return 1;

    // ---- phase 2: the firmware's streaming RX decoder (APRS_DecodeCapture copy) ----
    // must recover the frame from a capture at any bit alignment and polarity
    int rx_fail = 0;
    for (int shift = 0; shift < 16; shift++) {
        // simulate a capture: noise byte + stream shifted by 0..7 bits, optionally inverted
        uint8_t cap[HDLC_BUF_SIZE + 2];
        int capbits = 8 + (int)w.bits + (shift & 7);
        memset(cap, 0, sizeof(cap));
        cap[0] = 0x35;  // leading noise
        for (uint32_t b = 0; b < w.bits; b++) {
            int pos = 8 + (shift & 7) + (int)b;
            uint8_t level = (hdlcbuf[b >> 3] >> (7 - (b & 7))) & 1;
            if (shift >= 8) level ^= 1;  // opposite NRZI polarity
            if (level) cap[pos >> 3] |= 0x80 >> (pos & 7);
        }
        int capbytes = (capbits + 7) / 8;

        // ---- verbatim logic from APRS_DecodeCapture ----
        uint8_t  rxf[80];
        uint8_t  prev2 = 0, ones2 = 0;
        uint16_t nbits = 0xFFFF;
        int found = 0;
        memset(rxf, 0, sizeof(rxf));
        for (uint32_t i = 0; i < (uint32_t)capbytes * 8u; i++) {
            const uint8_t level = (cap[i >> 3] >> (7u - (i & 7u))) & 1u;
            const uint8_t bit   = (level == prev2) ? 1u : 0u;
            prev2 = level;
            if (bit) {
                if (ones2 < 7) ones2++;
                if (ones2 >= 7) { nbits = 0xFFFF; continue; }
                if (nbits != 0xFFFF) {
                    if (nbits < sizeof(rxf) * 8u)
                        rxf[nbits >> 3] |= (uint8_t)(1u << (nbits & 7u));
                    nbits++;
                }
                continue;
            }
            if (ones2 == 5) { ones2 = 0; continue; }
            if (ones2 == 6) {
                if (nbits != 0xFFFF && nbits >= 7u) {
                    const uint16_t fb = nbits - 7u;
                    if ((fb & 7u) == 0 && fb >= 17u * 8u && (fb >> 3) <= sizeof(rxf)) {
                        const uint16_t len = fb >> 3;
                        const uint16_t f2 = AX25_CalculateFCS(rxf, len - 2u);
                        if (f2 == (uint16_t)(rxf[len - 2] | (rxf[len - 1] << 8))) {
                            if (len == idx && !memcmp(rxf, frame, idx)) found = 1;
                            break;
                        }
                    }
                }
                memset(rxf, 0, sizeof(rxf));
                nbits = 0; ones2 = 0;
                continue;
            }
            if (nbits != 0xFFFF) {
                if (nbits < sizeof(rxf) * 8u) nbits++;
                else nbits = 0xFFFF;
            }
            ones2 = 0;
        }
        if (!found) { printf("RX decode FAIL at shift %d\n", shift); rx_fail = 1; }
    }
    printf(rx_fail ? "RX STREAMING DECODER FAILED\n" : "RX STREAMING DECODER PASSED (16 alignments/polarities)\n");
    if (rx_fail) return 1;

    // ---- phase 3: position parsers ----
    int pfail = 0;
    int32_t lat, lon;

    // uncompressed: our own beacon text
    if (!APRS_ParseUncompressed((const uint8_t *)"1000.00N/02000.00E>", 19, &lat, &lon) ||
        lat != 10000000 || lon != 20000000) {
        printf("uncompressed FAIL: %d %d\n", lat, lon);
        pfail = 1;
    }

    // Mic-E: 33 deg 25.64' N, 112 deg 07.35' W (APRS 1.01 spec example values)
    // dest "S32UVT" (digits 3,3,2,5,6,4; N=1, lon offset=1, W=1), AX.25-shifted
    {
        uint8_t dest[7];
        const char *dc = "S32UVT";
        for (int k = 0; k < 6; k++) dest[k] = (uint8_t)(dc[k] << 1);
        dest[6] = 0x60;
        const uint8_t micinfo[9] = { 0x60, 12 + 28, 7 + 28, 35 + 28, 'x', 'x', 'x', '/', '>' };
        if (!APRS_ParseMicE(dest, micinfo, 9, &lat, &lon) ||
            lat != 33427333 || lon != -112122500) {
            printf("mic-e FAIL: %d %d\n", lat, lon);
            pfail = 1;
        }
    }

    // compressed: APRS 1.01 spec example "/5L!!<*e7>" = 49.5000N 72.7500W
    if (!APRS_ParseCompressed((const uint8_t *)"/5L!!<*e7>{?!", 13, &lat, &lon) ||
        abs(lat - 49500000) > 120 || abs(lon - -72750000) > 120) {
        printf("compressed FAIL: %d %d\n", lat, lon);
        pfail = 1;
    }

    printf(pfail ? "POSITION PARSERS FAILED\n" : "POSITION PARSERS PASSED (uncompressed, Mic-E, base-91)\n");

    // ---- phase 4: third-party (IS->RF gated) unwrap ----
    int tpfail = 0;
    {
        const char *pl; int pll; char src[16];

        // (A) gated message TO me (N0CALL, SSID 0) — the self-test round-trip
        if (!TP_UnwrapAndMatch("}N0CALL>APRS,TCPIP,IGATE*::N0CALL   :Gate test via IGATE{2",
                               "N0CALL", 0, &pl, &pll, src)
            || strcmp(src, "N0CALL") != 0
            || strcmp(pl, ":N0CALL   :Gate test via IGATE{2") != 0) {
            printf("third-party msg (to me) FAIL: src=%s payload=%.*s\n", src, pll, pl);
            tpfail = 1;
        }

        // (B) gated message addressed to bare N0CALL must NOT match when I am N0CALL-7
        if (TP_UnwrapAndMatch("}FOO>APRS,TCPIP,IGATE*::N0CALL   :x{4",
                              "N0CALL", 7, &pl, &pll, src)) {
            printf("third-party msg (SSID mismatch) FAIL: matched wrongly\n");
            tpfail = 1;
        }

        // (C) gated message to N0CALL-5, I am N0CALL-5 — matches, originator kept
        if (!TP_UnwrapAndMatch("}FOO-1>APRS,TCPIP,IGATE*::N0CALL-5 :hey{5",
                               "N0CALL", 5, &pl, &pll, src)
            || strcmp(src, "FOO-1") != 0) {
            printf("third-party msg (SSID match) FAIL: src=%s\n", src);
            tpfail = 1;
        }

        // (D) gated uncompressed position unwraps and parses
        if (TP_UnwrapAndMatch("}N0CALL>APRS,TCPIP,IGATE*:!1000.00N/02000.00E>UV-K5 APRS",
                              "N0CALL", 0, &pl, &pll, src)  // not a message -> 0
            || strcmp(src, "N0CALL") != 0 || pl[0] != '!') {
            printf("third-party posn (unwrap) FAIL: src=%s payload=%.*s\n", src, pll, pl);
            tpfail = 1;
        } else if (!APRS_ParseUncompressed((const uint8_t *)pl + 1, (uint16_t)(pll - 1), &lat, &lon)
                   || lat != 10000000 || lon != 20000000) {
            printf("third-party posn (parse) FAIL: %d %d\n", lat, lon);
            tpfail = 1;
        }

        // (E) regression: a normal (non-gated) direct message still matches
        if (!TP_UnwrapAndMatch(":N0CALL   :direct{6", "N0CALL", 0, &pl, &pll, src)
            || src[0] != 0 || strcmp(pl, ":N0CALL   :direct{6") != 0) {
            printf("non-gated direct msg FAIL: src=%s payload=%.*s\n", src, pll, pl);
            tpfail = 1;
        }
    }
    printf(tpfail ? "THIRD-PARTY UNWRAP FAILED\n" : "THIRD-PARTY UNWRAP PASSED (msg to me, SSID match/mismatch, gated posn, direct regression)\n");

    const int dfail = test_digi();

    return pfail || tpfail || dfail;
}
