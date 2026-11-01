/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The seam between the native Wi-Fi front (the IO80211Controller and its
 * interfaces) and RTW89HardwareTransport, the one owner of the card.
 *
 * Plain C with no apple80211 and no Linux types: an Apple ABI change stays on
 * the front's side of it, and the rtw89 side never sees the Tahoe headers.
 *
 * Ownership. The provider owns the transport, the transport owns the PCI
 * device, its BAR, its interrupt and the driver. The front owns nothing of the
 * hardware: it holds this table from a successful bind() until quiesce()
 * returns, and the provider outlives it because the controller is its client.
 *
 * Going away. The front calls quiesce() first. When it returns no event is
 * running and none will follow, so the front may free what its events use.
 * Only then does the provider close the transport. A transport that has been
 * quiesced is not bound again: the driver does not survive a second probe in
 * one boot (efuse reads as zero), so nothing here pretends otherwise.
 */
#ifndef RTW89_HW_BRIDGE_H
#define RTW89_HW_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtw89_glue.h"

#define RTW89_HW_API_VERSION 5

#define RTW89_HW_EVENT_LINK         0x1u    /* see rtw89_glue_link() */
#define RTW89_HW_EVENT_SCAN_DONE    0x2u
#define RTW89_HW_EVENT_SCAN_ABORTED 0x4u
#define RTW89_HW_EVENT_POWER        0x8u    /* the radio settled; see radio_on() */
#define RTW89_HW_EVENT_JOIN_FAILED  0x10u   /* see join_error() */
#define RTW89_HW_EVENT_TICK         0x20u   /* every second from bind() on */
#define RTW89_HW_EVENT_MAC          0x40u   /* the address settled; see mac_error() */

