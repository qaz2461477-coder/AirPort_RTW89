#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "rtw89_hw_bridge.h"
#include "rtw89_wcl_join.h"

static bool get_info(void *ctx, struct rtw89_glue_info *info) { (void)ctx; (void)info; return true; }
static int bind(void *ctx, const struct rtw89_hw_front *front) { (void)ctx; (void)front; return 0; }
static int set_power(void *ctx, bool on) { (void)ctx; (void)on; return 0; }
static bool radio_on(void *ctx) { (void)ctx; return false; }
static int power_error(void *ctx) { (void)ctx; return 0; }
static int request_scan(void *ctx) { (void)ctx; return 0; }
static bool scan_entry(void *ctx, unsigned int i, struct rtw89_glue_bss *o, uint8_t *b, size_t m,
                       size_t *l, uint16_t *n) { (void)ctx; (void)i; (void)o; (void)b; (void)m; (void)l; (void)n; return false; }
static unsigned int channels(void *ctx, struct rtw89_glue_channel *o, unsigned int m) { (void)ctx; (void)o; (void)m; return 0; }
static void link_info(void *ctx, struct rtw89_glue_link *l) { (void)ctx; (void)l; }
static bool find_bss(void *ctx, const uint8_t b[6], struct rtw89_glue_bss *o, uint8_t *i, size_t m,
                     size_t *l, uint16_t *n) { (void)ctx; (void)b; (void)o; (void)i; (void)m; (void)l; (void)n; return false; }
static size_t beacon_copy(void *ctx, uint8_t *f, size_t m) { (void)ctx; (void)f; (void)m; return 0; }
static size_t rsn_copy(void *ctx, uint8_t *f, size_t m) { (void)ctx; (void)f; (void)m; return 0; }
static int join_fn(void *ctx, const uint8_t *s, size_t n, const uint8_t b[6], const char *p, size_t l,
                   const uint8_t *k) { (void)ctx; (void)s; (void)n; (void)b; (void)p; (void)l; (void)k; return 0; }
static int join_error(void *ctx) { (void)ctx; return 0; }
static void leave_fn(void *ctx) { (void)ctx; }
static int set_mac(void *ctx, const uint8_t m[6]) { (void)ctx; (void)m; return 0; }
static int mac_error(void *ctx) { (void)ctx; return 0; }
static void *tx_alloc(void *ctx, size_t l, uint8_t **f) { (void)ctx; (void)l; (void)f; return NULL; }
static int tx_fn(void *ctx, void *h) { (void)ctx; (void)h; return 0; }
static void tx_cancel(void *ctx, void *h) { (void)ctx; (void)h; }
static unsigned int tx_room(void *ctx) { (void)ctx; return 0; }
static void rx_frame(void *ctx, const uint8_t *f, size_t l) { (void)ctx; (void)f; (void)l; }
static void tx_wake(void *ctx) { (void)ctx; }
static void quiesce(void *ctx) { (void)ctx; }
static void events(void *ctx, uint32_t bits) { (void)ctx; (void)bits; }

static void test_mac(void) {
    const uint8_t card[6] = {0x50, 0xee, 0x32, 0xaf, 0x39, 0x1b};
    const uint8_t zero[6] = {0};
    const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t multicast[6] = {0x51, 0xee, 0x32, 0xaf, 0x39, 0x1b};
    /* eth_random_addr() on a failed efuse read, and the 0.2.0 diagnostic address */
    const uint8_t random[6] = {0x7a, 0x1d, 0x45, 0xdb, 0x37, 0x8d};
    const uint8_t diagnostic[6] = {0x02, 0x52, 0x89, 0x22, 0x00, 0x01};
    assert(rtw89_hw_mac_publishable(card));
    assert(!rtw89_hw_mac_publishable(zero));
    assert(!rtw89_hw_mac_publishable(broadcast));
    assert(!rtw89_hw_mac_publishable(multicast));
    assert(!rtw89_hw_mac_publishable(random));
    assert(!rtw89_hw_mac_publishable(diagnostic));
}

