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
    // clear the interior and frame it
    for (unsigned int r = 1; r <= 5; r++) {
        memset(&gFrameBuffer[r][2], 0, 124);
        gFrameBuffer[r][2]   = 0xFF;
        gFrameBuffer[r][3]   = 0xFF;
        gFrameBuffer[r][124] = 0xFF;
        gFrameBuffer[r][125] = 0xFF;
    }
    for (unsigned int x = 2; x < 126; x++) {
        gFrameBuffer[1][x] |= 0x03;   // top edge
        gFrameBuffer[5][x] |= 0xC0;   // bottom edge
    }

    // up to 3 lines of 16 chars
    char line[17];
    const unsigned int len = strlen(gAPRS_RxDisplay);
    for (unsigned int r = 0; r < 3 && r * 16 < len; r++) {
        unsigned int n = len - r * 16;
        if (n > 16)
            n = 16;
        memcpy(line, &gAPRS_RxDisplay[r * 16], n);
        line[n] = 0;
        UI_PrintStringSmallNormal(line, 8, 0, 2 + r);
    }
}
#endif

void GUI_DisplayScreen(void)
{
    if (gScreenToDisplay != DISPLAY_INVALID) {
        UI_DisplayFunctions[gScreenToDisplay]();
    }

#ifdef ENABLE_APRS
    if (gAPRS_RxSticky) {
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
