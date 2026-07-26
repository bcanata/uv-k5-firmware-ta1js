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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if !defined(ENABLE_OVERLAY)
    #include "ARMCM0.h"
#endif
#include "app/dtmf.h"
#include "app/generic.h"
#include "app/menu.h"
#include "app/scanner.h"
#ifdef ENABLE_APRS
    #include "aprs_minimal.h"
#endif
#include "audio.h"
#include "board.h"
#include "bsp/dp32g030/gpio.h"
#include "driver/backlight.h"
#include "driver/bk4819.h"
#include "driver/eeprom.h"
#include "driver/gpio.h"
#include "driver/keyboard.h"
#include "frequencies.h"
#include "helper/battery.h"
#include "misc.h"
#include "settings.h"
#if defined(ENABLE_OVERLAY)
    #include "sram-overlay.h"
#endif
#include "ui/inputbox.h"
#include "ui/menu.h"

#ifdef ENABLE_APRS
// Editable APRS text/numeric fields: returns max length (0 = not one of them),
// numeric fields report the fixed '.' position via dot_pos, text fields -1.
static int APRS_EditMaxLen(int id, int *dot_pos)
{
    int dot = -1, len = 0;
    switch (id) {
        case MENU_APRS_CALL:
        case MENU_APRS_MSGTO:  len = 9;          break;
        case MENU_APRS_CMNT:
        case MENU_APRS_MSGTXT: len = 31;          break;
        case MENU_APRS_LOC:    len = 15; dot = 15; break;  // numeric, no dot
    }
    if (dot_pos)
        *dot_pos = dot;
    return len;
}

// Two-digit character entry for text fields (kept in sync with
// utils/aprs-location.html): 00-09 digits, 10-35 A-Z, 36+ punctuation.
static const char APRS_PairCharset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ -./?!@:,'";
static uint8_t aprs_pair_first = 0xFF;
#endif
#include "ui/ui.h"

#ifndef ARRAY_SIZE
    #define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif

uint8_t gUnlockAllTxConfCnt;

#ifdef ENABLE_F_CAL_MENU
    void writeXtalFreqCal(const int32_t value, const bool update_eeprom)
    {
        BK4819_WriteRegister(BK4819_REG_3B, 22656 + value);

        if (update_eeprom)
        {
            struct
            {
                int16_t  BK4819_XtalFreqLow;
                uint16_t EEPROM_1F8A;
                uint16_t EEPROM_1F8C;
                uint8_t  VOLUME_GAIN;
                uint8_t  DAC_GAIN;
            } __attribute__((packed)) misc;

            gEeprom.BK4819_XTAL_FREQ_LOW = value;

            // radio 1 .. 04 00 46 00 50 00 2C 0E
            // radio 2 .. 05 00 46 00 50 00 2C 0E
            //
            EEPROM_ReadBuffer(0x1F88, &misc, 8);
            misc.BK4819_XtalFreqLow = value;
            EEPROM_WriteBuffer(0x1F88, &misc);
        }
    }
#endif

void MENU_StartCssScan(void)
{
    SCANNER_Start(true);
    gUpdateStatus = true;
    gCssBackgroundScan = true;

    gRequestDisplayScreen = DISPLAY_MENU;
}

void MENU_CssScanFound(void)
{
    if(gScanCssResultType == CODE_TYPE_DIGITAL || gScanCssResultType == CODE_TYPE_REVERSE_DIGITAL) {
        gMenuCursor = UI_MENU_GetMenuIdx(MENU_R_DCS);
    }
    else if(gScanCssResultType == CODE_TYPE_CONTINUOUS_TONE) {
        gMenuCursor = UI_MENU_GetMenuIdx(MENU_R_CTCS);
    }

    MENU_ShowCurrentSetting();

    gUpdateStatus = true;
    gUpdateDisplay = true;
}

void MENU_StopCssScan(void)
{
    gCssBackgroundScan = false;

#ifdef ENABLE_VOICE
    gAnotherVoiceID       = VOICE_ID_SCANNING_STOP;
#endif
    gUpdateDisplay = true;
    gUpdateStatus = true;
}

int MENU_GetLimits(uint8_t menu_id, int32_t *pMin, int32_t *pMax)
{
    *pMin = 0;

    switch (menu_id)
    {
        case MENU_SQL:
            //*pMin = 0;
            *pMax = 9;
            break;

        case MENU_STEP:
            //*pMin = 0;
            *pMax = STEP_N_ELEM - 1;
            break;

        case MENU_ABR:
            //*pMin = 0;
            *pMax = 61;
            break;

        case MENU_ABR_MIN:
            //*pMin = 0;
            *pMax = 9;
            break;

        case MENU_ABR_MAX:
            *pMin = 1;
            *pMax = 10;
            break;

        case MENU_F_LOCK:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_F_LOCK) - 1;
            break;

        case MENU_MDF:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_MDF) - 1;
            break;

        case MENU_TXP:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_TXP) - 1;
            break;

        case MENU_SFT_D:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SFT_D) - 1;
            break;

        case MENU_TDR:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_RXMode) - 1;
            break;

        #ifdef ENABLE_VOICE
            case MENU_VOICE:
                //*pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_VOICE) - 1;
                break;
        #endif

        case MENU_SC_REV:
            //*pMin = 0;
            *pMax = 104;
            break;

        case MENU_ROGER:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_ROGER) - 1;
            break;

        case MENU_PONMSG:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_PONMSG) - 1;
            break;

        case MENU_R_DCS:
        case MENU_T_DCS:
            //*pMin = 0;
            *pMax = 208;
            //*pMax = (ARRAY_SIZE(DCS_Options) * 2);
            break;

        case MENU_R_CTCS:
        case MENU_T_CTCS:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(CTCSS_Options);
            break;

        case MENU_W_N:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_W_N) - 1;
            break;

        #ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                //*pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_AL_MOD) - 1;
                break;
        #endif

        case MENU_RESET:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_RESET) - 1;
            break;

        case MENU_COMPAND:
        case MENU_ABR_ON_TX_RX:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_RX_TX) - 1;
            break;

        #ifndef ENABLE_FEAT_F4HWN
            #ifdef ENABLE_AM_FIX
                case MENU_AM_FIX:
            #endif
        #endif
        #ifdef ENABLE_AUDIO_BAR
            case MENU_MIC_BAR:
        #endif
        case MENU_BCL:
        case MENU_BEEP:
        case MENU_S_ADD1:
        case MENU_S_ADD2:
        case MENU_S_ADD3:
        case MENU_STE:
        case MENU_D_ST:
#ifdef ENABLE_DTMF_CALLING
        case MENU_D_DCD:
#endif
        case MENU_D_LIVE_DEC:
        #ifdef ENABLE_NOAA
            case MENU_NOAA_S:
        #endif
#ifndef ENABLE_FEAT_F4HWN
        case MENU_350TX:
        case MENU_200TX:
        case MENU_500TX:
#endif
        case MENU_350EN:
#ifndef ENABLE_FEAT_F4HWN
        case MENU_SCREN:
#endif
#ifdef ENABLE_FEAT_F4HWN
        case MENU_SET_TMR:
#endif
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_OFF_ON) - 1;
            break;
        case MENU_AM:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gModulationStr) - 1;
            break;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_SCR:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SCRAMBLER) - 1;
            break;
#endif

        case MENU_AUTOLK:
            *pMax = 40;
            break;

        case MENU_TOT:
            //*pMin = 0;
            *pMin = 5;
            *pMax = 179;
            break;

        #ifdef ENABLE_VOX
            case MENU_VOX:
        #endif
        case MENU_RP_STE:
            //*pMin = 0;
            *pMax = 10;
            break;

        case MENU_MEM_CH:
        case MENU_1_CALL:
        case MENU_DEL_CH:
        case MENU_MEM_NAME:
            //*pMin = 0;
            *pMax = MR_CHANNEL_LAST;
            break;

        case MENU_SLIST1:
        case MENU_SLIST2:
        case MENU_SLIST3:
            *pMin = -1;
            *pMax = MR_CHANNEL_LAST;
            break;

        case MENU_SAVE:
            //*pMin = 0;
            *pMax = 5;
            break;

        case MENU_MIC:
            //*pMin = 0;
            *pMax = 4;
            break;

        case MENU_S_LIST:
            //*pMin = 0;
            *pMax = 5;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_RSP:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_D_RSP) - 1;
            break;
#endif
        case MENU_PTT_ID:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_PTT_ID) - 1;
            break;

        case MENU_BAT_TXT:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_BAT_TXT) - 1;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_HOLD:
            *pMin = 5;
            *pMax = 60;
            break;
#endif
        case MENU_D_PRE:
            *pMin = 3;
            *pMax = 99;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_LIST:
            *pMin = 1;
            *pMax = 16;
            break;
#endif
        #ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                *pMin = -50;
                *pMax = +50;
                break;
        #endif

        case MENU_BATCAL:
            *pMin = 1600;
            *pMax = 2200;
            break;

        case MENU_BATTYP:
            //*pMin = 0;
            *pMax = 2;
            break;

#ifdef ENABLE_APRS
        case MENU_APRS_ON:
#ifdef ENABLE_APRS_DIGI
        case MENU_APRS_DIGI:
#endif
        case MENU_APRS_TX:
        case MENU_APRS_MSGSND:
#ifdef ENABLE_APRS_ACOUSTIC
        case MENU_APRS_ACIN:
