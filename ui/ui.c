/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
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

#include <assert.h>
#include <string.h>

#include "app/chFrScanner.h"
#include "app/dtmf.h"
#ifdef ENABLE_FMRADIO
    #include "app/fm.h"
#endif
#include "driver/keyboard.h"
#include "driver/st7565.h"
#include "ui/helper.h"
#ifdef ENABLE_APRS
    #include "app/aprs_minimal.h"
#endif
#include "misc.h"
#ifdef ENABLE_AIRCOPY
    #include "ui/aircopy.h"
#endif
#ifdef ENABLE_FMRADIO
    #include "ui/fmradio.h"
#endif
#ifdef ENABLE_REGA
    #include "app/rega.h"
#endif
#include "ui/inputbox.h"
#include "ui/main.h"
#include "ui/menu.h"
#include "ui/scanner.h"
#include "ui/ui.h"
#include "../misc.h"

GUI_DisplayType_t gScreenToDisplay;
GUI_DisplayType_t gRequestDisplayScreen = DISPLAY_INVALID;

uint8_t           gAskForConfirmation;
bool              gAskToSave;
bool              gAskToDelete;


void (*UI_DisplayFunctions[])(void) = {
    [DISPLAY_MAIN] = &UI_DisplayMain,
    [DISPLAY_MENU] = &UI_DisplayMenu,
    [DISPLAY_SCANNER] = &UI_DisplayScanner,

#ifdef ENABLE_FMRADIO
    [DISPLAY_FM] = &UI_DisplayFM,
#endif

#ifdef ENABLE_AIRCOPY
    [DISPLAY_AIRCOPY] = &UI_DisplayAircopy,
#endif

#ifdef ENABLE_REGA
    [DISPLAY_REGA] = &UI_DisplayREGA,
#endif
};

static_assert(ARRAY_SIZE(UI_DisplayFunctions) == DISPLAY_N_ELEM);

#ifdef ENABLE_APRS
static void UI_DrawAPRSMessageBox(void)
{
    // Overlay box across the bottom half of the screen (frame rows 3..6),
    // completely covering the B-VFO/MR column (rows 4..6). Wipe the full
    // width first so nothing of VFO B shows through; VFO B is redrawn by
    // UI_DisplayMain once the message clears (sticky flag drops / times out).
    for (unsigned int r = 3; r <= 6; r++) {
        memset(gFrameBuffer[r], 0, LCD_WIDTH);
        gFrameBuffer[r][2]   = 0xFF;   // left frame
        gFrameBuffer[r][3]   = 0xFF;
        gFrameBuffer[r][124] = 0xFF;   // right frame
        gFrameBuffer[r][125] = 0xFF;
    }
    for (unsigned int x = 2; x < 126; x++) {
        gFrameBuffer[3][x] |= 0x01;   // top edge    (y = 32)
        gFrameBuffer[6][x] |= 0x80;   // bottom edge (y = 63; bit7 unused by font)
    }

    // up to 3 lines of 16 chars, vertically centred within text rows 4..6
    const unsigned int len = strlen(gAPRS_RxDisplay);
    unsigned int nlines = (len + 15) / 16;
    if (nlines < 1) nlines = 1;
    if (nlines > 3) nlines = 3;
    const unsigned int start = 4 + (3 - nlines) / 2;

    char line[17];
    for (unsigned int r = 0; r < nlines; r++) {
        unsigned int n = len - r * 16;
        if (n > 16)
            n = 16;
        memcpy(line, &gAPRS_RxDisplay[r * 16], n);
        line[n] = 0;
        UI_PrintStringSmallNormal(line, 8, 0, start + r);
    }
}
#endif

void GUI_DisplayScreen(void)
{
    if (gScreenToDisplay != DISPLAY_INVALID) {
        UI_DisplayFunctions[gScreenToDisplay]();
    }

#ifdef ENABLE_APRS
    // Draw any APRS RX text (a message to me, or a decoded position/telemetry
    // packet) inside the cleared, framed box — never printed raw over the main
    // screen, so a long packet can't garble the VFO-B column or status line.
    if (gAPRS_RxDisplay[0]) {
        UI_DrawAPRSMessageBox();
        ST7565_BlitFullScreen();
    }
#endif
}

void GUI_SelectNextDisplay(GUI_DisplayType_t Display)
{
    if (Display == DISPLAY_INVALID)
        return;

    if (gScreenToDisplay != Display)
    {
        DTMF_clear_input_box();

        gInputBoxIndex       = 0;
        gIsInSubMenu         = false;
        gCssBackgroundScan   = false;
        gScanStateDir        = SCAN_OFF;
        #ifdef ENABLE_FMRADIO
            gFM_ScanState    = FM_SCAN_OFF;
        #endif
        gAskForConfirmation  = 0;
        gAskToSave           = false;
        gAskToDelete         = false;
        gWasFKeyPressed      = false;

        gUpdateStatus        = true;
    }

    gScreenToDisplay = Display;
    gUpdateDisplay   = true;
}
