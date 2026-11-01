/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Tahoe's native join and what macOS asks about a joined network, as bytes.
 * Plain C with no kernel or Apple types, so that tools/hosttest/bridge_test.c can
 * throw malformed requests at it.
 *
 * Offsets, message numbers and sizes adapted from AirportRTW WCL/WCL.hpp,
 * laobamac, copyright 2026, GPL-2.0, pinned source
 * 82c78d0b1c300b25a6706c433170c1c635293670. They are observed Apple private
 * layouts, not documented ones.
 */
#ifndef RTW89_WCL_JOIN_H
#define RTW89_WCL_JOIN_H

#include "rtw89_wcl_scan.h"

/* sizeof(apple80211AssocCandidates): the family passes this much. */
#define RTW89_WCL_JOIN_REQUEST_SIZE 0x2cc
#define RTW89_WCL_JOIN_MAX_CANDIDATES 10

#define RTW89_WCL_MSG_LQM           39
#define RTW89_WCL_MSG_RESET_IFACE   49
#define RTW89_WCL_MSG_DRIVER        55
#define RTW89_WCL_MSG_DEAUTH        203
#define RTW89_WCL_MSG_ASSOC         211
#define RTW89_WCL_MSG_FIRST_BEACON  212
#define RTW89_WCL_MSG_CONNECT       213
#define RTW89_WCL_MSG_JOIN_ABORTED  214
#define RTW89_WCL_MSG_LINK          216

/* apple80211_authtype_upper, and the key's apple80211 cipher types */
#define RTW89_WCL_AUTH_WPA_PSK      (1u << 1)
#define RTW89_WCL_AUTH_WPA2_PSK     (1u << 3)
#define RTW89_WCL_CIPHER_PMK        6
#define RTW89_WCL_CIPHER_PMKSA      7
#define RTW89_WCL_CIPHER_PASSPHRASE 10

/* Sizes of what macOS passes for the getters below. */
#define RTW89_WCL_BEACON_INFO_SIZE  0x808
#define RTW89_WCL_BEACON_FRAME_MAX  0x800
#define RTW89_WCL_BSS_INFO_SIZE     (sizeof(struct rtw89_wcl_beacon) + 0x800)
#define RTW89_WCL_EXT_BSS_SIZE      0x214
#define RTW89_WCL_EXT_BSS_MCS       0xbc
#define RTW89_WCL_EXT_BSS_VHT       0xcc
#define RTW89_WCL_EXT_BSS_HE        0xd4
#define RTW89_WCL_EXT_BSS_RSN       0x113