#endif
            *pMax = 1;  // OFF/ON
            break;
        case MENU_APRS_INTV:
            *pMin = 0;
            *pMax = 60;  // 0 = beacon off, 1-60 minutes
            break;
        case MENU_APRS_SSID:
            *pMax = 15;  // SSID 0-15
            break;
#endif
        case MENU_F1SHRT:
        case MENU_F1LONG:
        case MENU_F2SHRT:
        case MENU_F2LONG:
        case MENU_MLONG:
            //*pMin = 0;
            *pMax = gSubMenu_SIDEFUNCTIONS_size-1;
            break;

#ifdef ENABLE_FEAT_F4HWN_SLEEP
        case MENU_SET_OFF:
            *pMax = 120;
            break;
#endif

#ifdef ENABLE_FEAT_F4HWN
        case MENU_SET_PWR:
            *pMax = ARRAY_SIZE(gSubMenu_SET_PWR) - 1;
            break;
        case MENU_SET_PTT:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SET_PTT) - 1;
            break;
        case MENU_SET_TOT:
        case MENU_SET_EOT:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SET_TOT) - 1;
            break;
#ifdef ENABLE_FEAT_F4HWN_CTR
        case MENU_SET_CTR:
            *pMin = 1;
            *pMax = 15;
            break;
#endif
        case MENU_TX_LOCK:
#ifdef ENABLE_FEAT_F4HWN_INV
        case MENU_SET_INV:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_OFF_ON) - 1;
            break;
#endif
        case MENU_SET_LCK:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SET_LCK) - 1;
            break;
        case MENU_SET_MET:
        case MENU_SET_GUI:
            //*pMin = 0;
            *pMax = ARRAY_SIZE(gSubMenu_SET_MET) - 1;
            break;
        #ifdef ENABLE_FEAT_F4HWN_NARROWER
            case MENU_SET_NFM:
                //*pMin = 0;
                *pMax = ARRAY_SIZE(gSubMenu_SET_NFM) - 1;
                break;
        #endif
        #ifdef ENABLE_FEAT_F4HWN_VOL
            case MENU_SET_VOL:
                //*pMin = 0;
                *pMax = 63;
                break;
        #endif
        #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            case MENU_SET_KEY:
                //*pMin = 0;
                *pMax = 4;
                break;
        #endif
#endif

        default:
            return -1;
    }

    return 0;
}

void MENU_AcceptSetting(void)
{
    int32_t        Min;
    int32_t        Max;
    FREQ_Config_t *pConfig = &gTxVfo->freq_config_RX;

    if (!MENU_GetLimits(UI_MENU_GetCurrentMenuId(), &Min, &Max))
    {
        if (gSubMenuSelection < Min) gSubMenuSelection = Min;
        else
        if (gSubMenuSelection > Max) gSubMenuSelection = Max;
    }

    switch (UI_MENU_GetCurrentMenuId())
    {
        default:
            return;

        case MENU_SQL:
            gEeprom.SQUELCH_LEVEL = gSubMenuSelection;
            gVfoConfigureMode     = VFO_CONFIGURE;
            break;

        case MENU_STEP:
            gTxVfo->STEP_SETTING = FREQUENCY_GetStepIdxFromSortedIdx(gSubMenuSelection);
            if (IS_FREQ_CHANNEL(gTxVfo->CHANNEL_SAVE))
            {
                gRequestSaveChannel = 1;
            }
            return;

        case MENU_TXP:
            gTxVfo->OUTPUT_POWER = gSubMenuSelection;
            gRequestSaveChannel = 1;
            return;

        case MENU_T_DCS:
            pConfig = &gTxVfo->freq_config_TX;

            // Fallthrough

        case MENU_R_DCS: {
            if (gSubMenuSelection == 0) {
                if (pConfig->CodeType == CODE_TYPE_CONTINUOUS_TONE) {
                    return;
                }
                pConfig->Code = 0;
                pConfig->CodeType = CODE_TYPE_OFF;
            }
            else if (gSubMenuSelection < 105) {
                pConfig->CodeType = CODE_TYPE_DIGITAL;
                pConfig->Code = gSubMenuSelection - 1;
            }
            else {
                pConfig->CodeType = CODE_TYPE_REVERSE_DIGITAL;
                pConfig->Code = gSubMenuSelection - 105;
            }

            gRequestSaveChannel = 1;
            return;
        }
        case MENU_T_CTCS:
            pConfig = &gTxVfo->freq_config_TX;
            [[fallthrough]];
        case MENU_R_CTCS: {
            if (gSubMenuSelection == 0) {
                if (pConfig->CodeType != CODE_TYPE_CONTINUOUS_TONE) {
                    return;
                }
                pConfig->Code     = 0;
                pConfig->CodeType = CODE_TYPE_OFF;
            }
            else {
                pConfig->Code     = gSubMenuSelection - 1;
                pConfig->CodeType = CODE_TYPE_CONTINUOUS_TONE;
            }

            gRequestSaveChannel = 1;
            return;
        }
        case MENU_SFT_D:
            gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION = gSubMenuSelection;
            gRequestSaveChannel                   = 1;
            return;

        case MENU_OFFSET:
            gTxVfo->TX_OFFSET_FREQUENCY = gSubMenuSelection;
            gRequestSaveChannel         = 1;
            return;

        case MENU_W_N:
            gTxVfo->CHANNEL_BANDWIDTH = gSubMenuSelection;
            gRequestSaveChannel       = 1;
            return;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_SCR:
            gTxVfo->SCRAMBLING_TYPE = gSubMenuSelection;
            #if 0
                if (gSubMenuSelection > 0 && gSetting_ScrambleEnable)
                    BK4819_EnableScramble(gSubMenuSelection - 1);
                else
                    BK4819_DisableScramble();
            #endif
            gRequestSaveChannel     = 1;
            return;
#endif

        case MENU_BCL:
            gTxVfo->BUSY_CHANNEL_LOCK = gSubMenuSelection;
            gRequestSaveChannel       = 1;
            return;

        case MENU_MEM_CH:
            gTxVfo->CHANNEL_SAVE = gSubMenuSelection;
            #if 0
                gEeprom.MrChannel[0] = gSubMenuSelection;
            #else
                gEeprom.MrChannel[gEeprom.TX_VFO] = gSubMenuSelection;
            #endif
            gRequestSaveChannel = 2;
            gVfoConfigureMode   = VFO_CONFIGURE_RELOAD;
            gFlagResetVfos      = true;
            return;

        case MENU_MEM_NAME:
            for (int i = 9; i >= 0; i--) {
                if (edit[i] != ' ' && edit[i] != '_' && edit[i] != 0x00 && edit[i] != 0xff)
                    break;
                edit[i] = ' ';
            }

            SETTINGS_SaveChannelName(gSubMenuSelection, edit);
            return;

        case MENU_SAVE:
            gEeprom.BATTERY_SAVE = gSubMenuSelection;
            break;

        #ifdef ENABLE_VOX
            case MENU_VOX:
                gEeprom.VOX_SWITCH = gSubMenuSelection != 0;
                if (gEeprom.VOX_SWITCH)
                    gEeprom.VOX_LEVEL = gSubMenuSelection - 1;
                SETTINGS_LoadCalibration();
                gFlagReconfigureVfos = true;
                gUpdateStatus        = true;
                break;
        #endif

        case MENU_ABR:
            gEeprom.BACKLIGHT_TIME = gSubMenuSelection;
            #ifdef ENABLE_FEAT_F4HWN
                gBackLight = false;
            #endif
            break;

        case MENU_ABR_MIN:
            gEeprom.BACKLIGHT_MIN = gSubMenuSelection;
            gEeprom.BACKLIGHT_MAX = MAX(gSubMenuSelection + 1 , gEeprom.BACKLIGHT_MAX);
            break;

        case MENU_ABR_MAX:
            gEeprom.BACKLIGHT_MAX = gSubMenuSelection;
            gEeprom.BACKLIGHT_MIN = MIN(gSubMenuSelection - 1, gEeprom.BACKLIGHT_MIN);
            break;

        case MENU_ABR_ON_TX_RX:
            gSetting_backlight_on_tx_rx = gSubMenuSelection;
            break;

        case MENU_TDR:
            gEeprom.DUAL_WATCH = (gEeprom.TX_VFO + 1) * (gSubMenuSelection & 1);
            gEeprom.CROSS_BAND_RX_TX = (gEeprom.TX_VFO + 1) * ((gSubMenuSelection & 2) > 0);

            #ifdef ENABLE_FEAT_F4HWN
                gDW = gEeprom.DUAL_WATCH;
                gCB = gEeprom.CROSS_BAND_RX_TX;
                gSaveRxMode = true;
            #endif

            gFlagReconfigureVfos = true;
            gUpdateStatus        = true;
            break;

        case MENU_BEEP:
            gEeprom.BEEP_CONTROL = gSubMenuSelection;
            break;

        case MENU_TOT:
            gEeprom.TX_TIMEOUT_TIMER = gSubMenuSelection;
            break;

        #ifdef ENABLE_VOICE
            case MENU_VOICE:
                gEeprom.VOICE_PROMPT = gSubMenuSelection;
                gUpdateStatus        = true;
                break;
        #endif

        case MENU_SC_REV:
            gEeprom.SCAN_RESUME_MODE = gSubMenuSelection;
            break;

        case MENU_MDF:
            gEeprom.CHANNEL_DISPLAY_MODE = gSubMenuSelection;
            break;

        case MENU_AUTOLK:
            gEeprom.AUTO_KEYPAD_LOCK = gSubMenuSelection;
            gKeyLockCountdown        = gEeprom.AUTO_KEYPAD_LOCK * 30; // 15 seconds step
            break;

        case MENU_S_ADD1:
            gTxVfo->SCANLIST1_PARTICIPATION = gSubMenuSelection;
            SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true, false, true);
            gVfoConfigureMode = VFO_CONFIGURE;
            gFlagResetVfos    = true;
            return;

        case MENU_S_ADD2:
            gTxVfo->SCANLIST2_PARTICIPATION = gSubMenuSelection;
            SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true, false, true);
            gVfoConfigureMode = VFO_CONFIGURE;
            gFlagResetVfos    = true;
            return;

        case MENU_S_ADD3:
            gTxVfo->SCANLIST3_PARTICIPATION = gSubMenuSelection;
            SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true, false, true);
            gVfoConfigureMode = VFO_CONFIGURE;
            gFlagResetVfos    = true;
            return;

        case MENU_STE:
            gEeprom.TAIL_TONE_ELIMINATION = gSubMenuSelection;
            break;

        case MENU_RP_STE:
            gEeprom.REPEATER_TAIL_TONE_ELIMINATION = gSubMenuSelection;
            break;

        case MENU_MIC:
            gEeprom.MIC_SENSITIVITY = gSubMenuSelection;
            SETTINGS_LoadCalibration();
            gFlagReconfigureVfos = true;
            break;

        #ifdef ENABLE_AUDIO_BAR
            case MENU_MIC_BAR:
                gSetting_mic_bar = gSubMenuSelection;
                break;
        #endif

        case MENU_COMPAND:
            gTxVfo->Compander = gSubMenuSelection;
            SETTINGS_UpdateChannel(gTxVfo->CHANNEL_SAVE, gTxVfo, true, false, true);
            gVfoConfigureMode = VFO_CONFIGURE;
            gFlagResetVfos    = true;
