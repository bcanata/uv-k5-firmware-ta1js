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

// Main APRS task (called from app loop)
void APRS_Task(void);

// Check if APRS is currently transmitting
bool APRS_IsTransmitting(void);

#endif // APP_APRS_MINIMAL_H