#ifdef __cplusplus
extern "C" {
#endif

/* Holds a secret: rtw89_wcl_join_wipe() when done with it. */
struct rtw89_wcl_join {
    uint32_t lower_auth;
    uint32_t upper_auth;
    uint32_t ssid_len;
    uint32_t key_len;
    uint32_t key_cipher;
    uint16_t rsn_len;
    uint8_t  ssid[32];
    uint8_t  bssid[6];
    uint8_t  key[64];
};

enum rtw89_wcl_join_kind {
    RTW89_WCL_JOIN_UNSUPPORTED,
    RTW89_WCL_JOIN_OPEN,
    RTW89_WCL_JOIN_PASSPHRASE,
    RTW89_WCL_JOIN_PMK,
};

static inline void rtw89_wcl_join_wipe(struct rtw89_wcl_join *join)
{
    volatile uint8_t *p = (volatile uint8_t *)join;

    for (size_t i = 0; i < sizeof(*join); i++)
        p[i] = 0;
}

static inline uint16_t rtw89_wcl_read16(const uint8_t *p)
{
    uint16_t value;

    memcpy(&value, p, sizeof(value));
    return value;
}

/* False if @buffer cannot be a join request. Nothing past @length is read, and
 * the lengths inside it are not trusted. */
static inline bool rtw89_wcl_decode_join(const void *buffer, size_t length,
                                         struct rtw89_wcl_join *out)
{
    const uint8_t *p = (const uint8_t *)buffer;
    uint32_t count, ssid_len, key_len;
    uint16_t rsn_len;
    uint8_t bits = 0;

    if (!buffer || !out || length < 0x218)
        return false;
    count = rtw89_wcl_read32(p + 0x214);
    ssid_len = rtw89_wcl_read32(p + 0x1c);
    key_len = rtw89_wcl_read32(p + 0x44);
    rsn_len = rtw89_wcl_read16(p + 0xd4);
    if (!count || count > RTW89_WCL_JOIN_MAX_CANDIDATES || !ssid_len || ssid_len > 32 ||
        key_len > sizeof(out->key) || rsn_len > 257 || length < 0x218 + (size_t)count * 18)
        return false;
    for (unsigned int i = 0; i < 6; i++)
        bits |= p[0x21c + i];
    if (!bits || (p[0x21c] & 1))
        return false;

    memset(out, 0, sizeof(*out));
    out->lower_auth = rtw89_wcl_read32(p + 0x10);
    out->upper_auth = rtw89_wcl_read32(p + 0x14);
    out->ssid_len = ssid_len;
    memcpy(out->ssid, p + 0x20, ssid_len);
    out->key_len = key_len;
    out->key_cipher = rtw89_wcl_read32(p + 0x48);
    memcpy(out->key, p + 0x50, key_len);
    memcpy(out->bssid, p + 0x21c, 6);
    out->rsn_len = rsn_len;
    return true;
}

/*
 * What this driver can do with @join. WPA2-PSK is what the backend has proven,
 * with the key handshake done by the backend, so macOS only hands over the
 * secret. WEP, 802.1X, SAE and the SHA-256 AKMs are refused.
 */
static inline enum rtw89_wcl_join_kind rtw89_wcl_join_classify(const struct rtw89_wcl_join *join)
{
    if (join->lower_auth != 1)
        return RTW89_WCL_JOIN_UNSUPPORTED;
    if (!join->upper_auth)
        return join->key_len ? RTW89_WCL_JOIN_UNSUPPORTED : RTW89_WCL_JOIN_OPEN;
    if (!(join->upper_auth & RTW89_WCL_AUTH_WPA2_PSK) ||
        (join->upper_auth & ~(RTW89_WCL_AUTH_WPA2_PSK | RTW89_WCL_AUTH_WPA_PSK)))
        return RTW89_WCL_JOIN_UNSUPPORTED;
    if (join->key_cipher == RTW89_WCL_CIPHER_PASSPHRASE)
        return join->key_len >= 8 && join->key_len <= 63 ? RTW89_WCL_JOIN_PASSPHRASE :
                                                           RTW89_WCL_JOIN_UNSUPPORTED;
    if ((join->key_cipher == RTW89_WCL_CIPHER_PMK || join->key_cipher == RTW89_WCL_CIPHER_PMKSA) &&
        join->key_len == 32)
        return RTW89_WCL_JOIN_PMK;
    return RTW89_WCL_JOIN_UNSUPPORTED;
}

/* Whether macOS may use the driver at all: without this it stops every join with
 * "driver not available" before asking the driver. */
struct rtw89_wcl_driver_event {
    uint32_t version;
    uint32_t reserved0;
    uint32_t available;
    uint32_t reserved1;
    uint32_t reason;
    uint32_t sub_reason;
    uint8_t  reserved2[0xf8 - 24];
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_driver_event) == 0xf8, "DriverAvailability size");

static inline void rtw89_wcl_driver_fill(struct rtw89_wcl_driver_event *out, bool available)
{
    memset(out, 0, sizeof(*out));
    out->version = 3;
    out->available = available;
    out->reason = available ? 0xe0821803u : 0xe0821804u;
}

/* getHW_ADDR: the card's own address, whatever the station uses at the moment. */
struct rtw89_wcl_hw_address {
    uint32_t version;
    uint8_t  address[6];
    uint8_t  padding[2];
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_hw_address) == 12, "HW_ADDR native size");

/* Whether @mac can be a station's address, private ones included. */
static inline bool rtw89_wcl_mac_usable(const uint8_t mac[6])
{
    uint8_t any = 0;

    for (unsigned int i = 0; i < 6; i++)
        any |= mac[i];
    return any && !(mac[0] & 1);
}

struct rtw89_wcl_assoc_event {
    uint16_t status;
    uint16_t reason;
    uint8_t  mlo;
    uint8_t  bssid[6];
    uint8_t  reserved;
    uint32_t mlo_data[2];
    int32_t  driver_status;
    int32_t  driver_reason;
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_assoc_event) == 0x1c, "AssociationEvent size");

struct rtw89_wcl_peer {
    uint8_t  bssid[6];
    uint16_t reserved;
    int32_t  driver_status;
    int32_t  driver_reason;
};

struct rtw89_wcl_first_beacon {
    uint16_t status;
    uint16_t reason;
    struct rtw89_wcl_peer peer;
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_first_beacon) == 0x14, "FirstBeaconEvent size");

struct rtw89_wcl_connect_event {
    uint16_t status;
    uint16_t reason;
    struct rtw89_wcl_peer peers[10];
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_connect_event) == 0xa4, "ConnectionEvent size");

struct rtw89_wcl_link_event {
    uint8_t  bssid[6];
    uint8_t  up;
    uint8_t  in_sleep;
    uint32_t reason;
    uint32_t reserved;
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_link_event) == 16, "LinkStatusEvent size");

struct rtw89_wcl_deauth_event {
    int32_t  driver_status;
    int32_t  driver_reason;
    uint16_t reason;
    uint8_t  bssid[6];
    uint8_t  disassociation;
    uint8_t  reserved[3];
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_deauth_event) == 0x14, "DeauthEvent size");

/* The snapshot WCLNetManager::handleLqmUpdate takes: beacon reception only, the
 * radio measurements stay invalid. */