//          gRequestSaveChannel = 1;
            return;

        case MENU_1_CALL:
            gEeprom.CHAN_1_CALL = gSubMenuSelection;
            break;

        case MENU_S_LIST:
            gEeprom.SCAN_LIST_DEFAULT = gSubMenuSelection;
            break;

        #ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                gEeprom.ALARM_MODE = gSubMenuSelection;
                break;
        #endif

        case MENU_D_ST:
            gEeprom.DTMF_SIDE_TONE = gSubMenuSelection;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_RSP:
            gEeprom.DTMF_DECODE_RESPONSE = gSubMenuSelection;
            break;

        case MENU_D_HOLD:
            gEeprom.DTMF_auto_reset_time = gSubMenuSelection;
            break;
#endif
        case MENU_D_PRE:
            gEeprom.DTMF_PRELOAD_TIME = gSubMenuSelection * 10;
            break;

        case MENU_PTT_ID:
            gTxVfo->DTMF_PTT_ID_TX_MODE = gSubMenuSelection;
            gRequestSaveChannel         = 1;
            return;

        case MENU_BAT_TXT:
            gSetting_battery_text = gSubMenuSelection;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_DCD:
            gTxVfo->DTMF_DECODING_ENABLE = gSubMenuSelection;
            DTMF_clear_RX();
            gRequestSaveChannel = 1;
            return;
#endif

        case MENU_D_LIVE_DEC:
            gSetting_live_DTMF_decoder = gSubMenuSelection;
            gDTMF_RX_live_timeout = 0;
            memset(gDTMF_RX_live, 0, sizeof(gDTMF_RX_live));
            if (!gSetting_live_DTMF_decoder)
                BK4819_DisableDTMF();
            gFlagReconfigureVfos     = true;
            gUpdateStatus            = true;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_LIST:
            gDTMF_chosen_contact = gSubMenuSelection - 1;
            if (gIsDtmfContactValid)
            {
                GUI_SelectNextDisplay(DISPLAY_MAIN);
                gDTMF_InputMode       = true;
                gDTMF_InputBox_Index  = 3;
                memcpy(gDTMF_InputBox, gDTMF_ID, 4);
                gRequestDisplayScreen = DISPLAY_INVALID;
            }
            return;
#endif
        case MENU_PONMSG:
            gEeprom.POWER_ON_DISPLAY_MODE = gSubMenuSelection;
            break;

        case MENU_ROGER:
            gEeprom.ROGER = gSubMenuSelection;
            break;

        case MENU_AM:
            gTxVfo->Modulation     = gSubMenuSelection;
            gRequestSaveChannel = 1;
            return;

        #ifndef ENABLE_FEAT_F4HWN
            #ifdef ENABLE_AM_FIX
                case MENU_AM_FIX:
                    gSetting_AM_fix = gSubMenuSelection;
                    gVfoConfigureMode = VFO_CONFIGURE_RELOAD;
                    gFlagResetVfos    = true;
                    break;
            #endif
        #endif

        #ifdef ENABLE_NOAA
            case MENU_NOAA_S:
                gEeprom.NOAA_AUTO_SCAN = gSubMenuSelection;
                gFlagReconfigureVfos   = true;
                break;
        #endif

        case MENU_DEL_CH:
            SETTINGS_UpdateChannel(gSubMenuSelection, NULL, false, false, true);
            gVfoConfigureMode = VFO_CONFIGURE_RELOAD;
            gFlagResetVfos    = true;
            return;

        case MENU_RESET:
            SETTINGS_FactoryReset(gSubMenuSelection);
            return;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_350TX:
            gSetting_350TX = gSubMenuSelection;
            break;
#endif

        case MENU_F_LOCK: {
            if(gSubMenuSelection == F_LOCK_NONE) { // select 10 times to enable
                gUnlockAllTxConfCnt++;
#ifdef ENABLE_FEAT_F4HWN
                if(gUnlockAllTxConfCnt < 3)
#else
                if(gUnlockAllTxConfCnt < 10)
#endif
                    return;
            }
            else
                gUnlockAllTxConfCnt = 0;

            gSetting_F_LOCK = gSubMenuSelection;

            #ifdef ENABLE_FEAT_F4HWN
            if(gSetting_F_LOCK == F_LOCK_ALL) {
                SETTINGS_ResetTxLock();
            }
            #endif
            break;
        }
#ifndef ENABLE_FEAT_F4HWN
        case MENU_200TX:
            gSetting_200TX = gSubMenuSelection;
            break;

        case MENU_500TX:
            gSetting_500TX = gSubMenuSelection;
            break;
#endif
        case MENU_350EN:
            gSetting_350EN       = gSubMenuSelection;
            gVfoConfigureMode    = VFO_CONFIGURE_RELOAD;
            gFlagResetVfos       = true;
            break;
#ifndef ENABLE_FEAT_F4HWN
        case MENU_SCREN:
            gSetting_ScrambleEnable = gSubMenuSelection;
            gFlagReconfigureVfos    = true;
            break;
#endif

        #ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                writeXtalFreqCal(gSubMenuSelection, true);
                return;
        #endif

        case MENU_BATCAL:
        {                                                                // voltages are averages between discharge curves of 1600 and 2200 mAh
            // gBatteryCalibration[0] = (520ul * gSubMenuSelection) / 760;  // 5.20V empty, blinking above this value, reduced functionality below
            // gBatteryCalibration[1] = (689ul * gSubMenuSelection) / 760;  // 6.89V,  ~5%, 1 bars above this value
            // gBatteryCalibration[2] = (724ul * gSubMenuSelection) / 760;  // 7.24V, ~17%, 2 bars above this value
            gBatteryCalibration[3] =          gSubMenuSelection;         // 7.6V,  ~29%, 3 bars above this value
            // gBatteryCalibration[4] = (771ul * gSubMenuSelection) / 760;  // 7.71V, ~65%, 4 bars above this value
            // gBatteryCalibration[5] = 2300;
            SETTINGS_SaveBatteryCalibration(gBatteryCalibration);
            return;
        }

        case MENU_BATTYP:
            gEeprom.BATTERY_TYPE = gSubMenuSelection;
            break;

#ifdef ENABLE_APRS
        case MENU_APRS_ON:
            gEeprom.APRS_ON = gSubMenuSelection;
            APRS_ResetBeaconTimer();
            break;
#ifdef ENABLE_APRS_DIGI
        case MENU_APRS_DIGI:
            gAPRS_DIGI = (gSubMenuSelection != 0);
            break;
