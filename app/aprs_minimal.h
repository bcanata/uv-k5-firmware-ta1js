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

#ifndef APP_APRS_MINIMAL_H
#define APP_APRS_MINIMAL_H

#include <stdbool.h>
#include <stdint.h>

// APRS configuration - simplified to use integer coordinates
// Latitude/Longitude stored as integer degrees * 1000000 (millionths)
// Example: 40.7128° = 40712800

#define APRS_MAX_PACKET_SIZE      256
#define APRS_MAX_CALLSIGN_LEN     9

// Initialize APRS system
void APRS_Init(void);

// Manual transmission trigger
void APRS_TransmitNow(void);

// (Re)start the automatic beacon interval timer (call when APRS/Intv changes)
void APRS_ResetBeaconTimer(void);

// Beacon a specific position in micro-degrees (UART 0x0704 / live GPS)
void APRS_BeaconAt(int32_t lat_udeg, int32_t lon_udeg);

// Main APRS task (called from app loop)
void APRS_Task(void);

// Check if APRS is currently transmitting
bool APRS_IsTransmitting(void);

// True once the operator has set a real callsign (not empty / not N0CALL);
// transmit paths refuse to key up until this is true.
bool APRS_Configured(void);

// RX listen mode (active while the APRS menu item is ON)
void APRS_HandleRxInterrupts(uint16_t interrupt_bits);
void APRS_StopListening(void);

// Messaging
void APRS_SendMessage(void);
bool APRS_DismissMessage(void);

// "41.15N 27.84E" for the Loc menu display
void APRS_FormatLatLon(char *out);
extern char gAPRS_MsgTo[10];
extern bool gAPRS_MsgDirty;
extern char gAPRS_MsgText[31];

// Last decoded packet, shown on the main screen while non-empty
extern char    gAPRS_RxDisplay[44];
extern uint8_t gAPRS_RxDisplayTimer;
extern bool    gAPRS_RxSticky;

#endif // APP_APRS_MINIMAL_H