static void test_info(void) {
    struct rtw89_glue_info info = {{0x50, 0xee, 0x32, 0xaf, 0x39, 0x1b}, "0.35.80.3", 0, 2, 1, 2, 2};
    struct rtw89_glue_info copy = info;
    assert(rtw89_hw_info_publishable(&info));
    assert(!rtw89_hw_info_publishable(NULL));
    copy.fw_version[0] = 0;
    assert(!rtw89_hw_info_publishable(&copy));
    copy = info;
    copy.tx_streams = 0;
    assert(!rtw89_hw_info_publishable(&copy));
    copy = info;
    copy.rx_streams = 0;
    assert(!rtw89_hw_info_publishable(&copy));
    copy = info;
    memset(copy.mac, 0, sizeof(copy.mac));
    assert(!rtw89_hw_info_publishable(&copy));
}

static void test_tables(void) {
    struct rtw89_hw_ops ops = {sizeof(ops), RTW89_HW_API_VERSION, NULL, get_info, bind, set_power,
                               radio_on, power_error, request_scan, scan_entry, channels, link_info,
                               find_bss, beacon_copy, rsn_copy, join_fn, join_error, leave_fn, set_mac,
                               mac_error, tx_alloc,
                               tx_fn, tx_cancel, tx_room, quiesce};
    struct rtw89_hw_ops bad = ops;
    struct rtw89_hw_front front = {sizeof(front), RTW89_HW_API_VERSION, NULL, events, rx_frame, tx_wake};
    struct rtw89_hw_front badf = front;
    assert(rtw89_hw_ops_compatible(&ops));
    assert(!rtw89_hw_ops_compatible(NULL));
    bad.size = sizeof(bad) - 1;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.version = RTW89_HW_API_VERSION + 1;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.quiesce = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.set_power = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.radio_on = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.request_scan = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.scan_entry = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    assert(rtw89_hw_front_compatible(&front));
    assert(!rtw89_hw_front_compatible(NULL));
    badf.size = 0;
    assert(!rtw89_hw_front_compatible(&badf));
    badf = front;
    badf.events = NULL;
    assert(!rtw89_hw_front_compatible(&badf));
    badf = front;
    badf.rx_frame = NULL;
    assert(!rtw89_hw_front_compatible(&badf));
    bad = ops;
    bad.join = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.set_mac = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
    bad = ops;
    bad.tx = NULL;
    assert(!rtw89_hw_ops_compatible(&bad));
}

static void put32(uint8_t *p, uint32_t value) { memcpy(p, &value, sizeof(value)); }

/* A request in a buffer exactly @size long, so ASan sees any read past it. */
static uint8_t *scan_request(size_t size, uint32_t type, uint32_t ssid_len, uint32_t count) {
    uint8_t *p = calloc(1, size);
    assert(p);
    if (size >= 0x58) {
        put32(p, 7);
        put32(p + 0x1c, ssid_len);
        put32(p + 0x40, type);
        put32(p + 0x54, count);
    }
    return p;
}

static void put_channel(uint8_t *p, unsigned int index, uint32_t channel, uint32_t flags) {
    put32(p + 0x58 + index * 12 + 4, channel);
    put32(p + 0x58 + index * 12 + 8, flags);
}