#endif
        case MENU_APRS_INTV:
            gEeprom.APRS_INTERVAL = gSubMenuSelection;
            APRS_ResetBeaconTimer();
            break;
        case MENU_APRS_CALL:
            // Sanitize callsign: A-Z/0-9, stop at '_' padding or space.
            {
                uint8_t o = 0;
                for (uint8_t i = 0; i < 9 && o < 9; i++) {
                    char c = edit[i];
                    if (c == '_' || c == ' ' || c == 0)
                        break;
                    if (c >= 'a' && c <= 'z')
                        c -= 32;
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) {
                        gEeprom.APRS_CALLSIGN[o++] = c;
                    }
                }
                gEeprom.APRS_CALLSIGN[o] = 0;
            }
            break;
        case MENU_APRS_SSID:
            gEeprom.APRS_SSID = gSubMenuSelection;
            break;
        case MENU_APRS_LOC:
            // 15-digit phone code: (lat+90)*1e4 [7] + (lon+180)*1e4 [7] + checksum [1]
            {
                uint32_t sum = 0, la = 0, lo = 0;
                uint8_t ok = 1;
                for (uint8_t i = 0; i < 15 && ok; i++) {
                    const char c = edit[i];
                    if (c < '0' || c > '9') { ok = 0; break; }
                    const uint8_t d = (uint8_t)(c - '0');
                    if (i < 7)
                        la = la * 10u + d;
                    else if (i < 14)
                        lo = lo * 10u + d;
                    if (i < 14)
                        sum += (uint32_t)(i + 1) * d;
                    else if ((sum % 10u) != d)
                        ok = 0;
                }
                if (ok && la <= 1800000u && lo <= 3600000u && (la || lo)) {
                    gEeprom.APRS_LATITUDE  = (int32_t)(la * 100u) - 90000000;
                    gEeprom.APRS_LONGITUDE = (int32_t)(lo * 100u) - 180000000;
                }
            }
            break;
        case MENU_APRS_CMNT:
            // Copy + trim, limit to 31 chars.
            {
                uint8_t len = 0;
                for (; len < 31 && edit[len] != 0; len++) {
                    gEeprom.APRS_COMMENT[len] = edit[len];
                }
                while (len > 0 && gEeprom.APRS_COMMENT[len - 1] == ' ')
                    len--;
                gEeprom.APRS_COMMENT[len] = 0;
            }
            break;
        case MENU_APRS_MSGTO:
            {
                uint8_t o = 0;
                for (uint8_t i = 0; i < 9 && o < 9; i++) {
                    char c = edit[i];
                    if (c == '_' || c == ' ' || c == 0)
                        break;
                    if (c >= 'a' && c <= 'z')
                        c -= 32;
                    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-')
                        gAPRS_MsgTo[o++] = c;
                }
                gAPRS_MsgTo[o] = 0;
                gAPRS_MsgDirty = true;   // new target -> new {nn on the next Send
            }
            break;
        case MENU_APRS_MSGTXT:
            {
                uint8_t len = 0;
                for (; len < 30 && edit[len] != 0; len++)
                    gAPRS_MsgText[len] = edit[len];
                while (len > 0 && gAPRS_MsgText[len - 1] == ' ')
                    len--;
                gAPRS_MsgText[len] = 0;
                gAPRS_MsgDirty = true;   // new text -> new {nn on the next Send
            }
            break;
        case MENU_APRS_MSGSND:
            if (gSubMenuSelection == 1)
                APRS_SendMessage();
            break;
        case MENU_APRS_TX:
            // Trigger manual transmission (toggle behavior)
            if (gSubMenuSelection == 1) {
                APRS_TransmitNow();
                // Don't reset immediately - let user see TX triggered
                // Menu will exit after transmission
            }
            break;
#endif
        case MENU_F1SHRT:
        case MENU_F1LONG:
        case MENU_F2SHRT:
        case MENU_F2LONG:
        case MENU_MLONG:
            {
                uint8_t * fun[]= {
                    &gEeprom.KEY_1_SHORT_PRESS_ACTION,
                    &gEeprom.KEY_1_LONG_PRESS_ACTION,
                    &gEeprom.KEY_2_SHORT_PRESS_ACTION,
                    &gEeprom.KEY_2_LONG_PRESS_ACTION,
                    &gEeprom.KEY_M_LONG_PRESS_ACTION};
                *fun[UI_MENU_GetCurrentMenuId()-MENU_F1SHRT] = gSubMenu_SIDEFUNCTIONS[gSubMenuSelection].id;
            }
            break;

#ifdef ENABLE_FEAT_F4HWN_SLEEP 
        case MENU_SET_OFF:
            gSetting_set_off = gSubMenuSelection;
            break;
#endif

#ifdef ENABLE_FEAT_F4HWN
        case MENU_SET_PWR:
            gSetting_set_pwr = gSubMenuSelection;
            gRequestSaveChannel = 1;
            break;
        case MENU_SET_PTT:
            gSetting_set_ptt = gSubMenuSelection;
            gSetting_set_ptt_session = gSetting_set_ptt; // Special for action
            break;
        case MENU_SET_TOT:
            gSetting_set_tot = gSubMenuSelection;
            break;
        case MENU_SET_EOT:
            gSetting_set_eot = gSubMenuSelection;
            break;
#ifdef ENABLE_FEAT_F4HWN_CTR
        case MENU_SET_CTR:
            gSetting_set_ctr = gSubMenuSelection;
            break;
#endif
        case MENU_SET_INV:
            gSetting_set_inv = gSubMenuSelection;
            break;
        case MENU_SET_LCK:
            gSetting_set_lck = gSubMenuSelection;
            break;
        case MENU_SET_MET:
            gSetting_set_met = gSubMenuSelection;
            break;
        case MENU_SET_GUI:
            gSetting_set_gui = gSubMenuSelection;
            break;
        #ifdef ENABLE_FEAT_F4HWN_NARROWER
            case MENU_SET_NFM:
                gSetting_set_nfm = gSubMenuSelection;
                RADIO_SetTxParameters();
                RADIO_SetupRegisters(true);
                break;
        #endif
        #ifdef ENABLE_FEAT_F4HWN_VOL
            case MENU_SET_VOL:
                gEeprom.VOLUME_GAIN = gSubMenuSelection;
                break;
        #endif
        #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            case MENU_SET_KEY:
                gEeprom.SET_KEY = gSubMenuSelection;
                break;
        #endif
        case MENU_SET_TMR:
            gSetting_set_tmr = gSubMenuSelection;
            break;
        case MENU_TX_LOCK:
            gTxVfo->TX_LOCK = gSubMenuSelection;
            gRequestSaveChannel       = 1;
            return;
#endif
    }

#ifdef ENABLE_APRS
    switch (UI_MENU_GetCurrentMenuId()) {
#ifdef ENABLE_APRS_DIGI
        case MENU_APRS_DIGI:
#endif
        case MENU_APRS_ON:   case MENU_APRS_INTV: case MENU_APRS_CALL:
        case MENU_APRS_SSID:  case MENU_APRS_LOC: case MENU_APRS_CMNT:
        case MENU_APRS_MSGTO: case MENU_APRS_MSGTXT:
            SETTINGS_SaveAPRS();
            break;
        default:
            break;
    }
#endif

    gRequestSaveSettings = true;
}

static void MENU_ClampSelection(int8_t Direction)
{
    int32_t Min;
    int32_t Max;

    if (!MENU_GetLimits(UI_MENU_GetCurrentMenuId(), &Min, &Max))
    {
        int32_t Selection = gSubMenuSelection;
        if (Selection < Min) Selection = Min;
        else
        if (Selection > Max) Selection = Max;
        gSubMenuSelection = NUMBER_AddWithWraparound(Selection, Direction, Min, Max);
    }
}

