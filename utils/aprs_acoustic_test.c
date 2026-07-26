// Cross-checks the two ends of the acoustic link without a radio: takes the bit
// stream utils/aprs-acoustic.html actually generates and runs it through the
// framing and CRC from app/aprs_minimal.c (APRS_AcousticReceive).
//
//   node  -e "$(the page's frameBits)"        -> bit string
//   cc -o /tmp/t utils/aprs_acoustic_test.c && /tmp/t <bits>
//
// Keep in sync with APRS_AcousticReceive if the frame ever changes. What this
// proves is agreement between the encoder and decoder plus that a single
// flipped bit is refused; audio, timing and the envelope still need a radio.
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// verbatim from app/aprs_minimal.c
static uint16_t APRS_CalculateCRC(const uint8_t *data, uint16_t length){
    uint16_t crc = 0xFFFF;
    for (uint16_t i=0;i<length;i++){ crc ^= data[i];
        for (uint8_t j=0;j<8;j++) crc = (crc & 1) ? (crc>>1)^0x8408 : crc>>1; }
    return crc;
}

// the decoder's symbol stream: each bit becomes two symbols (Manchester)
static int pos; static const char *SYM;
static bool sym(void){ return SYM[pos++] == '1'; }

int main(int argc, char **argv){
    const char *bits = argv[1];
    // build the symbol stream exactly as the page plays it
    static char syms[8192]; int n=0;
    for (const char *p=bits; *p; p++){ syms[n++] = *p=='1'?'1':'0'; syms[n++] = *p=='1'?'0':'1'; }
    syms[n]=0; SYM=syms; pos=0;

    // --- mirror of APRS_AcousticReceive's framing ---
    uint8_t sync=0; int guard=0;
    do { bool a=sym(), b=sym();
         if (a==b){ printf("FAIL: manchester violation while hunting sync\n"); return 1; }
         sync = (uint8_t)((sync<<1)|(a?1:0));
    } while (sync != 0xA5 && ++guard < 200);
    if (sync != 0xA5){ printf("FAIL: no sync\n"); return 1; }
    printf("sync found after %d bits of preamble\n", guard);

    uint8_t buf[10];
    for (int i=0;i<10;i++){ uint8_t by=0;
        for (int k=0;k<8;k++){ bool a=sym(), b=sym();
            if (a==b){ printf("FAIL: manchester violation in payload\n"); return 1; }
            by=(uint8_t)((by<<1)|(a?1:0)); }
        buf[i]=by; }

    uint16_t want = (uint16_t)(buf[8] | (buf[9]<<8));
    uint16_t got  = APRS_CalculateCRC(buf, 8);
    printf("crc: frame=%u computed=%u %s\n", want, got, want==got?"MATCH":"MISMATCH");
    if (want!=got) return 1;

    int32_t la, lo; memcpy(&la,&buf[0],4); memcpy(&lo,&buf[4],4);
    printf("decoded: lat=%d lon=%d  (%.5f, %.5f)\n", la, lo, la/1e6, lo/1e6);
    if (la!=40993400 || lo!=27599700){ printf("FAIL: wrong coordinates\n"); return 1; }

    // and prove a single flipped bit is caught
    buf[3] ^= 0x01;
    printf("bit flip -> crc %s (must be rejected)\n",
           APRS_CalculateCRC(buf,8)==want ? "STILL MATCHES — BAD" : "rejected");
    return APRS_CalculateCRC(buf,8)==want;
}