static void test_scan_decode(void) {
    struct rtw89_wcl_scan scan;
    uint8_t *p;

    assert(!rtw89_wcl_decode_scan(NULL, RTW89_WCL_SCAN_REQUEST_SIZE, &scan));
    p = scan_request(0x57, 1, 0, 0);
    assert(!rtw89_wcl_decode_scan(p, 0x57, &scan));
    free(p);

    /* the smallest request: every channel, any network */
    p = scan_request(0x58, 1, 0, 0);
    assert(rtw89_wcl_decode_scan(p, 0x58, &scan));
    assert(scan.identifier == 7 && scan.channel_count == 0 && rtw89_wcl_scan_usable(&scan));
    free(p);

    /* lengths inside the request are not trusted */
    p = scan_request(0x100, 1, 33, 0);
    assert(!rtw89_wcl_decode_scan(p, 0x100, &scan));
    free(p);
    p = scan_request(0x100, 1, 0, 401);
    assert(!rtw89_wcl_decode_scan(p, 0x100, &scan));
    free(p);
    p = scan_request(0x58 + 12, 1, 0, 2);
    assert(!rtw89_wcl_decode_scan(p, 0x58 + 12, &scan));
    free(p);
    p = scan_request(0x100, 1, 0, 0xffffffffu);
    assert(!rtw89_wcl_decode_scan(p, 0x100, &scan));
    free(p);
    p = scan_request(0x100, 0, 0, 0);
    assert(!rtw89_wcl_decode_scan(p, 0x100, &scan));
    free(p);
    p = scan_request(0x100, 5, 0, 0);
    assert(!rtw89_wcl_decode_scan(p, 0x100, &scan));
    free(p);

    /* the largest count fits the size the family passes */
    p = scan_request(RTW89_WCL_SCAN_REQUEST_SIZE, 1, 32, RTW89_WCL_SCAN_MAX_CHANNELS);
    memset(p + 0x20, 'x', 32);
    assert(rtw89_wcl_decode_scan(p, RTW89_WCL_SCAN_REQUEST_SIZE, &scan));
    assert(scan.ssid_len == 32 && scan.ssid[31] == 'x');
    free(p);

    /* channels: band mismatches and 6 GHz drop out, the highest one fits */
    p = scan_request(0x58 + 6 * 12, 1, 0, 6);
    put_channel(p, 0, 6, RTW89_WCL_CHANNEL_FLAG_2GHZ);
    put_channel(p, 1, 36, RTW89_WCL_CHANNEL_FLAG_5GHZ);
    put_channel(p, 2, 196, RTW89_WCL_CHANNEL_FLAG_5GHZ);
    put_channel(p, 3, 5, RTW89_WCL_CHANNEL_FLAG_6GHZ);
    put_channel(p, 4, 100, RTW89_WCL_CHANNEL_FLAG_2GHZ);
    put_channel(p, 5, 197, 0);
    assert(rtw89_wcl_decode_scan(p, 0x58 + 6 * 12, &scan));
    assert(scan.channels[0] == (1 << 6));
    assert(scan.channels[36 / 8] & (1 << (36 % 8)));
    assert(scan.channels[196 / 8] & (1 << (196 % 8)));
    assert(!(scan.channels[100 / 8] & (1 << (100 % 8))));
    assert(rtw89_wcl_scan_usable(&scan));
    free(p);

    /* only 6 GHz asked for: nothing to scan */
    p = scan_request(0x58 + 12, 1, 0, 1);
    put_channel(p, 0, 5, RTW89_WCL_CHANNEL_FLAG_6GHZ);
    assert(rtw89_wcl_decode_scan(p, 0x58 + 12, &scan));
    assert(!rtw89_wcl_scan_usable(&scan));
    free(p);
}

static void test_scan_results(void) {
    struct rtw89_glue_bss bss = {{0xa4, 0xb1, 0xc2, 0xd3, 0xe4, 0xf5}, 6, "TestAP", 5200, 40, -48,
                                 0x411, 3, 3, 80, 2};
    struct rtw89_wcl_scan scan;
    struct rtw89_wcl_beacon beacon;

    memset(&scan, 0, sizeof(scan));
    scan.type = 1;
    assert(rtw89_wcl_scan_wants(&scan, &bss));
    scan.channel_count = 1;
    assert(!rtw89_wcl_scan_wants(&scan, &bss));
    scan.channels[40 / 8] = 1 << (40 % 8);
    assert(rtw89_wcl_scan_wants(&scan, &bss));
    memcpy(scan.ssid, "Other", 5);
    scan.ssid_len = 5;
    assert(!rtw89_wcl_scan_wants(&scan, &bss));
    memcpy(scan.ssid, "Ming", 4);
    scan.ssid_len = 4;
    assert(!rtw89_wcl_scan_wants(&scan, &bss));
    memcpy(scan.ssid, "TestAP", 6);
    scan.ssid_len = 6;
    assert(rtw89_wcl_scan_wants(&scan, &bss));
    /* a passive scan does not narrow by name */
    memcpy(scan.ssid, "Other", 5);
    scan.ssid_len = 5;
    scan.type = 2;
    assert(rtw89_wcl_scan_wants(&scan, &bss));

    assert(rtw89_wcl_beacon_fill(&beacon, &bss, 100, 300));
    assert(beacon.ie_length == 300 && beacon.channel == 40 && beacon.channel_spec == (40 | 0xd000));
    assert(beacon.rssi == -48 && beacon.interval == 100 && beacon.capability == 0x411);
    assert(beacon.ssid_length == 6 && !memcmp(beacon.ssid, "TestAP", 6));
    assert(!memcmp(beacon.bssid, bss.bssid, 6));
    assert(beacon.flags == (RTW89_WCL_BEACON_SSID_PRESENT | RTW89_WCL_BEACON_RSSI_VALID));
    bss.freq = 2437;
    bss.channel = 6;
    assert(rtw89_wcl_beacon_fill(&beacon, &bss, 100, 0) && beacon.channel_spec == (6 | 0x1000));
    /* what cannot be said in this format is left out */
    bss.freq = 5955;
    assert(!rtw89_wcl_beacon_fill(&beacon, &bss, 100, 0));
    bss.freq = 2437;
    bss.channel = 0;
    assert(!rtw89_wcl_beacon_fill(&beacon, &bss, 100, 0));
    bss.channel = 197;
    assert(!rtw89_wcl_beacon_fill(&beacon, &bss, 100, 0));
    bss.channel = 6;
    bss.ssid_len = 33;
    assert(!rtw89_wcl_beacon_fill(&beacon, &bss, 100, 0));
}