void MENU_ShowCurrentSetting(void)
{
    switch (UI_MENU_GetCurrentMenuId())
    {
        case MENU_SQL:
            gSubMenuSelection = gEeprom.SQUELCH_LEVEL;
            break;

        case MENU_STEP:
            gSubMenuSelection = FREQUENCY_GetSortedIdxFromStepIdx(gTxVfo->STEP_SETTING);
            break;

        case MENU_TXP:
            gSubMenuSelection = gTxVfo->OUTPUT_POWER;
            break;

        case MENU_RESET:
            gSubMenuSelection = 0;
            break;

        case MENU_R_DCS:
        case MENU_R_CTCS:
        {
            DCS_CodeType_t type = gTxVfo->freq_config_RX.CodeType;
            uint8_t code = gTxVfo->freq_config_RX.Code;
            int menuid = UI_MENU_GetCurrentMenuId();

            if(gScanUseCssResult) {
                gScanUseCssResult = false;
                type = gScanCssResultType;
                code = gScanCssResultCode;
            }
            if((menuid==MENU_R_CTCS) ^ (type==CODE_TYPE_CONTINUOUS_TONE)) { //not the same type
                gSubMenuSelection = 0;
                break;
            }

            switch (type) {
                case CODE_TYPE_CONTINUOUS_TONE:
                case CODE_TYPE_DIGITAL:
                    gSubMenuSelection = code + 1;
                    break;
                case CODE_TYPE_REVERSE_DIGITAL:
                    gSubMenuSelection = code + 105;
                    break;
                default:
                    gSubMenuSelection = 0;
                    break;
            }
        break;
        }

        case MENU_T_DCS:
            switch (gTxVfo->freq_config_TX.CodeType)
            {
                case CODE_TYPE_DIGITAL:
                    gSubMenuSelection = gTxVfo->freq_config_TX.Code + 1;
                    break;
                case CODE_TYPE_REVERSE_DIGITAL:
                    gSubMenuSelection = gTxVfo->freq_config_TX.Code + 105;
                    break;
                default:
                    gSubMenuSelection = 0;
                    break;
            }
            break;

        case MENU_T_CTCS:
            gSubMenuSelection = (gTxVfo->freq_config_TX.CodeType == CODE_TYPE_CONTINUOUS_TONE) ? gTxVfo->freq_config_TX.Code + 1 : 0;
            break;

        case MENU_SFT_D:
            gSubMenuSelection = gTxVfo->TX_OFFSET_FREQUENCY_DIRECTION;
            break;

        case MENU_OFFSET:
            gSubMenuSelection = gTxVfo->TX_OFFSET_FREQUENCY;
            break;

        case MENU_W_N:
            gSubMenuSelection = gTxVfo->CHANNEL_BANDWIDTH;
            break;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_SCR:
            gSubMenuSelection = gTxVfo->SCRAMBLING_TYPE;
            break;
#endif

        case MENU_BCL:
            gSubMenuSelection = gTxVfo->BUSY_CHANNEL_LOCK;
            break;

        case MENU_MEM_CH:
            #if 0
                gSubMenuSelection = gEeprom.MrChannel[0];
            #else
                gSubMenuSelection = gEeprom.MrChannel[gEeprom.TX_VFO];
            #endif
            break;

        case MENU_MEM_NAME:
            gSubMenuSelection = gEeprom.MrChannel[gEeprom.TX_VFO];
            break;

        case MENU_SAVE:
            gSubMenuSelection = gEeprom.BATTERY_SAVE;
            break;

#ifdef ENABLE_VOX
        case MENU_VOX:
            gSubMenuSelection = gEeprom.VOX_SWITCH ? gEeprom.VOX_LEVEL + 1 : 0;
            break;
#endif

        case MENU_ABR:
            #ifdef ENABLE_FEAT_F4HWN
                if(gBackLight)
                {
                    gSubMenuSelection = gBacklightTimeOriginal;
                }
                else
                {
                    gSubMenuSelection = gEeprom.BACKLIGHT_TIME;
                }
            #else
                gSubMenuSelection = gEeprom.BACKLIGHT_TIME;
            #endif
            break;

        case MENU_ABR_MIN:
            gSubMenuSelection = gEeprom.BACKLIGHT_MIN;
            break;

        case MENU_ABR_MAX:
            gSubMenuSelection = gEeprom.BACKLIGHT_MAX;
            break;

        case MENU_ABR_ON_TX_RX:
            gSubMenuSelection = gSetting_backlight_on_tx_rx;
            break;

        case MENU_TDR:
            gSubMenuSelection = (gEeprom.DUAL_WATCH != DUAL_WATCH_OFF) + (gEeprom.CROSS_BAND_RX_TX != CROSS_BAND_OFF) * 2;
            break;

        case MENU_BEEP:
            gSubMenuSelection = gEeprom.BEEP_CONTROL;
            break;

        case MENU_TOT:
            gSubMenuSelection = gEeprom.TX_TIMEOUT_TIMER;
            break;

#ifdef ENABLE_VOICE
        case MENU_VOICE:
            gSubMenuSelection = gEeprom.VOICE_PROMPT;
            break;
#endif

        case MENU_SC_REV:
            gSubMenuSelection = gEeprom.SCAN_RESUME_MODE;
            break;

        case MENU_MDF:
            gSubMenuSelection = gEeprom.CHANNEL_DISPLAY_MODE;
            break;

        case MENU_AUTOLK:
            gSubMenuSelection = gEeprom.AUTO_KEYPAD_LOCK;
            break;

        case MENU_S_ADD1:
            gSubMenuSelection = gTxVfo->SCANLIST1_PARTICIPATION;
            break;

        case MENU_S_ADD2:
            gSubMenuSelection = gTxVfo->SCANLIST2_PARTICIPATION;
            break;

        case MENU_S_ADD3:
            gSubMenuSelection = gTxVfo->SCANLIST3_PARTICIPATION;
            break;

        case MENU_STE:
            gSubMenuSelection = gEeprom.TAIL_TONE_ELIMINATION;
            break;

        case MENU_RP_STE:
            gSubMenuSelection = gEeprom.REPEATER_TAIL_TONE_ELIMINATION;
            break;

        case MENU_MIC:
            gSubMenuSelection = gEeprom.MIC_SENSITIVITY;
            break;

#ifdef ENABLE_AUDIO_BAR
        case MENU_MIC_BAR:
            gSubMenuSelection = gSetting_mic_bar;
            break;
#endif

        case MENU_COMPAND:
            gSubMenuSelection = gTxVfo->Compander;
            return;

        case MENU_1_CALL:
            gSubMenuSelection = gEeprom.CHAN_1_CALL;
            break;

        case MENU_S_LIST:
            gSubMenuSelection = gEeprom.SCAN_LIST_DEFAULT;
            break;

        case MENU_SLIST1:
        case MENU_SLIST2:
        case MENU_SLIST3:
            gSubMenuSelection = RADIO_FindNextChannel(0, 1, true, UI_MENU_GetCurrentMenuId() - MENU_SLIST1 + 1);
            break;

        #ifdef ENABLE_ALARM
            case MENU_AL_MOD:
                gSubMenuSelection = gEeprom.ALARM_MODE;
                break;
        #endif

        case MENU_D_ST:
            gSubMenuSelection = gEeprom.DTMF_SIDE_TONE;
            break;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_RSP:
            gSubMenuSelection = gEeprom.DTMF_DECODE_RESPONSE;
            break;

        case MENU_D_HOLD:
            gSubMenuSelection = gEeprom.DTMF_auto_reset_time;
            break;
#endif
        case MENU_D_PRE:
            gSubMenuSelection = gEeprom.DTMF_PRELOAD_TIME / 10;
            break;

        case MENU_PTT_ID:
            gSubMenuSelection = gTxVfo->DTMF_PTT_ID_TX_MODE;
            break;

        case MENU_BAT_TXT:
            gSubMenuSelection = gSetting_battery_text;
            return;

#ifdef ENABLE_DTMF_CALLING
        case MENU_D_DCD:
            gSubMenuSelection = gTxVfo->DTMF_DECODING_ENABLE;
            break;

        case MENU_D_LIST:
            gSubMenuSelection = gDTMF_chosen_contact + 1;
            break;
#endif
        case MENU_D_LIVE_DEC:
            gSubMenuSelection = gSetting_live_DTMF_decoder;
            break;

        case MENU_PONMSG:
            gSubMenuSelection = gEeprom.POWER_ON_DISPLAY_MODE;
            break;

        case MENU_ROGER:
            gSubMenuSelection = gEeprom.ROGER;
            break;

        case MENU_AM:
            gSubMenuSelection = gTxVfo->Modulation;
            break;

#ifndef ENABLE_FEAT_F4HWN
    #ifdef ENABLE_AM_FIX
            case MENU_AM_FIX:
                gSubMenuSelection = gSetting_AM_fix;
                break;
    #endif
#endif

#ifdef ENABLE_APRS
        case MENU_APRS_ON:
            gSubMenuSelection = gEeprom.APRS_ON;
            break;
#ifdef ENABLE_APRS_DIGI
        case MENU_APRS_DIGI:
            gSubMenuSelection = gAPRS_DIGI;
            break;
#endif
        case MENU_APRS_INTV:
            gSubMenuSelection = gEeprom.APRS_INTERVAL;
            break;
        case MENU_APRS_CALL:
            strcpy(edit, gEeprom.APRS_CALLSIGN);
            break;
        case MENU_APRS_SSID:
            gSubMenuSelection = gEeprom.APRS_SSID;
            break;
        case MENU_APRS_LOC:
            edit[0] = 0;
            break;
        case MENU_APRS_CMNT:
            strcpy(edit, gEeprom.APRS_COMMENT);
            break;
        case MENU_APRS_MSGTO:
            strcpy(edit, gAPRS_MsgTo);
            break;
        case MENU_APRS_MSGTXT:
            strcpy(edit, gAPRS_MsgText);
            break;
        case MENU_APRS_TX:
        case MENU_APRS_MSGSND:
#ifdef ENABLE_APRS_ACOUSTIC
        case MENU_APRS_ACIN:
#endif
            gSubMenuSelection = 0;  // Always start at OFF
            break;
#endif
        #ifdef ENABLE_NOAA
            case MENU_NOAA_S:
                gSubMenuSelection = gEeprom.NOAA_AUTO_SCAN;
                break;
        #endif

        case MENU_DEL_CH:
            #if 0
                gSubMenuSelection = RADIO_FindNextChannel(gEeprom.MrChannel[0], 1, false, 1);
            #else
                gSubMenuSelection = RADIO_FindNextChannel(gEeprom.MrChannel[gEeprom.TX_VFO], 1, false, 1);
            #endif
            break;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_350TX:
            gSubMenuSelection = gSetting_350TX;
            break;
#endif

        case MENU_F_LOCK:
            gSubMenuSelection = gSetting_F_LOCK;
            break;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_200TX:
            gSubMenuSelection = gSetting_200TX;
            break;

        case MENU_500TX:
            gSubMenuSelection = gSetting_500TX;
            break;

#endif
        case MENU_350EN:
            gSubMenuSelection = gSetting_350EN;
            break;

#ifndef ENABLE_FEAT_F4HWN
        case MENU_SCREN:
            gSubMenuSelection = gSetting_ScrambleEnable;
            break;
#endif

        #ifdef ENABLE_F_CAL_MENU
            case MENU_F_CALI:
                gSubMenuSelection = gEeprom.BK4819_XTAL_FREQ_LOW;
                break;
        #endif

        case MENU_BATCAL:
            gSubMenuSelection = gBatteryCalibration[3];
            break;

        case MENU_BATTYP:
            gSubMenuSelection = gEeprom.BATTERY_TYPE;
            break;

        case MENU_F1SHRT:
        case MENU_F1LONG:
        case MENU_F2SHRT:
        case MENU_F2LONG:
        case MENU_MLONG:
        {
            uint8_t * fun[]= {
                &gEeprom.KEY_1_SHORT_PRESS_ACTION,
                &gEeprom.KEY_1_LONG_PRESS_ACTION,
                &gEeprom.KEY_2_SHORT_PRESS_ACTION,
                &gEeprom.KEY_2_LONG_PRESS_ACTION,
                &gEeprom.KEY_M_LONG_PRESS_ACTION};
            uint8_t id = *fun[UI_MENU_GetCurrentMenuId()-MENU_F1SHRT];

            for(int i = 0; i < gSubMenu_SIDEFUNCTIONS_size; i++) {
                if(gSubMenu_SIDEFUNCTIONS[i].id==id) {
                    gSubMenuSelection = i;
                    break;
                }

            }
            break;
        }

#ifdef ENABLE_FEAT_F4HWN_SLEEP 
        case MENU_SET_OFF:
            gSubMenuSelection = gSetting_set_off;
            break;
#endif

#ifdef ENABLE_FEAT_F4HWN
        case MENU_SET_PWR:
            gSubMenuSelection = gSetting_set_pwr;
            break;
        case MENU_SET_PTT:
            gSubMenuSelection = gSetting_set_ptt_session;
            break;
        case MENU_SET_TOT:
            gSubMenuSelection = gSetting_set_tot;
            break;
        case MENU_SET_EOT:
            gSubMenuSelection = gSetting_set_eot;
            break;
#ifdef ENABLE_FEAT_F4HWN_CTR
        case MENU_SET_CTR:
            gSubMenuSelection = gSetting_set_ctr;
            break;
#endif
        case MENU_SET_INV:
            gSubMenuSelection = gSetting_set_inv;
            break;
        case MENU_SET_LCK:
            gSubMenuSelection = gSetting_set_lck;
            break;
        case MENU_SET_MET:
            gSubMenuSelection = gSetting_set_met;
            break;
        case MENU_SET_GUI:
            gSubMenuSelection = gSetting_set_gui;
            break;
        #ifdef ENABLE_FEAT_F4HWN_NARROWER
            case MENU_SET_NFM:
                gSubMenuSelection = gSetting_set_nfm;
                break;
        #endif
        #ifdef ENABLE_FEAT_F4HWN_VOL
            case MENU_SET_VOL:
                gSubMenuSelection = gEeprom.VOLUME_GAIN;
                break;
        #endif
        #ifdef ENABLE_FEAT_F4HWN_RESCUE_OPS
            case MENU_SET_KEY:
                gSubMenuSelection = gEeprom.SET_KEY;
                break;
        #endif
        case MENU_SET_TMR:
            gSubMenuSelection = gSetting_set_tmr;
            break;
        case MENU_TX_LOCK:
            gSubMenuSelection = gTxVfo->TX_LOCK;
            break;
#endif

        default:
            return;
    }
}

