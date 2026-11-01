/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

static inline int rtw89_bssid_valid(const unsigned char bssid[6])
{
    unsigned char any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= bssid[i];
    return any && !(bssid[0] & 1);
}

static inline int rtw89_parse_bssid(const char *text, unsigned char out[6])
{
    unsigned char bytes[6];
    if (!text)
        return 0;
    for (unsigned i = 0; i < 6; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char)*text++;
            if (!c)
                return 0;
            unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                             c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                             c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (digit == 16)
                return 0;
            value = value * 16 + digit;
        }
        bytes[i] = (unsigned char)value;
        if (i < 5 && *text++ != ':')
            return 0;
    }
    if (*text || !rtw89_bssid_valid(bytes))
        return 0;
    for (unsigned i = 0; i < 6; ++i)
        out[i] = bytes[i];
    return 1;
}