static void test_channel_flags(void) {
    struct rtw89_glue_channel two = {2437, 6, false, false};
    struct rtw89_glue_channel dfs = {5260, 52, true, true};
    assert(rtw89_wcl_channel_flags(&two) == (0x2 | 0x8 | 0x80));
    assert(rtw89_wcl_channel_flags(&dfs) == (0x2 | 0x10 | 0x100));
}

/* A request in a buffer exactly @size long, so ASan sees any read past it. */
static uint8_t *join_request(size_t size, uint32_t count, uint32_t ssid_len, uint32_t key_len,
                             uint32_t upper, uint32_t cipher) {
    uint8_t *p = calloc(1, size);
    assert(p);
    if (size >= 0x218) {
        put32(p + 0x10, 1);
        put32(p + 0x14, upper);
        put32(p + 0x1c, ssid_len);
        memcpy(p + 0x20, "TestAP", 6);
        put32(p + 0x44, key_len);
        put32(p + 0x48, cipher);
        memset(p + 0x50, 'k', key_len < 64 ? key_len : 64);
        put32(p + 0x214, count);
        p[0x21c] = 0xb8;
        p[0x21d] = 0xfb;
    }
    return p;
}

static void test_join(void) {
    const size_t full = RTW89_WCL_JOIN_REQUEST_SIZE, psk = RTW89_WCL_AUTH_WPA2_PSK;
    struct rtw89_wcl_join join;
    uint8_t *p;

    assert(!rtw89_wcl_decode_join(NULL, full, &join));
    p = join_request(0x217, 1, 6, 8, psk, 10);
    assert(!rtw89_wcl_decode_join(p, 0x217, &join));
    free(p);

    /* the smallest whole request, and the largest count, which is the size passed */
    p = join_request(0x218 + 18, 1, 6, 8, psk, 10);
    assert(rtw89_wcl_decode_join(p, 0x218 + 18, &join));
    assert(join.ssid_len == 6 && !memcmp(join.ssid, "TestAP", 6) && join.key_len == 8);
    assert(join.bssid[0] == 0xb8 && join.key[7] == 'k');
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_PASSPHRASE);
    free(p);
    p = join_request(full, 10, 32, 63, psk, 10);
    assert(rtw89_wcl_decode_join(p, full, &join));
    free(p);

    /* lengths inside the request are not trusted */
    p = join_request(full, 0, 6, 8, psk, 10);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    p = join_request(full, 11, 6, 8, psk, 10);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    p = join_request(0x218 + 18, 2, 6, 8, psk, 10);
    assert(!rtw89_wcl_decode_join(p, 0x218 + 18, &join));
    free(p);
    p = join_request(full, 1, 0, 8, psk, 10);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    p = join_request(full, 1, 33, 8, psk, 10);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    p = join_request(full, 1, 6, 65, psk, 10);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    p = join_request(full, 1, 6, 0xffffffffu, psk, 10);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    p = join_request(full, 1, 6, 8, psk, 10);
    p[0xd4] = 0x02;
    p[0xd5] = 0x01;     /* an RSN element of 258 bytes */
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);
    /* an address that cannot be an access point's */
    p = join_request(full, 1, 6, 8, psk, 10);
    p[0x21c] = 0xb9;
    assert(!rtw89_wcl_decode_join(p, full, &join));
    memset(p + 0x21c, 0, 6);
    assert(!rtw89_wcl_decode_join(p, full, &join));
    free(p);

    /* what the backend can do, and nothing else */
    memset(&join, 0, sizeof(join));
    join.lower_auth = 1;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_OPEN);
    join.key_len = 5;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);   /* WEP */
    join.upper_auth = psk;
    join.key_cipher = 10;
    join.key_len = 7;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.key_len = 64;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.key_len = 8;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_PASSPHRASE);
    join.upper_auth = psk | RTW89_WCL_AUTH_WPA_PSK;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_PASSPHRASE);
    join.key_cipher = 6;
    join.key_len = 32;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_PMK);
    join.key_cipher = 2;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.key_cipher = 6;
    join.upper_auth = RTW89_WCL_AUTH_WPA_PSK;                               /* WPA1 alone */
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.upper_auth = 1u << 12;                                             /* SAE */
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.upper_auth = psk | (1u << 10);                                     /* with SHA-256 */
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.upper_auth = psk | (1u << 2);                                      /* with 802.1X */
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);
    join.upper_auth = psk;
    join.lower_auth = 2;
    assert(rtw89_wcl_join_classify(&join) == RTW89_WCL_JOIN_UNSUPPORTED);

    /* the secret does not stay */
    memset(join.key, 'k', sizeof(join.key));
    rtw89_wcl_join_wipe(&join);
    for (size_t i = 0; i < sizeof(join.key); i++)
        assert(join.key[i] == 0);
}