static void MENU_Key_0_to_9(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    uint8_t  Offset;
    int32_t  Min;
    int32_t  Max;
    uint16_t Value = 0;

    if (bKeyHeld || !bKeyPressed)
        return;

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME && edit_index >= 0)
    {   // currently editing the channel name

        if (edit_index < 10)
        {
            if (Key <= KEY_9)
            {
                edit[edit_index] = '0' + Key - KEY_0;

                if (++edit_index >= 10)
                {   // exit edit
                    gFlagAcceptSetting  = false;
                    gAskForConfirmation = 1;
                }

                gRequestDisplayScreen = DISPLAY_MENU;
            }
        }

        return;
    }

#ifdef ENABLE_APRS
    if (gIsInSubMenu && edit_index >= 0 && Key <= KEY_9 &&
        APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), NULL) > 0)
    {
        int dot_pos;
        const int max_len = APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), &dot_pos);

        if (dot_pos >= 0)
        {   // numeric field (Lat/Lon): digits go straight in
            if (edit_index < max_len)
            {
                if (edit_index == dot_pos)
                    edit_index++;
                if (edit_index < max_len)
                {
                    edit[edit_index] = '0' + (Key - KEY_0);
                    if (++edit_index == dot_pos)
                        edit_index++;
                    if (edit_index >= max_len)
                        edit_index = max_len - 1;
                }
            }
        }
        else if (edit_index < max_len)
        {   // text field: two digits = one character (see aprs-location.html)
            const uint8_t d = (uint8_t)(Key - KEY_0);
            if (aprs_pair_first == 0xFF)
            {   // first digit: show it tentatively, don't advance
                aprs_pair_first = d;
                edit[edit_index] = (char)('0' + d);
            }
            else
            {   // second digit completes the character
                const uint8_t v = aprs_pair_first * 10u + d;
                aprs_pair_first = 0xFF;
                if (v < sizeof(APRS_PairCharset) - 1) {
                    edit[edit_index] = APRS_PairCharset[v];
                    if (++edit_index >= max_len)
                        edit_index = max_len - 1;
                }
            }
        }
        gRequestDisplayScreen = DISPLAY_MENU;
        return;
    }
#endif

    INPUTBOX_Append(Key);

    gRequestDisplayScreen = DISPLAY_MENU;

    if (!gIsInSubMenu)
    {
        switch (gInputBoxIndex)
        {
            case 2:
                gInputBoxIndex = 0;

                Value = (gInputBox[0] * 10) + gInputBox[1];

                if (Value > 0 && Value <= gMenuListCount)
                {
                    gMenuCursor         = Value - 1;
                    gFlagRefreshSetting = true;
                    return;
                }

                if (Value <= gMenuListCount)
                    break;

                gInputBox[0]   = gInputBox[1];
                gInputBoxIndex = 1;
                [[fallthrough]];
            case 1:
                Value = gInputBox[0];
                if (Value > 0 && Value <= gMenuListCount)
                {
                    gMenuCursor         = Value - 1;
                    gFlagRefreshSetting = true;
                    return;
                }
                break;
        }

        gInputBoxIndex = 0;

        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_OFFSET)
    {
        uint32_t Frequency;

        if (gInputBoxIndex < 6)
        {   // invalid frequency
            #ifdef ENABLE_VOICE
                gAnotherVoiceID = (VOICE_ID_t)Key;
            #endif
            return;
        }

        #ifdef ENABLE_VOICE
            gAnotherVoiceID = (VOICE_ID_t)Key;
        #endif

        Frequency = StrToUL(INPUTBOX_GetAscii())*100;
        gSubMenuSelection = FREQUENCY_RoundToStep(Frequency, gTxVfo->StepFrequency);

        gInputBoxIndex = 0;
        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_CH ||
        UI_MENU_GetCurrentMenuId() == MENU_DEL_CH ||
        UI_MENU_GetCurrentMenuId() == MENU_1_CALL ||
        UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME)
    {   // enter 3-digit channel number

        if (gInputBoxIndex < 3)
        {
            #ifdef ENABLE_VOICE
                gAnotherVoiceID   = (VOICE_ID_t)Key;
            #endif
            gRequestDisplayScreen = DISPLAY_MENU;
            return;
        }

        gInputBoxIndex = 0;

        Value = ((gInputBox[0] * 100) + (gInputBox[1] * 10) + gInputBox[2]) - 1;

        if (IS_MR_CHANNEL(Value))
        {
            #ifdef ENABLE_VOICE
                gAnotherVoiceID = (VOICE_ID_t)Key;
            #endif
            gSubMenuSelection = Value;
            return;
        }

        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }

    if (MENU_GetLimits(UI_MENU_GetCurrentMenuId(), &Min, &Max))
    {
        gInputBoxIndex = 0;
        gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
        return;
    }

    Offset = (Max >= 100) ? 3 : (Max >= 10) ? 2 : 1;

    /*
    switch (gInputBoxIndex)
    {
        case 1:
            Value = gInputBox[0];
            break;
        case 2:
            Value = (gInputBox[0] *  10) + gInputBox[1];
            break;
        case 3:
            Value = (gInputBox[0] * 100) + (gInputBox[1] * 10) + gInputBox[2];
            break;
    }
    */

    for (uint8_t i = 0; i < gInputBoxIndex; i++) {
        Value = (Value * 10) + gInputBox[i];
    }

    if (Offset == gInputBoxIndex)
        gInputBoxIndex = 0;

    if (Value <= Max)
    {
        gSubMenuSelection = Value;
        return;
    }

    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
}

static void MENU_Key_EXIT(bool bKeyPressed, bool bKeyHeld)
{
    if (bKeyHeld || !bKeyPressed)
        return;

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

#ifdef ENABLE_APRS
    // in a text field EXIT = backspace; at position 0 it cancels as usual
    if (gIsInSubMenu && edit_index > 0) {
        int dot_pos;
        const int max_len = APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), &dot_pos);
        if (max_len > 0 && dot_pos < 0) {
            edit[--edit_index] = (max_len == 9) ? '_' : ' ';
            aprs_pair_first = 0xFF;
            gRequestDisplayScreen = DISPLAY_MENU;
            return;
        }
    }
#endif

    if (!gCssBackgroundScan)
    {
        /* Backlight related menus set full brightness. Set it back to the configured value,
           just in case we are exiting from one of them. */
        BACKLIGHT_TurnOn();

        if (gIsInSubMenu)
        {
            if (gInputBoxIndex == 0 || UI_MENU_GetCurrentMenuId() != MENU_OFFSET)
            {
                gAskForConfirmation = 0;
                gIsInSubMenu        = false;
                gInputBoxIndex      = 0;
                gFlagRefreshSetting = true;

                #ifdef ENABLE_VOICE
                    gAnotherVoiceID = VOICE_ID_CANCEL;
                #endif
            }
            else
                gInputBox[--gInputBoxIndex] = 10;

            // ***********************

            gRequestDisplayScreen = DISPLAY_MENU;
            return;
        }

        #ifdef ENABLE_VOICE
            gAnotherVoiceID = VOICE_ID_CANCEL;
        #endif

        gRequestDisplayScreen = DISPLAY_MAIN;

        if (gEeprom.BACKLIGHT_TIME == 0) // backlight set to always off
        {
            BACKLIGHT_TurnOff();    // turn the backlight OFF
        }
    }
    else
    {
        MENU_StopCssScan();

        #ifdef ENABLE_VOICE
            gAnotherVoiceID   = VOICE_ID_SCANNING_STOP;
        #endif

        gRequestDisplayScreen = DISPLAY_MENU;
    }

    gPttWasReleased = true;
}

