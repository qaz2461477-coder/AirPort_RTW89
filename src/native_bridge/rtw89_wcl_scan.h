/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tahoe's native scan, as bytes: the request macOS hands the driver and the
 * beacon message it wants back. Plain C with no kernel or Apple types, so that
 * tools/hosttest/bridge_test.c can throw malformed requests at it.
 *
 * Offsets and message numbers adapted from AirportRTW WCL/WCL.hpp, laobamac,
 * copyright 2026, GPL-2.0, pinned source 82c78d0b1c300b25a6706c433170c1c635293670.
 * They are observed Apple private layouts, not documented ones.
 */
#ifndef RTW89_WCL_SCAN_H
#define RTW89_WCL_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rtw89_glue.h"

/* sizeof(apple80211ScanRequest): the family passes this much. */
#define RTW89_WCL_SCAN_REQUEST_SIZE 0x1550
#define RTW89_WCL_SCAN_MAX_CHANNELS 400

/* Native messages, carried by IO80211Controller::postMessage(). */
#define RTW89_WCL_MSG_SCAN_RESULT   201
#define RTW89_WCL_MSG_SCAN_DONE     237

#define RTW89_WCL_BEACON_SSID_PRESENT (1u << 1)
#define RTW89_WCL_BEACON_RSSI_VALID   (1u << 14)

#define RTW89_WCL_CHANNEL_FLAG_2GHZ 0x8u
#define RTW89_WCL_CHANNEL_FLAG_5GHZ 0x10u
#define RTW89_WCL_CHANNEL_FLAG_6GHZ 0x2000u
#define RTW89_WCL_MAX_CHANNEL       196

#ifdef __cplusplus
extern "C" {
#define RTW89_WCL_ASSERT static_assert
#else
#define RTW89_WCL_ASSERT _Static_assert
#endif

struct rtw89_wcl_scan {
    uint32_t identifier;
    uint32_t type;                  /* 2: passive */
    uint32_t ssid_len;
    uint32_t channel_count;         /* as asked for: 0 means every channel */
    uint8_t  ssid[32];
    uint8_t  channels[32];          /* bit per channel number */
};

/* The header of one result; the beacon's elements follow it. */
struct rtw89_wcl_beacon {
    uint32_t ie_length;
    uint16_t channel_spec;
    uint8_t  ssid[32];
    uint8_t  ssid_length;
    uint8_t  channel;
    uint8_t  reserved0;
    uint8_t  bssid[6];
    uint8_t  reserved1;
    int32_t  rssi;
    int16_t  noise;
    int16_t  snr;
    uint16_t interval;
    uint16_t capability;
    uint32_t reserved2;
    uint32_t flags;
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_beacon) == 68, "native beacon size");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_beacon, rssi) == 48, "native beacon rssi offset");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_beacon, flags) == 64, "native beacon flags offset");

static inline uint32_t rtw89_wcl_read32(const uint8_t *p)
{
    uint32_t value;

    memcpy(&value, p, sizeof(value));
    return value;
}

/*
 * @buffer, @length: the request. False if it cannot be one: nothing past @length
 * is read, and the lengths inside it are not trusted.
 */
static inline bool rtw89_wcl_decode_scan(const void *buffer, size_t length,
                                         struct rtw89_wcl_scan *out)
{
    const uint8_t *p = (const uint8_t *)buffer;
    uint32_t count, ssid_len, type, i;

    if (!buffer || !out || length < 0x58)
        return false;
    count = rtw89_wcl_read32(p + 0x54);
    ssid_len = rtw89_wcl_read32(p + 0x1c);
    type = rtw89_wcl_read32(p + 0x40);
    if (count > RTW89_WCL_SCAN_MAX_CHANNELS || ssid_len > 32 || type < 1 || type > 4 ||
        length < 0x58 + (size_t)count * 12)
        return false;