#ifdef __cplusplus
extern "C" {
#endif

/* What the front gives the transport. */
struct rtw89_hw_front {
    uint32_t size;
    uint32_t version;
    void *ctx;

    /* Events that have happened since the last call. On a thread of the
     * transport's own, one call at a time, with no driver lock held: unlike the
     * glue's callbacks this may sleep and may call the glue. */
    void (*events)(void *ctx, uint32_t bits);
    /* A received Ethernet frame, valid during the call. On the driver's receive
     * thread, may sleep. */
    void (*rx_frame)(void *ctx, const uint8_t *frame, size_t len);
    /* There is room for frames again after tx_room() returned 0. */
    void (*tx_wake)(void *ctx);
};

/* What the transport gives the front. */
struct rtw89_hw_ops {
    uint32_t size;
    uint32_t version;
    void *ctx;

    /* What the probe found. */
    bool (*get_info)(void *ctx, struct rtw89_glue_info *info);
    /* Start delivering events. Copies @front. 0 or a negative errno. */
    int (*bind)(void *ctx, const struct rtw89_hw_front *front);
    /* Ask for the radio on or off. Returns at once, because starting it takes
     * seconds and macOS calls this inside its command gate: a later request
     * wins, and RTW89_HW_EVENT_POWER follows when the radio has settled. */
    int (*set_power)(void *ctx, bool on);
    /* Whether the radio is on right now. Never blocks. */
    bool (*radio_on)(void *ctx);
    /* What the last attempt to start it returned: 0 or a negative errno. */
    int (*power_error)(void *ctx);
    /* Start a scan of every channel. Returns at once, for the same reason as
     * set_power(): RTW89_HW_EVENT_SCAN_DONE or _ABORTED follows. */
    int (*request_scan)(void *ctx);
    /* The networks of the last scan, as rtw89_glue_scan_entry(). No sleeping. */
    bool (*scan_entry)(void *ctx, unsigned int index, struct rtw89_glue_bss *out, uint8_t *ies,
                       size_t ies_max, size_t *ies_len, uint16_t *beacon_int);
    /* The channels the card and the regulatory domain allow. No sleeping. */
    unsigned int (*channels)(void *ctx, struct rtw89_glue_channel *out, unsigned int max);
    /* The link and country. From the events thread: may take driver locks. */
    void (*link)(void *ctx, struct rtw89_glue_link *link);
    /* One network by address, as rtw89_glue_find_bss(). No sleeping. */
    bool (*find_bss)(void *ctx, const uint8_t bssid[6], struct rtw89_glue_bss *out, uint8_t *ies,
                     size_t ies_max, size_t *ies_len, uint16_t *beacon_int);
    /* The network's last beacon; returns its length, 0 if none. No sleeping. */
    size_t (*beacon)(void *ctx, uint8_t *frame, size_t max);
    /* The RSN element of the association request; returns its length. */
    size_t (*assoc_rsn_ie)(void *ctx, uint8_t *ie, size_t max);
    /*
     * Join @bssid. @passphrase or @pmk (32 bytes) is copied and wiped here; both
     * NULL means an open network. Returns at once: the backend does the key
     * handshake, the link events tell how it goes, and RTW89_HW_EVENT_JOIN_FAILED
     * follows if it could not start.
     */
    int (*join)(void *ctx, const uint8_t *ssid, size_t ssid_len, const uint8_t bssid[6],
                const char *passphrase, size_t passphrase_len, const uint8_t *pmk);
    int (*join_error)(void *ctx);
    /* Leave the network. Returns at once. */
    void (*leave)(void *ctx);
    /* Use @mac as the station's address, restarting the radio if it is up.
     * Returns at once and runs before a join queued after it. */
    int (*set_mac)(void *ctx, const uint8_t mac[6]);
    int (*mac_error)(void *ctx);
    /* As rtw89_glue_tx_alloc(), _tx(), _tx_cancel() and _tx_room(). */
    void *(*tx_alloc)(void *ctx, size_t len, uint8_t **frame);
    int (*tx)(void *ctx, void *handle);
    void (*tx_cancel)(void *ctx, void *handle);
    unsigned int (*tx_room)(void *ctx);
    /* Stop delivering events; see above. Any thread that may sleep, but not the
     * events thread itself, and not inside the front's command gate: a delivery
     * may be waiting for that gate. */
    void (*quiesce)(void *ctx);
};

/* Whether @ops is a table this build can call. */
static inline bool rtw89_hw_ops_compatible(const struct rtw89_hw_ops *ops)
{
    return ops && ops->size >= sizeof(*ops) && ops->version == RTW89_HW_API_VERSION &&
           ops->get_info && ops->bind && ops->set_power && ops->radio_on &&
           ops->power_error && ops->request_scan && ops->scan_entry && ops->channels &&
           ops->link && ops->find_bss && ops->beacon && ops->assoc_rsn_ie && ops->join &&
           ops->join_error && ops->leave && ops->set_mac && ops->mac_error && ops->tx_alloc &&
           ops->tx && ops->tx_cancel &&
           ops->tx_room && ops->quiesce;
}

static inline bool rtw89_hw_front_compatible(const struct rtw89_hw_front *front)
{
    return front && front->size >= sizeof(*front) && front->version == RTW89_HW_API_VERSION &&
           front->events && front->rx_frame && front->tx_wake;
}

/*
 * Whether @mac can be published as the card's own address. A failed efuse read
 * leaves zeros, and the driver then falls back on eth_random_addr()
 * (rtw8922a.c), whose result is locally administered: an address from a card's
 * efuse is never that, so such a probe did not read the real one.
 */
static inline bool rtw89_hw_mac_publishable(const uint8_t mac[6])
{
    uint8_t any = 0;

    for (unsigned int i = 0; i < 6; i++)
        any |= mac[i];
    return any && !(mac[0] & 0x03);
}

/* Whether a probe's result is complete enough to put in front of macOS. */
static inline bool rtw89_hw_info_publishable(const struct rtw89_glue_info *info)
{
    return info && rtw89_hw_mac_publishable(info->mac) && info->fw_version[0] &&
           info->tx_streams && info->rx_streams;
}

#ifdef __cplusplus
}
#endif
#endif