static void MENU_Key_MENU(const bool bKeyPressed, const bool bKeyHeld)
{
    if (bKeyHeld || !bKeyPressed)
        return;

    gBeepToPlay           = BEEP_1KHZ_60MS_OPTIONAL;
    gRequestDisplayScreen = DISPLAY_MENU;

    if (!gIsInSubMenu)
    {
        #ifdef ENABLE_VOICE
            if (UI_MENU_GetCurrentMenuId() != MENU_SCR)
                gAnotherVoiceID = MenuList[gMenuCursor].voice_id;
        #endif
        if (UI_MENU_GetCurrentMenuId() == MENU_UPCODE 
            || UI_MENU_GetCurrentMenuId() == MENU_DWCODE 
#ifdef ENABLE_DTMF_CALLING 
            || UI_MENU_GetCurrentMenuId() == MENU_ANI_ID
#endif
            )
            return;
        #if 1
            if (UI_MENU_GetCurrentMenuId() == MENU_DEL_CH || UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME)
                if (!RADIO_CheckValidChannel(gSubMenuSelection, false, 0))
                    return;  // invalid channel
        #endif

#ifdef ENABLE_APRS
        // BEACON and Send are one-shot actions, not settings: fire them on this
        // MENU press instead of opening a submenu and demanding an OFF->ON step
        // (the menu shows "SEND BEACON" / "SEND MESSAGE" as the value, so what
        // the key does is visible before it is pressed).
        if (UI_MENU_GetCurrentMenuId() == MENU_APRS_TX) {
            APRS_TransmitNow();
            return;
        }
        if (UI_MENU_GetCurrentMenuId() == MENU_APRS_MSGSND) {
            APRS_SendMessage();
            return;
        }
#ifdef ENABLE_APRS_ACOUSTIC
        // Blocking listen: the radio is deaf and unresponsive for up to 15 s,
        // which is why it is an explicit action rather than a background mode.
        if (UI_MENU_GetCurrentMenuId() == MENU_APRS_ACIN) {
            int32_t la = 0, lo = 0;
            if (APRS_AcousticReceive(&la, &lo)) {
                gEeprom.APRS_LATITUDE  = la;
                gEeprom.APRS_LONGITUDE = lo;
                SETTINGS_SaveAPRS();
                AUDIO_PlayBeep(BEEP_880HZ_60MS_DOUBLE_BEEP);
            } else {
                AUDIO_PlayBeep(BEEP_500HZ_60MS_DOUBLE_BEEP);
            }
            gUpdateDisplay = true;
            return;
        }
#endif
#endif

        gAskForConfirmation = 0;
        gIsInSubMenu        = true;

//      if (UI_MENU_GetCurrentMenuId() != MENU_D_LIST)
        {
            gInputBoxIndex      = 0;
            edit_index          = -1;
        }

        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME)
    {
        if (edit_index < 0)
        {   // enter channel name edit mode
            if (!RADIO_CheckValidChannel(gSubMenuSelection, false, 0))
                return;

            SETTINGS_FetchChannelName(edit, gSubMenuSelection);

            // pad the channel name out with '_'
            edit_index = strlen(edit);
            while (edit_index < 10)
                edit[edit_index++] = '_';
            edit[edit_index] = 0;
            edit_index = 0;  // 'edit_index' is going to be used as the cursor position

            // make a copy so we can test for change when exiting the menu item
            memcpy(edit_original, edit, sizeof(edit_original));

            return;
        }
        else
        if (edit_index >= 0 && edit_index < 10)
        {   // editing the channel name characters

            if (++edit_index < 10)
                return; // next char

            // exit
            gFlagAcceptSetting  = false;
            gAskForConfirmation = 0;
            if (memcmp(edit_original, edit, sizeof(edit_original)) == 0) {
                // no change - drop it
                gIsInSubMenu = false;
            }
        }
    }

#ifdef ENABLE_APRS
    // APRS string field editing
    if (APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), NULL) > 0)
    {
        if (edit_index < 0)
        {   // enter APRS string edit mode
            memset(edit, 0, sizeof(edit));
            // Load current value into edit buffer
            switch (UI_MENU_GetCurrentMenuId())
            {
                case MENU_APRS_CALL:
                    strcpy(edit, gEeprom.APRS_CALLSIGN);
                    edit_index = strlen(edit);
                    while (edit_index < 9)
                        edit[edit_index++] = '_';
                    break;
                case MENU_APRS_LOC:
                    for (edit_index = 0; edit_index < 15; edit_index++)
                        edit[edit_index] = '0';
                    break;
                case MENU_APRS_CMNT:
                    strcpy(edit, gEeprom.APRS_COMMENT);
                    edit_index = strlen(edit);
                    while (edit_index < 31)
                        edit[edit_index++] = ' ';
                    break;
                case MENU_APRS_MSGTO:
                    strcpy(edit, gAPRS_MsgTo);
                    edit_index = strlen(edit);
                    while (edit_index < 9)
                        edit[edit_index++] = '_';
                    break;
                case MENU_APRS_MSGTXT:
                    strcpy(edit, gAPRS_MsgText);
                    edit_index = strlen(edit);
                    while (edit_index < 31)
                        edit[edit_index++] = ' ';
                    break;
            }
            edit[edit_index] = 0;
            edit_index = 0;  // cursor position
            aprs_pair_first = 0xFF;

            // Save original for comparison
            memcpy(edit_original, edit, sizeof(edit_original));

            return;
        }
        else
        {   // editing APRS string fields: MENU saves whatever has been typed.
            // It used to step the cursor one character per press and only save
            // once the cursor reached the end of the field, so committing a
            // two-character message meant ~29 further MENU presses and looked
            // like a hang (reported in ta1js#1). EXIT still backspaces.
            if (aprs_pair_first != 0xFF && edit_index >= 0)
            {   // drop a half-typed digit pair instead of storing the digit
                const int len = APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), NULL);
                edit[edit_index] = (len == 9) ? '_' : ' ';
                aprs_pair_first  = 0xFF;
            }
            gFlagAcceptSetting  = true;
            gAskForConfirmation = 0;
            gIsInSubMenu        = false;
            edit_index          = -1;
            return;
        }
    }
#endif

    // exiting the sub menu

    if (gIsInSubMenu)
    {
        if (UI_MENU_GetCurrentMenuId() == MENU_RESET  ||
            UI_MENU_GetCurrentMenuId() == MENU_MEM_CH ||
            UI_MENU_GetCurrentMenuId() == MENU_DEL_CH ||
            UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME
#ifdef ENABLE_APRS
            || APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), NULL) > 0
#endif
            )
        {
            switch (gAskForConfirmation)
            {
                case 0:
                    gAskForConfirmation = 1;
                    break;

                case 1:
                    gAskForConfirmation = 2;

                    UI_DisplayMenu();

                    if (UI_MENU_GetCurrentMenuId() == MENU_RESET)
                    {
                        #ifdef ENABLE_VOICE
                            AUDIO_SetVoiceID(0, VOICE_ID_CONFIRM);
                            AUDIO_PlaySingleVoice(true);
                        #endif

                        MENU_AcceptSetting();

                        #if defined(ENABLE_OVERLAY)
                            overlay_FLASH_RebootToBootloader();
                        #else
                            NVIC_SystemReset();
                        #endif
                    }

                    gFlagAcceptSetting  = true;
                    gIsInSubMenu        = false;
                    gAskForConfirmation = 0;
            }
        }
        else
        {
            gFlagAcceptSetting = true;
            gIsInSubMenu       = false;
        }
    }

    SCANNER_Stop();

    #ifdef ENABLE_VOICE
        if (UI_MENU_GetCurrentMenuId() == MENU_SCR)
            gAnotherVoiceID = (gSubMenuSelection == 0) ? VOICE_ID_SCRAMBLER_OFF : VOICE_ID_SCRAMBLER_ON;
        else
            gAnotherVoiceID = VOICE_ID_CONFIRM;
    #endif

    gInputBoxIndex = 0;
}

static void MENU_Key_STAR(const bool bKeyPressed, const bool bKeyHeld)
{
    if (bKeyHeld || !bKeyPressed)
        return;

    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME && edit_index >= 0)
    {   // currently editing the channel name

        if (edit_index < 10)
        {
            edit[edit_index] = '-';

            if (++edit_index >= 10)
            {   // exit edit
                gFlagAcceptSetting  = false;
                gAskForConfirmation = 1;
            }

            gRequestDisplayScreen = DISPLAY_MENU;
        }

        return;
    }

#ifdef ENABLE_APRS
    if (gIsInSubMenu && edit_index >= 0 &&
        (UI_MENU_GetCurrentMenuId() == MENU_APRS_CALL ||
         UI_MENU_GetCurrentMenuId() == MENU_APRS_MSGTO ||
         UI_MENU_GetCurrentMenuId() == MENU_APRS_MSGTXT ||
         UI_MENU_GetCurrentMenuId() == MENU_APRS_CMNT))
    {
        const int menu_id = UI_MENU_GetCurrentMenuId();
        const int max_len = APRS_EditMaxLen(menu_id, NULL);

        if (edit_index < max_len)
        {
            edit[edit_index] = (max_len == 31) ? '-' : ' ';
            if (++edit_index >= max_len)
                edit_index = max_len - 1;
            gRequestDisplayScreen = DISPLAY_MENU;
        }
        return;
    }