struct rtw89_wcl_lqm {
    uint8_t  prefix[0x28];
    uint32_t received_delta;
    uint32_t scheduled_delta;
    uint8_t  beacon_valid;
    uint8_t  reserved[0x1d8 - 0x31];
    uint8_t  snapshot_valid;
    uint8_t  counters_valid;
    uint8_t  tail[2];
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_lqm) == 0x1dc, "WCL LQM update size");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_lqm, beacon_valid) == 0x30, "LQM beacon valid");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_lqm, snapshot_valid) == 0x1d8, "LQM snapshot valid");

static inline bool rtw89_wcl_lqm_fill(struct rtw89_wcl_lqm *out, uint32_t received_delta)
{
    if (!received_delta)
        return false;
    memset(out, 0, sizeof(*out));
    out->received_delta = received_delta;
    out->beacon_valid = 1;
    out->snapshot_valid = 1;
    out->counters_valid = 1;
    return true;
}

/* getCHANNELS_INFO */
struct rtw89_wcl_channels {
    uint8_t  reserved0[8];
    uint8_t  country[4];
    uint16_t count;
    uint16_t channel_spec[400];
    uint8_t  channel[400];
    uint8_t  indoor[400];
    uint8_t  dfs[400];
    uint8_t  passive[400];
    uint8_t  supports_40[400];
    uint8_t  supports_80[400];
    uint8_t  reserved1[2];
    uint32_t firmware_spec[400];
    uint32_t bitmap[400];
    uint8_t  reserved2[0x2720 - 0x1910];
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_channels) == 0x2720, "CHANNELS_INFO size");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_channels, channel) == 0x32e, "CHANNELS_INFO channel");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_channels, firmware_spec) == 0xc90, "CHANNELS_INFO spec");
RTW89_WCL_ASSERT(offsetof(struct rtw89_wcl_channels, bitmap) == 0x12d0, "CHANNELS_INFO bitmap");

static inline uint16_t rtw89_wcl_channel_spec(const struct rtw89_glue_channel *channel)
{
    return (uint16_t)(channel->channel | (channel->freq < 3000 ? 0x1000 : 0xd000));
}

static inline bool rtw89_wcl_channels_add(struct rtw89_wcl_channels *info,
                                          const struct rtw89_glue_channel *channel)
{
    unsigned int index = info->count;
    const bool five = channel->freq >= 3000;

    if (index >= 400 || !channel->channel || channel->freq >= 5925)
        return false;
    info->count++;
    info->channel_spec[index] = rtw89_wcl_channel_spec(channel);
    info->firmware_spec[index] = info->channel_spec[index];
    info->channel[index] = channel->channel;
    info->dfs[index] = channel->radar;
    info->passive[index] = channel->passive;
    info->supports_40[index] = 1;
    info->supports_80[index] = five;
    info->bitmap[index] = 3u | (five ? 4u : 0) | (channel->radar ? 8u : 0) |
                          (channel->passive ? 32u : 0);
    return true;
}

/* getWCL_CHANNELS_INFO */
#pragma pack(push, 1)
struct rtw89_wcl_channel_entry {
    uint16_t channel_spec;
    uint8_t  reserved[4];
    uint8_t  channel;
    uint8_t  flags;
};

struct rtw89_wcl_channel_list {
    struct rtw89_wcl_channel_entry channels[400];
    uint16_t count;
    uint8_t  country[3];
    uint8_t  supports_6ghz;
};
#pragma pack(pop)
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_channel_list) == 0xc86, "ChannelInfo size");

/* getWCL_TRAFFIC_COUNTERS */
struct rtw89_wcl_traffic {
    uint64_t realtime_infra_tx;
    uint64_t realtime_infra_rx;
    uint64_t infra_tx;
    uint64_t infra_rx;
    uint64_t realtime_awdl_tx;
    uint64_t realtime_nan_tx;
    uint64_t timestamp_ns;
};
RTW89_WCL_ASSERT(sizeof(struct rtw89_wcl_traffic) == 0x38, "WCL_TRAFFIC_COUNTERS size");

/* A rate the driver reported, for getRATE: whole megabits, never 0 when known. */
static inline uint32_t rtw89_wcl_rate_mbps(const struct rtw89_glue_rate *rate)
{
    return rate->kbps ? (rate->kbps + 999) / 1000 : 0;
}

/* What ConnectComplete should say about a join that ended: 802.11 status from
 * the backend's -1000 - status, otherwise a plain failure. */
static inline uint16_t rtw89_wcl_join_status(int last_error)
{
    if (last_error <= -1000 && last_error > -2000)
        return (uint16_t)(-1000 - last_error);
    return 1;
}

/* The 802.11 reason in the backend's -2000 - reason, or 0. */
static inline uint16_t rtw89_wcl_deauth_reason(int last_error)
{
    return last_error <= -2000 ? (uint16_t)(-2000 - last_error) : 0;
}

#ifdef __cplusplus
}
#endif
#endif
