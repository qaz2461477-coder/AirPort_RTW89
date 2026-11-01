#include <assert.h>
#include <string.h>
#include "../../src/kext/rtw89_bssid.h"
int main(void) {
    unsigned char out[6];
    const unsigned char expected[] = {0xb8,0xfb,0xb3,0xc7,0xa0,0x22};
    assert(rtw89_parse_bssid("a4:b1:c2:d3:e4:f5", out));
    assert(!memcmp(out, expected, 6));
    assert(rtw89_parse_bssid("B8:FB:B3:C7:A0:22", out));
    assert(rtw89_parse_bssid("02:00:00:00:00:01", out));
    const char *bad[] = {"", "a", "a4:", "a4:b1:c2:d3:e4:f", "a4:b1:c2:d3:e4:f55",
        "a4-b1-c2-d3-e4-f5", "a4:b1:c2:d3:e4:fz", "00:00:00:00:00:00",
        "ff:ff:ff:ff:ff:ff", "01:00:00:00:00:01"};
    for (unsigned i=0; i<sizeof(bad)/sizeof(bad[0]); ++i) {
        memset(out, 0xaa, 6);
        assert(!rtw89_parse_bssid(bad[i], out));
        for (unsigned j=0;j<6;++j) assert(out[j]==0xaa);
    }
    return 0;
}
