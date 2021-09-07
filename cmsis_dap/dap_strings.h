/**
 * @file    dap_strings.h
 *
 * DAPLink Interface Firmware
 * Copyright (c) 2019, ARM Limited, All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <esp_mac.h>

/** Get Vendor ID string.
\param str Pointer to buffer to store the string.
\return String length.
*/
static inline uint8_t DAP_GetVendorString (char *str) {
  (void)str;
  return (0U);
}

/** Get Product ID string.
\param str Pointer to buffer to store the string.
\return String length.
*/
static inline uint8_t DAP_GetProductString (char *str) {
  (void)str;
  return (0U);
}

/** Get Serial Number string.
\param str Pointer to buffer to store the string.
\return String length.
*/
static inline uint8_t DAP_GetSerNumString (char *str) {
    char data[7] = { 0 };
    esp_efuse_mac_get_default((uint8_t *)data);
    uint8_t length = (uint8_t)strlen(data) + 1;
    memcpy(str, data, sizeof(data));
    return length;
}

/** Get firmware version string.
\param str Pointer to buffer to store the string.
\return String length.
*/
static inline uint8_t DAP_ProductFirmwareVerString (char *str) {
    const char * data = "2.0.0";
    uint8_t length = (uint8_t)strlen(data) + 1;
    memcpy(str, data, length);
    return length;
}