static void test_driver_and_address(void) {
    struct rtw89_wcl_driver_event event;
    const uint8_t card[6] = {0x50, 0xee, 0x32, 0xaf, 0x39, 0x1b};
    const uint8_t private_mac[6] = {0x7a, 0x1d, 0x45, 0xdb, 0x37, 0x8d};
    const uint8_t zero[6] = {0};
    const uint8_t group[6] = {0x51, 0xee, 0x32, 0xaf, 0x39, 0x1b};

    rtw89_wcl_driver_fill(&event, true);
    assert(event.version == 3 && event.available == 1 && event.reason == 0xe0821803u);
    rtw89_wcl_driver_fill(&event, false);
    assert(event.available == 0 && event.reason == 0xe0821804u);
    assert(rtw89_wcl_mac_usable(card) && rtw89_wcl_mac_usable(private_mac));
    assert(sizeof(struct rtw89_wcl_hw_address) == 12);
    assert(!rtw89_wcl_mac_usable(zero) && !rtw89_wcl_mac_usable(group));
}

static void test_join_state(void) {
    struct rtw89_wcl_lqm lqm;
    struct rtw89_glue_rate rate = {0, 0, 0, 0, 0};
    static struct rtw89_wcl_channels info;
    struct rtw89_glue_channel two = {2437, 6, false, false};
    struct rtw89_glue_channel five = {5260, 52, true, true};
    struct rtw89_glue_channel six = {5955, 1, false, false};

    assert(rtw89_wcl_join_status(-1017) == 17);
    assert(rtw89_wcl_join_status(-110) == 1);
    assert(rtw89_wcl_join_status(0) == 1);
    assert(rtw89_wcl_deauth_reason(-2015) == 15);
    assert(rtw89_wcl_deauth_reason(-1017) == 0);
    assert(!rtw89_wcl_lqm_fill(&lqm, 0));
    assert(rtw89_wcl_lqm_fill(&lqm, 8) && lqm.received_delta == 8 && lqm.beacon_valid && lqm.counters_valid);
    assert(rtw89_wcl_rate_mbps(&rate) == 0);
    rate.kbps = 960700;
    assert(rtw89_wcl_rate_mbps(&rate) == 961);

    assert(rtw89_wcl_channels_add(&info, &two) && rtw89_wcl_channels_add(&info, &five));
    assert(!rtw89_wcl_channels_add(&info, &six));
    assert(info.count == 2 && info.channel_spec[0] == (6 | 0x1000) && info.channel_spec[1] == (52 | 0xd000));
    assert(info.bitmap[0] == 3 && info.bitmap[1] == (3 | 4 | 8 | 32));
    assert(info.dfs[1] && info.passive[1] && info.supports_80[1] && !info.supports_80[0]);
    info.count = 400;
    assert(!rtw89_wcl_channels_add(&info, &two));
}

int main(void) {
    test_join();
    test_driver_and_address();
    test_join_state();
    test_scan_decode();
    test_scan_results();
    test_channel_flags();
    test_mac();
    test_info();
    test_tables();
    return 0;
}