#endif

    RADIO_SelectVfos();

    #ifdef ENABLE_NOAA
        if (!IS_NOAA_CHANNEL(gRxVfo->CHANNEL_SAVE) && gRxVfo->Modulation == MODULATION_FM)
    #else
        if (gRxVfo->Modulation ==  MODULATION_FM)
    #endif
    {
        if ((UI_MENU_GetCurrentMenuId() == MENU_R_CTCS || UI_MENU_GetCurrentMenuId() == MENU_R_DCS) && gIsInSubMenu)
        {   // scan CTCSS or DCS to find the tone/code of the incoming signal
            if (!SCANNER_IsScanning())
                MENU_StartCssScan();
            else
                MENU_StopCssScan();
        }

        gPttWasReleased = true;
        return;
    }

    gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
}

static void MENU_Key_UP_DOWN(bool bKeyPressed, bool bKeyHeld, int8_t Direction)
{
    uint8_t VFO;
    uint8_t Channel;
    bool    bCheckScanList;

    if (UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME && gIsInSubMenu && edit_index >= 0)
    {   // change the character
        if (bKeyPressed && edit_index < 10 && Direction != 0)
        {
            const char   unwanted[] = "$%&!\"':;?^`|{}";
            char         c          = edit[edit_index] + Direction;
            unsigned int i          = 0;
            while (i < sizeof(unwanted) && c >= 32 && c <= 126)
            {
                if (c == unwanted[i++])
                {   // choose next character
                    c += Direction;
                    i = 0;
                }
            }
            edit[edit_index] = (c < 32) ? 126 : (c > 126) ? 32 : c;

            gRequestDisplayScreen = DISPLAY_MENU;
        }
        return;
    }

#ifdef ENABLE_APRS
    if (gIsInSubMenu && edit_index >= 0 && Direction != 0 &&
        APRS_EditMaxLen(UI_MENU_GetCurrentMenuId(), NULL) > 0)
    {
        const int menu_id = UI_MENU_GetCurrentMenuId();
        int dot_pos;
        const int max_len = APRS_EditMaxLen(menu_id, &dot_pos);

        if (!bKeyPressed || edit_index >= max_len)
            return;

        if (edit_index == dot_pos) {
            // Auto-skip the dot.
            edit_index++;
            if (edit_index >= max_len)
                return;
        }

        if (dot_pos >= 0)
        {   // numeric fields: change digit with wrap-around
            char c = edit[edit_index];
            int digit = (c >= '0' && c <= '9') ? (c - '0') : 0;
            const int max_digit = 9;

            digit += Direction;
            if (digit < 0) digit = max_digit;
            if (digit > max_digit) digit = 0;
            edit[edit_index] = '0' + digit;
        }
        else if (menu_id == MENU_APRS_CALL || menu_id == MENU_APRS_MSGTO)
        {   // callsign: cycle through a restricted charset
            const char *allowed = " _0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            char c = edit[edit_index];
            if (c == 0) c = ' ';
            if (c >= 'a' && c <= 'z') c -= 32;
            if (c == '_') c = ' ';

            int idx = 0;
            while (allowed[idx] && allowed[idx] != c) idx++;
            if (!allowed[idx]) idx = 0;
            idx += Direction;
            const int len = (int)strlen(allowed);
            if (idx < 0) idx = len - 1;
            if (idx >= len) idx = 0;
            edit[edit_index] = allowed[idx];
        }
        else
        {   // comment: general ASCII cycling
            char c = edit[edit_index];
            if (c == 0) c = ' ';
            c = (char)(c + Direction);
            if (c < 32) c = 126;
            if (c > 126) c = 32;
            edit[edit_index] = c;
        }

        gRequestDisplayScreen = DISPLAY_MENU;
        return;
    }
#endif

    if (!bKeyHeld)
    {
        if (!bKeyPressed)
            return;

        gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;

        gInputBoxIndex = 0;
    }
    else
    if (!bKeyPressed)
        return;

    if (SCANNER_IsScanning()) {
        return;
    }

    if (!gIsInSubMenu)
    {
        gMenuCursor = NUMBER_AddWithWraparound(gMenuCursor, -Direction, 0, gMenuListCount - 1);

        gFlagRefreshSetting = true;

        gRequestDisplayScreen = DISPLAY_MENU;

        if (UI_MENU_GetCurrentMenuId() != MENU_ABR
            && UI_MENU_GetCurrentMenuId() != MENU_ABR_MIN
            && UI_MENU_GetCurrentMenuId() != MENU_ABR_MAX
            && gEeprom.BACKLIGHT_TIME == 0) // backlight always off and not in the backlight menu
        {
            BACKLIGHT_TurnOff();
        }

        return;
    }

    if (UI_MENU_GetCurrentMenuId() == MENU_OFFSET)
    {
        int32_t Offset = (Direction * gTxVfo->StepFrequency) + gSubMenuSelection;
        if (Offset < 99999990)
        {
            if (Offset < 0)
                Offset = 99999990;
        }
        else
            Offset = 0;

        gSubMenuSelection     = FREQUENCY_RoundToStep(Offset, gTxVfo->StepFrequency);
        gRequestDisplayScreen = DISPLAY_MENU;
        return;
    }

    VFO = 0;

    switch (UI_MENU_GetCurrentMenuId())
    {
        case MENU_DEL_CH:
        case MENU_1_CALL:
        case MENU_MEM_NAME:
            bCheckScanList = false;
            break;

        case MENU_SLIST3:
            bCheckScanList = true;
            VFO = 3;
            break;
        case MENU_SLIST2:
            bCheckScanList = true;
            VFO = 2;
            break;
        case MENU_SLIST1:
            bCheckScanList = true;
            VFO = 1;
            break;

        default:
            MENU_ClampSelection(Direction);
            gRequestDisplayScreen = DISPLAY_MENU;
            return;
    }

    Channel = RADIO_FindNextChannel(gSubMenuSelection + Direction, Direction, bCheckScanList, VFO);
    if (Channel != 0xFF)
        gSubMenuSelection = Channel;

    gRequestDisplayScreen = DISPLAY_MENU;
}

void MENU_ProcessKeys(KEY_Code_t Key, bool bKeyPressed, bool bKeyHeld)
{
    switch (Key)
    {
        case KEY_0:
        case KEY_1:
        case KEY_2:
        case KEY_3:
        case KEY_4:
        case KEY_5:
        case KEY_6:
        case KEY_7:
        case KEY_8:
        case KEY_9:
            MENU_Key_0_to_9(Key, bKeyPressed, bKeyHeld);
            break;
        case KEY_MENU:
            MENU_Key_MENU(bKeyPressed, bKeyHeld);
            break;
        case KEY_UP:
            MENU_Key_UP_DOWN(bKeyPressed, bKeyHeld,  1);
            break;
        case KEY_DOWN:
            MENU_Key_UP_DOWN(bKeyPressed, bKeyHeld, -1);
            break;
        case KEY_EXIT:
            MENU_Key_EXIT(bKeyPressed, bKeyHeld);
            break;
        case KEY_STAR:
            MENU_Key_STAR(bKeyPressed, bKeyHeld);
            break;
        case KEY_F:
            if ((UI_MENU_GetCurrentMenuId() == MENU_MEM_NAME
#ifdef ENABLE_APRS
                 || UI_MENU_GetCurrentMenuId() == MENU_APRS_CALL
                 || UI_MENU_GetCurrentMenuId() == MENU_APRS_CMNT
#endif
                ) && edit_index >= 0)
            {   // currently editing the channel name
                if (!bKeyHeld && bKeyPressed)
                {
                    gBeepToPlay = BEEP_1KHZ_60MS_OPTIONAL;
                    int max_len = 10;
#ifdef ENABLE_APRS
                    if (UI_MENU_GetCurrentMenuId() == MENU_APRS_CALL) max_len = 9;
                    else if (UI_MENU_GetCurrentMenuId() == MENU_APRS_CMNT) max_len = 31;
#endif
                    if (edit_index < max_len)
                    {
                        edit[edit_index] = ' ';
                        if (++edit_index >= max_len)
                            edit_index = max_len - 1;
                        gRequestDisplayScreen = DISPLAY_MENU;
                    }
                }
                break;
            }

            GENERIC_Key_F(bKeyPressed, bKeyHeld);
            break;
        case KEY_PTT:
            GENERIC_Key_PTT(bKeyPressed);
            break;
        default:
            if (!bKeyHeld && bKeyPressed)
                gBeepToPlay = BEEP_500HZ_60MS_DOUBLE_BEEP_OPTIONAL;
            break;
    }

    if (gScreenToDisplay == DISPLAY_MENU)
    {
        if (UI_MENU_GetCurrentMenuId() == MENU_VOL ||
            #ifdef ENABLE_F_CAL_MENU
                UI_MENU_GetCurrentMenuId() == MENU_F_CALI ||
            #endif
            UI_MENU_GetCurrentMenuId() == MENU_BATCAL)
        {
            gMenuCountdown = menu_timeout_long_500ms;
        }
        else
        {
            gMenuCountdown = menu_timeout_500ms;
        }
    }
}