    memset(out, 0, sizeof(*out));
    out->identifier = rtw89_wcl_read32(p);
    out->type = type;
    out->ssid_len = ssid_len;
    out->channel_count = count;
    memcpy(out->ssid, p + 0x20, ssid_len);
    /* Each entry is an apple80211_channel: version, channel, flags. */
    for (i = 0; i < count; i++) {
        const uint8_t *entry = p + 0x58 + (size_t)i * 12;
        uint32_t channel = rtw89_wcl_read32(entry + 4), flags = rtw89_wcl_read32(entry + 8);

        /* 6 GHz channel numbers overlap the 2.4 GHz ones. */
        if (!channel || channel > RTW89_WCL_MAX_CHANNEL || (flags & RTW89_WCL_CHANNEL_FLAG_6GHZ))
            continue;
        if (((flags & RTW89_WCL_CHANNEL_FLAG_2GHZ) && channel > 14) ||
            ((flags & RTW89_WCL_CHANNEL_FLAG_5GHZ) && channel <= 14))
            continue;
        out->channels[channel / 8] |= (uint8_t)(1u << (channel % 8));
    }
    return true;
}

/* Whether @scan can cover anything: a list of only 6 GHz channels cannot. */
static inline bool rtw89_wcl_scan_usable(const struct rtw89_wcl_scan *scan)
{
    uint8_t any = 0;

    if (!scan->channel_count)
        return true;
    for (size_t i = 0; i < sizeof(scan->channels); i++)
        any |= scan->channels[i];
    return any != 0;
}

/* Whether @bss answers @scan. The hardware scans everything; this narrows it. */
static inline bool rtw89_wcl_scan_wants(const struct rtw89_wcl_scan *scan,
                                        const struct rtw89_glue_bss *bss)
{
    if (scan->channel_count &&
        !(scan->channels[bss->channel / 8] & (1u << (bss->channel % 8))))
        return false;
    if (scan->type != 2 && scan->ssid_len &&
        (scan->ssid_len != bss->ssid_len || memcmp(scan->ssid, bss->ssid, scan->ssid_len)))
        return false;
    return true;
}

/* Fill @out with one result. @out's elements are the caller's already, at
 * @out + 1, @ies_len bytes. False if the network cannot be described. */
static inline bool rtw89_wcl_beacon_fill(struct rtw89_wcl_beacon *out,
                                         const struct rtw89_glue_bss *bss,
                                         uint16_t interval, size_t ies_len)
{
    if (!bss->channel || bss->channel > RTW89_WCL_MAX_CHANNEL || bss->ssid_len > 32 ||
        bss->freq >= 5925)
        return false;
    memset(out, 0, sizeof(*out));
    out->ie_length = (uint32_t)ies_len;
    out->channel_spec = (uint16_t)(bss->channel | (bss->freq < 3000 ? 0x1000 : 0xd000));
    out->channel = bss->channel;
    out->ssid_length = bss->ssid_len;
    memcpy(out->ssid, bss->ssid, bss->ssid_len);
    memcpy(out->bssid, bss->bssid, sizeof(out->bssid));
    out->rssi = bss->signal;
    out->interval = interval;
    out->capability = bss->capability;
    out->flags = RTW89_WCL_BEACON_SSID_PRESENT | RTW89_WCL_BEACON_RSSI_VALID;
    return true;
}

/* An apple80211_channel's flags for one channel of the card. */
static inline uint32_t rtw89_wcl_channel_flags(const struct rtw89_glue_channel *channel)
{
    /* APPLE80211_C_FLAG_20MHZ, 2GHZ or 5GHZ, ACTIVE, DFS */
    return 0x2u | (channel->freq < 3000 ? RTW89_WCL_CHANNEL_FLAG_2GHZ : RTW89_WCL_CHANNEL_FLAG_5GHZ) |
           (channel->passive ? 0 : 0x80u) | (channel->radar ? 0x100u : 0);
}

#ifdef __cplusplus
}
#endif
#endif
