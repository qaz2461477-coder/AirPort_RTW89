/* SPDX-License-Identifier: GPL-2.0 */
#include "RTW89BridgeBase.inc"
#include <kern/clock.h>
#include <kern/locks.h>
#include "../kext/rtw89_chips.h"
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOOutputQueue.h>

static void bridgeStage(IOService *controller, const char *value, bool pause = true) {
    controller->setProperty("RTW89 Bridge Stage", value);
    auto bridge = controller->getProvider();
    if (bridge && bridge->getProvider())
        bridge->getProvider()->setProperty("RTW89 Bridge Stage", value);
    IOLog("RTW89NativeBridge: %s\n", value);
    if (pause) IOSleep(500);
}

class RTW89BridgeController;

class RTW89BridgeInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(RTW89BridgeInterface)
    IO80211Controller *_controller = nullptr;
    ether_addr _address = {};
    RTW89BridgeController *_owner = nullptr;
    volatile bool _radioUp = false;
    volatile SInt32 _operations = 0;
    uint8_t _permanent[6] = {};
    rtw89_glue_channel _channels[APPLE80211_MAX_CHANNELS] = {};
    volatile UInt32 _channelCount = 0;
    /* Tells macOS no, and on the first time records that it asked and in what
     * order: what a powered radio is asked for next is unknown until seen. */
    IOReturn unsupported(const char *operation, volatile SInt32 *calls) {
        if (OSIncrementAtomic(calls) == 0 && !ml_at_interrupt_context()) {
            char name[80];
            snprintf(name, sizeof(name), "RTW89 Op %s", operation);
            setProperty(name, static_cast<unsigned long long>(OSIncrementAtomic(&_operations) + 1), 32);
        }
        return kIOReturnUnsupported;
    }
public:
    void radioChanged(bool up) { _radioUp = up; }
    void setOwner(RTW89BridgeController *owner) { _owner = owner; }
    /* What getSUPPORTED_CHANNELS reports; refreshed from the events thread. */
    void refreshChannels(const rtw89_hw_ops *hw) {
        _channelCount = hw->channels(hw->ctx, _channels, APPLE80211_MAX_CHANNELS);
    }
    IOReturn getOP_MODE(apple80211_opmode_data *data) override {
        if (!data) return kIOReturnBadArgument;
        data->version = APPLE80211_VERSION;
        data->op_mode = APPLE80211_M_STA;
        return kIOReturnSuccess;
    }
    IOReturn getSUPPORTED_CHANNELS(apple80211_sup_channel_data *data) override {
        if (!data) return kIOReturnBadArgument;
        /* Only what is written below: the size of the caller's buffer is known
         * from the headers, not from the family. */
        data->version = APPLE80211_VERSION;
        data->num_channels = 0;
        for (UInt32 i = 0; i < _channelCount; ++i) {
            /* The structure has no room for 6 GHz, and no join uses it yet. */
            if (_channels[i].freq >= 5925 || data->num_channels >= APPLE80211_MAX_CHANNELS) continue;
            auto &out = data->supported_channels[data->num_channels++];
            out.version = APPLE80211_VERSION;
            out.channel = _channels[i].channel;
            out.flags = rtw89_wcl_channel_flags(&_channels[i]);
        }
        return kIOReturnSuccess;
    }
    /* The channels macOS may ask for are the ones the card has. */
    IOReturn getHW_SUPPORTED_CHANNELS(apple80211_sup_channel_data *data) override {
        return getSUPPORTED_CHANNELS(data);
    }
    IOReturn getHW_ADDR(apple80211_hw_mac_address *data) override {
        if (!data) return kIOReturnBadArgument;
        rtw89_wcl_hw_address value = {};
        value.version = 1;
        memcpy(value.address, _permanent, sizeof(value.address));
        memcpy(data, &value, sizeof(value));
        return kIOReturnSuccess;
    }
    IOReturn setWCL_SCAN_REQ(apple80211ScanRequest *request) override;
    IOReturn setWCL_SCAN_ABORT(void *) override;
    IOReturn setWCL_ASSOCIATE(apple80211AssocCandidates *request) override;
    IOReturn setWCL_JOIN_ABORT(apple80211_wcl_abort_join *) override;
    IOReturn setWCL_LEAVE_NETWORK(apple80211_leave_network *) override;
    IOReturn getCHANNEL(apple80211_channel_data *data) override;
    IOReturn getRSSI(apple80211_rssi_data *data) override;
    IOReturn getRATE(apple80211_rate_data *data) override;
    IOReturn getMCS(apple80211_mcs_data *data) override;
    IOReturn getNSS(apple80211_nss_data *data) override;
    IOReturn getBEACON_INFO(apple80211_beacon_info_t *data) override;
    IOReturn getWCL_BSS_INFO(apple80211_beacon_msg *data) override;
    IOReturn getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *data) override;
    IOReturn getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *data) override;
    IOReturn getCHANNELS_INFO(apple80211_channels_info *data) override;
    IOReturn getWCL_CHANNELS_INFO(apple80211ChannelInfo *data) override;
    IOReturn getWCL_LOW_LATENCY_INFO(apple80211_low_latency_info *data) override {
        if (!data) return kIOReturnBadArgument;
        /* No low-latency or P2P session. */
        bzero(data, 4);
        return kIOReturnSuccess;
    }
    IOReturn setWCL_UPDATE_FAST_LANE(apple80211_fastlane *data) override {
        if (!data) return kIOReturnBadArgument;
        auto bytes = reinterpret_cast<const uint8_t *>(data);
        return (bytes[0] || bytes[1]) ? kIOReturnUnsupported : kIOReturnSuccess;
    }
    /* The backend keeps no PMK cache across joins. */
    IOReturn setCLEAR_PMKSA_CACHE(void *) override { return kIOReturnSuccess; }
    bool initializeInfra(IO80211Controller *controller) {
        _controller = controller;
        return IO80211InfraInterface::init();
    }
    bool initializeSkywalk(const uint8_t mac[6]) {
        memcpy(_address.octet, mac, sizeof(_address.octet));
        memcpy(_permanent, mac, sizeof(_permanent));
        return _controller && IO80211SkywalkInterface::init(_controller, &_address);
    }
    IO80211Controller *getController() override { return _controller; }
    CCLogStream *getLogger() const override {
        return _controller ? _controller->getLogger() : nullptr;
    }
    IOReturn getHardwareAddress(ether_addr *address) override {
        if (!address) return kIOReturnBadArgument;
        *address = _address;
        return kIOReturnSuccess;
    }
    /* macOS gives each network a private address before joining it. */
    IOReturn setMacAddress(mloAddrArray &addresses) override;
    void addressChanged(const uint8_t mac[6]) { memcpy(_address.octet, mac, sizeof(_address.octet)); }
    IOReturn enable(unsigned int) override { return _radioUp ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn disable(unsigned int) override { return kIOReturnSuccess; }
    bool isCommandProhibited(int) override { return false; }
    bool isChipInterfaceReady() override { return _radioUp; }
    bool isInterfaceEnabled() override { return _radioUp; }
    int getAssocState() override { return 0; }
/* ../native_interface/UnsupportedOperations.inc with each answer recorded */
#include "TracedOperations.inc"
};
OSDefineMetaClassAndStructors(RTW89BridgeInterface, IO80211InfraProtocol)

/* The BSD adapter exposes its associated Skywalk provider once BSD prepare
 * succeeds. This is required by native Skywalk consumers of the BSD object;
 * before attachment it retains the normal controller provider relationship.
 * Adapted from AirportRTW WCL AirportRTWEthernetInterface, qcwap / 钟先耀,
 * copyright 2023, GPL-2.0, pinned source 82c78d0b1c300b25a6706c433170c1c635293670.
 * This version omits the upstream HAL/datapath code. */
class RTW89BridgeBSD : public IOEthernetInterface {
    OSDeclareDefaultStructors(RTW89BridgeBSD)
    IO80211SkywalkInterface *_native = nullptr;
    IOCommandGate *_gate = nullptr;
    bool _bsdAttached = false;
    static IOReturn prepareGated(OSObject *, void *arg, void *, void *, void *) {
        auto self = static_cast<RTW89BridgeBSD *>(arg);
        auto controller = OSDynamicCast(IO80211Controller, self->getController());
        auto queue = controller ? controller->getWorkQueue() : nullptr;
        if (!queue || !queue->inGate() || queue->onThread()) return kIOReturnNotPermitted;
        return self->_native->prepareBSDInterface(self->getIfnet(), 0);
    }
public:
    bool initialize(IO80211Controller *controller, IO80211SkywalkInterface *native,
                    IOCommandGate *gate) {
        _native = native;
        _gate = gate;
        return native && gate && IOEthernetInterface::init(controller);
    }
    IOReturn attachToDataLinkLayer(IOOptionBits options, void *parameter) override {
        auto controller = OSDynamicCast(IO80211Controller, getController());
        if (!controller || !setInterfaceSubType(3)) return kIOReturnNotReady;
        bridgeStage(controller, "bsd-data-link-attach-enter", false);
        IOReturn result = IOEthernetInterface::attachToDataLinkLayer(options, parameter);
        if (result != kIOReturnSuccess) {
            bridgeStage(controller, "bsd-data-link-attach-failed", false);
            return result;
        }
        char name[IFNAMSIZ];
        snprintf(name, sizeof(name), "%s%u", ifnet_name(getIfnet()), ifnet_unit(getIfnet()));
        UInt8 builtIn = 0;
        _native->setProperty("built-in", &builtIn, sizeof(builtIn));
        IOEthernetAddress address = {};
        if (controller->getHardwareAddress(&address) == kIOReturnSuccess)
            setProperty(kIOMACAddress, address.bytes, sizeof(address.bytes));
        _native->setProperty("IOInterfaceName", name);
        _native->setProperty(kIOInterfaceUnit, ifnet_unit(getIfnet()), 32);
        _native->setProperty(kIOInterfaceNamePrefix, ifnet_name(getIfnet()));
        bridgeStage(controller, "bsd-native-prepare-enter", false);
        result = _gate->runAction(prepareGated, this);
        if (result != kIOReturnSuccess) {
            controller->setProperty("RTW89 BSD Prepare Error", static_cast<unsigned long long>(result), 32);
            bridgeStage(controller, "bsd-native-prepare-failed", false);
            IOEthernetInterface::detachFromDataLinkLayer(options, parameter);
            return result;
        }
        _bsdAttached = true;
        _native->registerService();
        controller->setProperty("RTW89 BSD Name", name);
        controller->setProperty("RTW89 BSD Publication Complete", kOSBooleanTrue);
        bridgeStage(controller, "native-interface-published-hardware-probed", false);
        return kIOReturnSuccess;
    }
    void detachFromDataLinkLayer(IOOptionBits options, void *parameter) override {
        IOEthernetInterface::detachFromDataLinkLayer(options, parameter);
        _bsdAttached = false;
    }
    IOService *getProvider() const override {
        return _bsdAttached ? _native : IOEthernetInterface::getProvider();
    }
    /* The address the stack puts on frames, kept in step with the radio's. */
    bool setLinkAddress(const uint8_t mac[6]) {
        if (!getIfnet() || ifnet_set_lladdr(getIfnet(), mac, 6)) return false;
        setProperty(kIOMACAddress, const_cast<uint8_t *>(mac), 6);
        return true;
    }
    /* Adapted from the same reference: the stack sends once this is running. */
    void setLinkRunning(bool up) {
        if (!getIfnet()) return;
        if (up) ifnet_set_flags(getIfnet(), IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
        else ifnet_set_flags(getIfnet(), 0, IFF_RUNNING);
    }
};
OSDefineMetaClassAndStructors(RTW89BridgeBSD, IOEthernetInterface)

class RTW89BridgeController : public RTW89BridgeBase {
    OSDeclareDefaultStructors(RTW89BridgeController)
    RTW89BridgeInterface *_native = nullptr;
    IONetworkInterface *_bsd = nullptr;
    IOCommandGate *_bsdGate = nullptr;
    bool _serviceAttached = false;
    bool _logicalAttached = false;
    bool _nativeStarted = false;
    bool _baseStarted = false;
    /* Told to macOS; it refuses every join until the driver says it is there. */
    bool _driverAnnounced = false;
    /* A new address is on its way to the radio, which restarts for it. */
    volatile bool _macChanging = false;
    uint8_t _previousMac[6] = {};
    /* Restarting the radio ends a scan, and macOS changes the address twice
     * right after power on: a change during a scan waits for the scan's end. */
    volatile UInt32 _macDeferred = 0;
    /* The table of the provider's transport, from a successful bind until
     * stop() has quiesced it. */
    const rtw89_hw_ops *_hw = nullptr;
    rtw89_glue_info _hwInfo = {};
    char _hardwareVersion[64] = {};
    volatile SInt32 _powerGets = 0;
    volatile SInt32 _powerSets = 0;
    char _country[3] = {};
    /* From an accepted request until its results are out. Only the one holding
     * this may touch _scan. */
    volatile UInt32 _scanActive = 0;
    rtw89_wcl_scan _scan = {};
    volatile SInt32 _scanRequests = 0;
    volatile SInt32 _scanRejected = 0;

    IONetworkMedium *_medium = nullptr;
    /* The last link state, read on the events thread: the getters answer from
     * this, because they run where the driver's locks must not be waited for. */
    struct rtw89_glue_link _link = {};
    /* From an accepted join until ConnectComplete. */
    volatile UInt32 _joinActive = 0;
    bool _joinSeen = false;
    bool _assocNotified = false;
    bool _firstBeacon = false;
    bool _linkIndicated = false;
    bool _voluntary = false;
    unsigned _joinSeconds = 0;
    uint32_t _beaconsAtJoin = 0;
    uint32_t _lqmBeacons = 0;
    uint8_t _joinBSSID[6] = {};
    volatile bool _bssValid = false;
    uint8_t _bssInfo[RTW89_WCL_BSS_INFO_SIZE] = {};
    /* frames may pass; the stack has the link */
    volatile bool _dataUp = false;
    volatile SInt32 _txBusy = 0;
    volatile SInt32 _joinRequests = 0;
    volatile SInt32 _joinRejected = 0;

    /* Adapted from the same reference's country update: the native interface
     * hears of a change inside the queue's gate, never on the queue's own
     * thread, and never with a driver lock held. */
    static IOReturn powerChangedGated(OSObject *, void *arg, void *, void *, void *) {
        auto self = static_cast<RTW89BridgeController *>(arg);
        auto queue = self->getWorkQueue();
        if (!queue || !queue->inGate() || queue->onThread()) return kIOReturnNotPermitted;
        self->_native->postMessage(APPLE80211_M_POWER_CHANGED, nullptr, 0, false);
        return kIOReturnSuccess;
    }
    static IOReturn driverGated(OSObject *, void *arg, void *available, void *, void *) {
        auto self = static_cast<RTW89BridgeController *>(arg);
        auto queue = self->getWorkQueue();
        if (!queue || !queue->inGate() || queue->onThread()) return kIOReturnNotPermitted;
        self->announceDriver(available != nullptr);
        return kIOReturnSuccess;
    }
    /* Adapted from the same reference's firmware events, which macOS waits for.
     * Once per power up: the reset halts the family's scan and join managers,
     * and only the power on that follows the first one brings them back. */
    void announceDriver(bool available) {
        rtw89_wcl_driver_event event;
        if (available) {
            UInt32 reset = 0;
            postMessage(_native, RTW89_WCL_MSG_RESET_IFACE, &reset, sizeof(reset), true);
            reset = 1;
            postMessage(_native, RTW89_WCL_MSG_RESET_IFACE, &reset, sizeof(reset), true);
        }
        rtw89_wcl_driver_fill(&event, available);
        IOReturn post = postMessage(_native, RTW89_WCL_MSG_DRIVER, &event, sizeof(event), true);
        setProperty("RTW89 Driver Announced", available ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("RTW89 Driver Post Status", static_cast<unsigned long long>(static_cast<UInt32>(post)), 32);
    }
    /* Whichever side gets here first sends the address: the end of the scan, or
     * requestAddress() if the scan ended just as it was putting it aside. */
    void applyDeferredAddress() {
        if (OSCompareAndSwap(1, 0, &_macDeferred) && _hw && _hw->set_mac(_hw->ctx, _hwInfo.mac))
            _macChanging = false;
    }
    /* The address reached the radio, or could not. */
    void addressSettled() {
        const int error = _hw->mac_error(_hw->ctx);
        setProperty("RTW89 MAC Error", static_cast<unsigned long long>(static_cast<long long>(error)), 32);
        if (error) {
            auto bsd = OSDynamicCast(RTW89BridgeBSD, _bsd);
            memcpy(_hwInfo.mac, _previousMac, sizeof(_hwInfo.mac));
            _native->addressChanged(_previousMac);
            if (bsd) bsd->setLinkAddress(_previousMac);
        }
        _macChanging = false;
    }
    static IOReturn scanFinishedGated(OSObject *, void *arg, void *status, void *, void *) {
        auto self = static_cast<RTW89BridgeController *>(arg);
        auto queue = self->getWorkQueue();
        if (!queue || !queue->inGate() || queue->onThread()) return kIOReturnNotPermitted;
        self->finishScan(static_cast<IOReturn>(reinterpret_cast<uintptr_t>(status)));
        return kIOReturnSuccess;
    }
    /* In the gate, where the family accepts them: every result, then the end. */
    void finishScan(IOReturn status) {
        if (!_hw || !OSCompareAndSwap(1, 0, &_scanActive)) return;
        unsigned posted = 0;
        OSArray *results = OSArray::withCapacity(16);
        const size_t room = sizeof(rtw89_wcl_beacon) + 1024;
        auto message = status == kIOReturnSuccess ? static_cast<uint8_t *>(IOMalloc(room)) : nullptr;
        if (message) {
            rtw89_glue_bss bss;
            size_t length = 0;
            uint16_t interval = 0;
            auto beacon = reinterpret_cast<rtw89_wcl_beacon *>(message);
            for (unsigned i = 0; _hw->scan_entry(_hw->ctx, i, &bss, message + sizeof(*beacon),
                                                 room - sizeof(*beacon), &length, &interval); ++i) {
                if (!rtw89_wcl_scan_wants(&_scan, &bss) || !rtw89_wcl_beacon_fill(beacon, &bss, interval, length))
                    continue;
                IOReturn result = postMessage(_native, RTW89_WCL_MSG_SCAN_RESULT, message,
                                              sizeof(*beacon) + length, true);
                if (result == kIOReturnSuccess) ++posted;
                describeNetwork(results, bss);
            }
            IOFree(message, room);
        }
        UInt32 done = static_cast<UInt32>(status);
        IOReturn post = postMessage(_native, RTW89_WCL_MSG_SCAN_DONE, &done, sizeof(done), true);
        setProperty("RTW89 Scan Requests", static_cast<unsigned long long>(_scanRequests), 32);
        setProperty("RTW89 Scan Rejected", static_cast<unsigned long long>(_scanRejected), 32);
        setProperty("RTW89 Scan Posted", posted, 32);
        setProperty("RTW89 Scan Status", static_cast<unsigned long long>(static_cast<UInt32>(status)), 32);
        setProperty("RTW89 Scan Done Post Status", static_cast<unsigned long long>(static_cast<UInt32>(post)), 32);
        if (results) {
            setProperty("RTW89 Scan Results", results);
            results->release();
        }
        applyDeferredAddress();
    }
    /* What the last scan found, for the test: what macOS got, seen from here. */
    void describeNetwork(OSArray *results, const rtw89_glue_bss &bss) {
        auto entry = results ? OSDictionary::withCapacity(4) : nullptr;
        if (!entry) return;
        char address[18];
        snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x", bss.bssid[0], bss.bssid[1],
                 bss.bssid[2], bss.bssid[3], bss.bssid[4], bss.bssid[5]);
        auto ssid = OSData::withBytes(bss.ssid, bss.ssid_len);
        auto bssid = OSString::withCString(address);
        auto channel = OSNumber::withNumber(bss.channel, 32);
        auto signal = OSNumber::withNumber(static_cast<unsigned long long>(static_cast<long long>(bss.signal)), 32);
        if (ssid) entry->setObject("ssid", ssid);
        if (bssid) entry->setObject("bssid", bssid);
        if (channel) entry->setObject("channel", channel);
        if (signal) entry->setObject("rssi", signal);
        OSSafeReleaseNULL(ssid);
        OSSafeReleaseNULL(bssid);
        OSSafeReleaseNULL(channel);
        OSSafeReleaseNULL(signal);
        results->setObject(entry);
        entry->release();
    }
    /* A scan ended. On the transport's own thread, no driver lock held. */
    void scanEnded(bool aborted) {
        _bsdGate->runAction(scanFinishedGated, this,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(aborted ? kIOReturnAborted : kIOReturnSuccess)));
    }
    /* The radio settled. On the transport's own thread, no driver lock held. */
    void powerChanged() {
        const bool up = _hw->radio_on(_hw->ctx);
        _hw->link(_hw->ctx, &_link);
        memcpy(_country, _link.country, sizeof(_country));
        _native->refreshChannels(_hw);
        _native->radioChanged(up);
        if (!up) {
            scanEnded(true);
            linkChanged(false);
        }
        if (up != _driverAnnounced) {
            _driverAnnounced = up;
            _bsdGate->runAction(driverGated, this, reinterpret_cast<void *>(static_cast<uintptr_t>(up)));
        }
        setProperty("RTW89 Radio Up", up ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("RTW89 Radio Error", static_cast<unsigned long long>(_hw->power_error(_hw->ctx)), 32);
        setProperty("RTW89 Power Gets", static_cast<unsigned long long>(_powerGets), 32);
        setProperty("RTW89 Power Sets", static_cast<unsigned long long>(_powerSets), 32);
        auto owner = OSDynamicCast(RTW89BridgeProvider, getProvider());
        if (owner) owner->refreshLog();
        IOReturn posted = _bsdGate->runAction(powerChangedGated, this);
        setProperty("RTW89 Power Post Status", static_cast<unsigned long long>(posted), 32);
        IOLog("RTW89NativeBridge: radio %s\n", up ? "up" : "down");
    }
    static void hardwareEvents(void *self, uint32_t bits) {
        auto me = static_cast<RTW89BridgeController *>(self);
        if (bits & RTW89_HW_EVENT_POWER) me->powerChanged();
        if (bits & RTW89_HW_EVENT_MAC) me->addressSettled();
        if (bits & (RTW89_HW_EVENT_SCAN_DONE | RTW89_HW_EVENT_SCAN_ABORTED))
            me->scanEnded(bits & RTW89_HW_EVENT_SCAN_ABORTED);
        if (bits & RTW89_HW_EVENT_JOIN_FAILED) me->linkChanged(true);
        else if ((bits & RTW89_HW_EVENT_LINK) ||
                 ((bits & RTW89_HW_EVENT_TICK) && (me->_joinActive || me->_dataUp))) me->linkChanged(false);
    }
    static void hardwareRx(void *self, const uint8_t *frame, size_t length) {
        auto me = static_cast<RTW89BridgeController *>(self);
        if (!me->_dataUp || !me->_bsd) return;
        mbuf_t packet = me->allocatePacket(static_cast<UInt32>(length));
        if (!packet) return;
        if (mbuf_copyback(packet, 0, length, frame, MBUF_DONTWAIT) != 0) {
            me->freePacket(packet);
            return;
        }
        me->_bsdGate->runAction(inputGated, me, packet, reinterpret_cast<void *>(length));
    }
    /* Queued input has to be serialised, and the family's gate is what does. */
    static IOReturn inputGated(OSObject *, void *arg, void *packet, void *length, void *) {
        auto me = static_cast<RTW89BridgeController *>(arg);
        me->_bsd->inputPacket(static_cast<mbuf_t>(packet), static_cast<UInt32>(reinterpret_cast<uintptr_t>(length)),
                              IONetworkInterface::kInputOptionQueuePacket);
        me->_bsd->flushInputQueue();
        return kIOReturnSuccess;
    }
    static void hardwareTxWake(void *self) {
        auto me = static_cast<RTW89BridgeController *>(self);
        if (me->_dataUp && me->_bsd) me->_bsd->signalOutputThread();
    }
    /* Tell the stack before macOS hears the join has finished. */
    void dataChanged(bool up, const struct rtw89_glue_link &link) {
        if (up == _dataUp) return;
        if (up) {
            UInt64 mbit = link.mode >= 3 ? (link.width >= 80 ? 1201 : link.width >= 40 ? 574 : 287) :
                          link.mode == 2 ? (link.width >= 80 ? 866 : link.width >= 40 ? 400 : 173) :
                          link.mode == 1 ? (link.width >= 40 ? 300 : 144) : 54;
            _dataUp = true;
            setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive, _medium, mbit * 1000000ULL);
            _bsd->startOutputThread();
        } else {
            _dataUp = false;
            setLinkStatus(kIONetworkLinkValid, _medium, 0);
            _bsd->stopOutputThread();
            _bsd->flushOutputQueue();
        }
    }
    /* The link moved, or a second passed with something to watch. On the events
     * thread, where the driver may be asked. */
    void linkChanged(bool joinFailed) {
        _hw->link(_hw->ctx, &_link);
        const bool up = _link.state == RTW89_GLUE_LINK_CONNECTED && _link.authorized;
        dataChanged(up, _link);
        _bsdGate->runAction(linkGated, this, reinterpret_cast<void *>(static_cast<uintptr_t>(joinFailed)));
        setProperty("RTW89 Link State", _link.state, 32);
        setProperty("RTW89 Link Authorized", _link.authorized ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("RTW89 Link Error", static_cast<unsigned long long>(static_cast<long long>(_link.last_error)), 32);
        setProperty("RTW89 Link Channel Frequency", _link.freq, 32);
        setProperty("RTW89 Link Beacons", _link.beacons, 32);
        setProperty("RTW89 TX Frames", _link.tx_frames, 32);
        setProperty("RTW89 RX Frames", _link.rx_frames, 32);
        setProperty("RTW89 Join Requests", static_cast<unsigned long long>(_joinRequests), 32);
        setProperty("RTW89 Join Rejected", static_cast<unsigned long long>(_joinRejected), 32);
    }
    static IOReturn linkGated(OSObject *, void *arg, void *joinFailed, void *, void *) {
        auto self = static_cast<RTW89BridgeController *>(arg);
        auto queue = self->getWorkQueue();
        if (!queue || !queue->inGate() || queue->onThread()) return kIOReturnNotPermitted;
        self->updateJoin(joinFailed != nullptr);
        return kIOReturnSuccess;
    }
    /* What the first beacon message will refer to: the network's description. */
    void captureBss() {
        auto beacon = reinterpret_cast<rtw89_wcl_beacon *>(_bssInfo);
        rtw89_glue_bss bss;
        size_t length = 0;
        uint16_t interval = 0;
        if (!_hw->find_bss(_hw->ctx, _joinBSSID, &bss, _bssInfo + sizeof(*beacon),
                           sizeof(_bssInfo) - sizeof(*beacon), &length, &interval) ||
            !rtw89_wcl_beacon_fill(beacon, &bss, interval, length)) return;
        _bssValid = true;
    }
    void postLink(bool up) {
        rtw89_wcl_link_event event = {};
        memcpy(event.bssid, _joinBSSID, sizeof(event.bssid));
        event.up = up;
        event.reason = 255;
        _linkIndicated = up;
        postMessage(_native, RTW89_WCL_MSG_LINK, &event, sizeof(event), true);
    }
    void finishJoin(uint16_t status, uint16_t reason, IOReturn driverStatus) {
        if (!OSCompareAndSwap(1, 0, &_joinActive)) return;
        rtw89_wcl_connect_event event = {};
        event.status = status;
        event.reason = reason;
        memcpy(event.peers[0].bssid, _joinBSSID, sizeof(event.peers[0].bssid));
        event.peers[0].driver_status = driverStatus;
        IOReturn post = postMessage(_native, RTW89_WCL_MSG_CONNECT, &event, sizeof(event), true);
        setProperty("RTW89 Connect Post Status", static_cast<unsigned long long>(static_cast<UInt32>(post)), 32);
        setProperty("RTW89 Connect Status", status, 32);
    }
    /* In the gate. A join is not done until the keys are in: ConnectComplete
     * waits for authorized, never for associated alone. */
    void updateJoin(bool joinFailed) {
        const bool up = _link.state == RTW89_GLUE_LINK_CONNECTED && _link.authorized;
        if (!_joinActive) {
            if (_linkIndicated && !up) {
                postLink(false);
                rtw89_wcl_deauth_event event = {};
                event.reason = rtw89_wcl_deauth_reason(_link.last_error);
                memcpy(event.bssid, _joinBSSID, sizeof(event.bssid));
                if (!_voluntary) postMessage(_native, RTW89_WCL_MSG_DEAUTH, &event, sizeof(event), true);
                _bssValid = false;
            } else if (up && _link.beacons - _lqmBeacons >= 8) {
                rtw89_wcl_lqm update;
                if (rtw89_wcl_lqm_fill(&update, _link.beacons - _lqmBeacons) &&
                    postMessage(_native, RTW89_WCL_MSG_LQM, &update, sizeof(update), true) == kIOReturnSuccess)
                    _lqmBeacons = _link.beacons;
            }
            return;
        }
        if (joinFailed) {
            finishJoin(1, 0, kIOReturnError);
            return;
        }
        if (_link.state != RTW89_GLUE_LINK_DOWN) _joinSeen = true;
        if (!_assocNotified && _link.state >= RTW89_GLUE_LINK_ASSOCIATED) {
            captureBss();
            rtw89_wcl_assoc_event event = {};
            memcpy(event.bssid, _joinBSSID, sizeof(event.bssid));
            _assocNotified = true;
            IOReturn post = postMessage(_native, RTW89_WCL_MSG_ASSOC, &event, sizeof(event), true);
            setProperty("RTW89 Assoc Post Status", static_cast<unsigned long long>(static_cast<UInt32>(post)), 32);
        }
        if (_assocNotified && !_bssValid) captureBss();
        if (_assocNotified && _bssValid && !_firstBeacon && _link.beacons != _beaconsAtJoin) {
            rtw89_wcl_first_beacon event = {};
            memcpy(event.peer.bssid, _joinBSSID, sizeof(event.peer.bssid));
            _firstBeacon = true;
            _lqmBeacons = _link.beacons;
            postLink(true);
            postMessage(_native, RTW89_WCL_MSG_FIRST_BEACON, &event, sizeof(event), true);
        }
        if (_firstBeacon && up) {
            finishJoin(0, 0, kIOReturnSuccess);
        } else if (_joinSeen && _link.state == RTW89_GLUE_LINK_DOWN) {
            finishJoin(rtw89_wcl_join_status(_link.last_error), rtw89_wcl_deauth_reason(_link.last_error),
                       kIOReturnError);
            if (_linkIndicated) postLink(false);
        } else if (++_joinSeconds > 30) {
            /* Ticks and link changes both land here, so this is at most 30 s. */
            finishJoin(1, 1004, kIOReturnTimeout);
            if (_linkIndicated) postLink(false);
            _hw->leave(_hw->ctx);
        }
    }
    /* One Ethernet frame from the stack: copied into the driver's own buffer and
     * the mbuf freed. */
    bool transmit(mbuf_t packet) {
        size_t length = mbuf_pkthdr_len(packet);
        uint8_t *frame = nullptr;
        void *handle = _dataUp && _hw ? _hw->tx_alloc(_hw->ctx, length, &frame) : nullptr;
        int result = -1;
        if (handle) {
            if (mbuf_copydata(packet, 0, length, frame) == 0) result = _hw->tx(_hw->ctx, handle);
            else _hw->tx_cancel(_hw->ctx, handle);
        }
        freePacket(packet);
        return result == 0;
    }
    bool bindHardware(const rtw89_hw_ops *ops) {
        rtw89_hw_front front = {};
        front.size = sizeof(front);
        front.version = RTW89_HW_API_VERSION;
        front.ctx = this;
        front.events = hardwareEvents;
        front.rx_frame = hardwareRx;
        front.tx_wake = hardwareTxWake;
        _native->refreshChannels(ops);
        if (ops->bind(ops->ctx, &front)) return false;
        _hw = ops;
        return true;
    }
    void quiesceHardware() {
        if (_native) _native->setOwner(nullptr);
        if (!_hw) return;
        _hw->quiesce(_hw->ctx);
        _hw = nullptr;
    }
    bool prepareRegistration() {
        IOSkywalkEthernetInterface::RegistrationInfo info = {};
        if (!_native->initRegistrationInfo(&info, 1, sizeof(info)) ||
            !_native->mExpansionData || !_native->mExpansionData2 ||
            _native->mExpansionData->fRegistrationInfo ||
            _native->mExpansionData2->fRegistrationInfo) return false;
        auto network = static_cast<IOSkywalkNetworkInterface::RegistrationInfo *>(IOMalloc(sizeof(info)));
        auto ethernet = static_cast<IOSkywalkEthernetInterface::RegistrationInfo *>(IOMalloc(sizeof(info)));
        if (!network || !ethernet) {
            if (network) IOFree(network, sizeof(info));
            if (ethernet) IOFree(ethernet, sizeof(info));
            return false;
        }
        memcpy(network, &info, sizeof(info));
        memcpy(ethernet, &info, sizeof(info));
        _native->mExpansionData->fRegistrationInfo = network;
        _native->mExpansionData2->fRegistrationInfo = ethernet;
        _native->deferBSDAttach(true);
        return true;
    }
    bool setupMedium() {
        auto dictionary = OSDictionary::withCapacity(1);
        if (!dictionary) return false;
        auto medium = IONetworkMedium::medium(kIOMediumIEEE80211Auto, 0);
        bool okay = medium && IONetworkMedium::addMedium(dictionary, medium) &&
                    publishMediumDictionary(dictionary) && setCurrentMedium(medium) &&
                    setSelectedMedium(medium);
        if (okay) _medium = medium;
        else OSSafeReleaseNULL(medium);
        dictionary->release();
        return okay;
    }
    void cleanup() {
        if (_bsd) {
            IONetworkController::detachInterface(_bsd, true);
            OSSafeReleaseNULL(_bsd);
        }
        if (_nativeStarted) { _native->stop(this); _nativeStarted = false; }
        if (_logicalAttached) { IO80211Controller::detachInterface(_native, true); _logicalAttached = false; }
        if (_serviceAttached) { _native->detach(this); _serviceAttached = false; }
        OSSafeReleaseNULL(_native);
        if (_bsdGate) {
            if (_bsdGate->getWorkLoop()) _bsdGate->getWorkLoop()->removeEventSource(_bsdGate);
            OSSafeReleaseNULL(_bsdGate);
        }
        OSSafeReleaseNULL(_medium);
    }
    static IOReturn versionData(apple80211_version_data *data, const char *text) {
        if (!data) return kIOReturnBadArgument;
        memset(data, 0, sizeof(*data));
        data->version = APPLE80211_VERSION;
        strlcpy(data->string, text, sizeof(data->string));
        data->string_len = static_cast<u_int16_t>(strlen(data->string));
        return kIOReturnSuccess;
    }
public:
    /* setWCL_SCAN_REQ: starts a scan, or says why not. The answer comes as
     * messages, when the scan has ended. */
    IOReturn requestScan(const void *request) {
        rtw89_wcl_scan scan;
        OSIncrementAtomic(&_scanRequests);
        if (!rtw89_wcl_decode_scan(request, RTW89_WCL_SCAN_REQUEST_SIZE, &scan)) {
            OSIncrementAtomic(&_scanRejected);
            return kIOReturnBadArgument;
        }
        if (!rtw89_wcl_scan_usable(&scan)) return kIOReturnUnsupported;
        /* A radio restarting for a new address is on its way: the command
         * thread starts the scan after it. */
        if (!_hw || (!_hw->radio_on(_hw->ctx) && !_macChanging)) return kIOReturnNotReady;
        if (!OSCompareAndSwap(0, 1, &_scanActive)) return kIOReturnBusy;
        _scan = scan;
        if (_hw->request_scan(_hw->ctx)) {
            _scanActive = 0;
            return kIOReturnError;
        }
        return kIOReturnSuccess;
    }
    /* setWCL_ASSOCIATE. The answer comes as messages. The secret goes to the
     * transport and is wiped here on every path. */
    IOReturn requestJoin(const void *request) {
        rtw89_wcl_join join;
        OSIncrementAtomic(&_joinRequests);
        if (!rtw89_wcl_decode_join(request, RTW89_WCL_JOIN_REQUEST_SIZE, &join)) {
            OSIncrementAtomic(&_joinRejected);
            setProperty("RTW89 Join Requests", static_cast<unsigned long long>(_joinRequests), 32);
            setProperty("RTW89 Join Rejected", static_cast<unsigned long long>(_joinRejected), 32);
            return kIOReturnBadArgument;
        }
        const auto kind = rtw89_wcl_join_classify(&join);
        IOReturn result = kIOReturnUnsupported;
        if (kind == RTW89_WCL_JOIN_UNSUPPORTED) {
            OSIncrementAtomic(&_joinRejected);
        } else if (!_hw || (!_hw->radio_on(_hw->ctx) && !_macChanging)) {
            /* The radio restarting for a new address is on its way: the join
             * runs after it on the command thread. */
            result = kIOReturnNotReady;
        } else if (_scanActive || !OSCompareAndSwap(0, 1, &_joinActive)) {
            result = kIOReturnBusy;
        } else {
            memcpy(_joinBSSID, join.bssid, sizeof(_joinBSSID));
            _joinSeen = _assocNotified = _firstBeacon = _voluntary = false;
            _bssValid = false;
            _joinSeconds = 0;
            _beaconsAtJoin = _link.beacons;
            const bool phrase = kind == RTW89_WCL_JOIN_PASSPHRASE;
            int ret = _hw->join(_hw->ctx, join.ssid, join.ssid_len, join.bssid,
                                phrase ? reinterpret_cast<const char *>(join.key) : nullptr,
                                phrase ? join.key_len : 0,
                                kind == RTW89_WCL_JOIN_PMK ? join.key : nullptr);
            if (ret) _joinActive = 0;
            result = ret ? kIOReturnError : kIOReturnSuccess;
        }
        rtw89_wcl_join_wipe(&join);
        /* Now, not at the next link event: a refused request causes none. */
        setProperty("RTW89 Join Requests", static_cast<unsigned long long>(_joinRequests), 32);
        setProperty("RTW89 Join Rejected", static_cast<unsigned long long>(_joinRejected), 32);
        setProperty("RTW89 Join Result", static_cast<unsigned long long>(static_cast<UInt32>(result)), 32);
        return result;
    }
    /* setMacAddress: takes effect at once for the stack, and on the command
     * thread for the radio. */
    IOReturn requestAddress(const uint8_t mac[6]) {
        auto bsd = OSDynamicCast(RTW89BridgeBSD, _bsd);
        if (!rtw89_wcl_mac_usable(mac)) return kIOReturnBadArgument;
        if (!_hw || !bsd) return kIOReturnNotReady;
        if (!memcmp(mac, _hwInfo.mac, sizeof(_hwInfo.mac))) return kIOReturnSuccess;
        if (_joinActive || _dataUp) return kIOReturnBusy;
        if (!_macChanging) memcpy(_previousMac, _hwInfo.mac, sizeof(_previousMac));
        if (!bsd->setLinkAddress(mac)) return kIOReturnError;
        memcpy(_hwInfo.mac, mac, sizeof(_hwInfo.mac));
        _native->addressChanged(mac);
        _macChanging = true;
        if (_scanActive) {
            _macDeferred = 1;
            if (!_scanActive) applyDeferredAddress();
            return kIOReturnSuccess;
        }
        if (_hw->set_mac(_hw->ctx, mac)) {
            _macChanging = false;
            bsd->setLinkAddress(_previousMac);
            memcpy(_hwInfo.mac, _previousMac, sizeof(_hwInfo.mac));
            _native->addressChanged(_previousMac);
            return kIOReturnError;
        }
        return kIOReturnSuccess;
    }
    /* macOS left, or gave up. It is not a loss, so no Deauth message follows. */
    IOReturn requestLeave() {
        _voluntary = true;
        OSCompareAndSwap(1, 0, &_joinActive);
        if (_hw) _hw->leave(_hw->ctx);
        return kIOReturnSuccess;
    }
    IOReturn abortJoin() {
        requestLeave();
        postMessage(_native, RTW89_WCL_MSG_JOIN_ABORTED, nullptr, 0, true);
        return kIOReturnSuccess;
    }
    const struct rtw89_glue_link &link() const { return _link; }
    const rtw89_hw_ops *hardware() const { return _hw; }
    const uint8_t *bssInfo() const { return _bssValid ? _bssInfo : nullptr; }
    const char *country() const { return _country; }
    /* setWCL_SCAN_ABORT. The chip finishes on its own; macOS hears now. */
    IOReturn abortScan() {
        return _bsdGate->runAction(scanFinishedGated, this,
                                   reinterpret_cast<void *>(static_cast<uintptr_t>(kIOReturnAborted)));
    }
    bool start(IOService *provider) override {
        bridgeStage(this, "bridge-layout-check");
        setProperty("RTW89 BSD Header Size", sizeof(IOEthernetInterface), 32);
        setProperty("RTW89 BSD Runtime Size", IOEthernetInterface::metaClass->getClassSize(), 32);
        if (IOEthernetInterface::metaClass->getClassSize() != sizeof(IOEthernetInterface) ||
            IOSkywalkNetworkInterface::metaClass->getClassSize() != sizeof(IOSkywalkNetworkInterface) ||
            IOSkywalkEthernetInterface::metaClass->getClassSize() != sizeof(IOSkywalkEthernetInterface)) {
            bridgeStage(this, "bridge-layout-mismatch");
            return false;
        }
        auto owner = OSDynamicCast(RTW89BridgeProvider, provider);
        const rtw89_hw_ops *ops = owner ? owner->hardwareOps() : nullptr;
        if (!rtw89_hw_ops_compatible(ops) || !ops->get_info(ops->ctx, &_hwInfo) ||
            !rtw89_hw_info_publishable(&_hwInfo)) {
            bridgeStage(this, "bridge-hardware-seam-rejected");
            return false;
        }
        snprintf(_hardwareVersion, sizeof(_hardwareVersion), "%s cut %c fw %s",
                 rtw89_chip_name(owner->hardwareDevice()), 'A' + _hwInfo.chip_cut, _hwInfo.fw_version);
        if (!RTW89BridgeBase::start(provider)) {
            bridgeStage(this, "bridge-base-start-failed");
            return false;
        }
        _baseStarted = true;
        UInt8 builtIn = 0;
        setProperty("built-in", &builtIn, sizeof(builtIn));
        setProperty("RTW89 Bridge Version", RTW89_BRIDGE_VERSION);
        if (provider->getProvider()) provider->getProvider()->setProperty("RTW89 Bridge Version", RTW89_BRIDGE_VERSION);
        setProperty("RTW89 Radio Up", kOSBooleanFalse);
        bridgeStage(this, "bridge-medium-setup");
        if (!setupMedium()) { bridgeStage(this, "bridge-medium-failed"); return false; }
        _bsdGate = IOCommandGate::commandGate(this);
        if (!_bsdGate || getWorkQueue()->addEventSource(_bsdGate) != kIOReturnSuccess) {
            bridgeStage(this, "bridge-bsd-gate-failed"); return false;
        }
        bridgeStage(this, "bridge-interface-allocation");
        _native = new RTW89BridgeInterface;
        if (!_native) { bridgeStage(this, "bridge-allocation-failed"); return false; }
        bridgeStage(this, "bridge-infra-init");
        if (!_native->initializeInfra(this)) { bridgeStage(this, "bridge-infra-init-failed"); return false; }
        bridgeStage(this, "bridge-skywalk-init");
        if (!_native->initializeSkywalk(_hwInfo.mac)) { bridgeStage(this, "bridge-skywalk-init-failed"); return false; }
        _native->setOwner(this);
        _native->setInterfaceRole(1);
        _native->setInterfaceId(1);
        bridgeStage(this, "bridge-service-attach");
        if (!_native->attach(this)) { bridgeStage(this, "bridge-service-attach-failed"); return false; }
        _serviceAttached = true;
        bridgeStage(this, "bridge-native-logical-attach");
        if (!IO80211Controller::attachInterface(_native, this)) { bridgeStage(this, "bridge-logical-attach-failed"); return false; }
        _logicalAttached = true;
        bridgeStage(this, "bridge-bsd-object-attach");
        if (!IONetworkController::attachInterface(&_bsd, false)) { bridgeStage(this, "bridge-bsd-object-failed"); return false; }
        bridgeStage(this, "bridge-info-prepare");
        if (!prepareRegistration()) { bridgeStage(this, "bridge-info-failed"); return false; }
        bridgeStage(this, "bridge-native-start");
        if (!_native->start(this)) { bridgeStage(this, "bridge-native-start-failed"); return false; }
        _nativeStarted = true;
        setLinkStatus(kIONetworkLinkValid);
        bridgeStage(this, "bridge-hardware-bind");
        if (!bindHardware(ops)) { bridgeStage(this, "bridge-hardware-bind-failed"); return false; }
        bridgeStage(this, "bridge-bsd-publish-request");
        _bsd->registerService();
        registerService();
        return true;
    }
    void stop(IOService *provider) override {
        /* Before anything an event could reach goes away, and before the
         * provider closes the transport this table belongs to. */
        if (_dataUp) dataChanged(false, _link);
        quiesceHardware();
        while (_txBusy) IOSleep(1);
        cleanup();
        if (_baseStarted) { RTW89BridgeBase::stop(provider); _baseStarted = false; }
    }
    void free() override {
        cleanup();
        RTW89BridgeBase::free();
    }
    IONetworkInterface *createInterface() override {
        auto interface = new RTW89BridgeBSD;
        if (!interface) return nullptr;
        if (!interface->initialize(this, _native, _bsdGate)) { interface->release(); return nullptr; }
        return interface;
    }
    IOReturn getHardwareAddress(IOEthernetAddress *address) override {
        if (!address) return kIOReturnBadArgument;
        memcpy(address->bytes, _hwInfo.mac, sizeof(address->bytes));
        return kIOReturnSuccess;
    }
    const OSString *newVendorString() const override { return OSString::withCString("Realtek"); }
    const OSString *newModelString() const override { return OSString::withCString("RTL8922AE Native Bridge"); }
    bool isCommandProhibited(int) override { return false; }
    IOReturn enable(IONetworkInterface *) override {
        return _hw && _hw->radio_on(_hw->ctx) ? kIOReturnSuccess : kIOReturnNotReady;
    }
    bool configureInterface(IONetworkInterface *netif) override {
        if (!RTW89BridgeBase::configureInterface(netif)) return false;
        /* The stack keeps the queue, with its scheduling and TCP back-pressure,
         * and outputStart() pulls as the driver has room. */
        return netif->configureOutputPullModel(256, 0, 0,
            IONetworkInterface::kOutputPacketSchedulingModelNormal) == kIOReturnSuccess;
    }
    /* The chip passes unicast, broadcast and the network's multicast by itself. */
    IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override {
        if (group == gIONetworkFilterGroup) {
            *filters = kIOPacketFilterUnicast | kIOPacketFilterBroadcast |
                       kIOPacketFilterMulticast | kIOPacketFilterMulticastAll;
            return kIOReturnSuccess;
        }
        return RTW89BridgeBase::getPacketFilters(group, filters);
    }
    IOReturn setPromiscuousMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastMode(bool) override { return kIOReturnSuccess; }
    IOReturn setMulticastList(IOEthernetAddress *, UInt32) override { return kIOReturnSuccess; }
    IOReturn outputStart(IONetworkInterface *netif, IOOptionBits) override {
        IOReturn result = kIOReturnSuccess;
        OSIncrementAtomic(&_txBusy);
        for (;;) {
            unsigned room = _dataUp && _hw ? _hw->tx_room(_hw->ctx) : 16;
            mbuf_t packet = nullptr;
            if (!room) { result = kIOReturnNoResources; break; }
            if (netif->dequeueOutputPackets(room < 16 ? room : 16, &packet) != kIOReturnSuccess) break;
            while (packet) {
                mbuf_t next = mbuf_nextpkt(packet);
                mbuf_setnextpkt(packet, nullptr);
                transmit(packet);
                packet = next;
            }
        }
        OSDecrementAtomic(&_txBusy);
        return result;
    }
    IOReturn disable(IONetworkInterface *) override { return kIOReturnSuccess; }
    /* Not used with the pull model; kept for callers of the old entry point. */
    UInt32 outputPacket(mbuf_t packet, void *) override {
        if (!packet) return kIOReturnOutputDropped;
        OSIncrementAtomic(&_txBusy);
        bool sent = transmit(packet);
        OSDecrementAtomic(&_txBusy);
        return sent ? kIOReturnOutputSuccess : kIOReturnOutputDropped;
    }
    IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *data) override {
        return versionData(data, "RTW89 native bridge " RTW89_BRIDGE_VERSION);
    }
    IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *data) override {
        return versionData(data, _hardwareVersion);
    }
    IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *data) override {
        if (!data) return kIOReturnBadArgument;
        memset(data, 0, sizeof(*data));
        data->version = APPLE80211_VERSION;
        /* Only what the backend has proven: WPA2 with CCMP. No WPA3, no PMF. */
        data->capabilities[0] = 1 << APPLE80211_CAP_AES_CCM;
        data->capabilities[1] = 1 << (APPLE80211_CAP_WPA2 - 8);
        data->capabilities[2] = 1 << (APPLE80211_CAP_WME - 16);
        data->capabilities[5] = 0x40;   /* scan completion events */
        data->capabilities[17] = 0x80;  /* HE, as WCLDeviceConfiguration reads it */
        return kIOReturnSuccess;
    }
    IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *data) override {
        if (!data) return kIOReturnBadArgument;
        memset(data, 0, sizeof(*data));
        data->version = APPLE80211_VERSION;
        memcpy(data->cc, _country, 2);
        return kIOReturnSuccess;
    }
    IOReturn getPOWER(IO80211SkywalkInterface *, apple80211_power_data *data) override {
        if (!data) return kIOReturnBadArgument;
        memset(data, 0, sizeof(*data));
        data->version = APPLE80211_VERSION;
        data->num_radios = 1;
        /* What the radio is, not what was asked: the switch must not claim a
         * radio that failed to start. */
        data->power_state[0] = _hw && _hw->radio_on(_hw->ctx) ? APPLE80211_POWER_ON : APPLE80211_POWER_OFF;
        OSIncrementAtomic(&_powerGets);
        return kIOReturnSuccess;
    }
    IOReturn setPOWER(IO80211SkywalkInterface *, apple80211_power_data *data) override {
        if (!data || !data->num_radios || data->num_radios > APPLE80211_MAX_RADIO) return kIOReturnBadArgument;
        bool on = false;
        for (unsigned i = 0; i < data->num_radios; ++i) {
            if (data->power_state[i] > APPLE80211_POWER_RX) return kIOReturnBadArgument;
            on |= data->power_state[i] != APPLE80211_POWER_OFF;
        }
        OSIncrementAtomic(&_powerSets);
        if (!_hw) return kIOReturnNotReady;
        /* Started on the transport's thread; POWER_CHANGED follows. */
        return _hw->set_power(_hw->ctx, on) ? kIOReturnError : kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(RTW89BridgeController, RTW89BridgeBase)

IOReturn RTW89BridgeInterface::setMacAddress(mloAddrArray &addresses) {
    /* The first address is the station's; the rest are for links not used here. */
    return _owner ? _owner->requestAddress(reinterpret_cast<const uint8_t *>(&addresses)) : kIOReturnNotReady;
}
IOReturn RTW89BridgeInterface::setWCL_SCAN_REQ(apple80211ScanRequest *request) {
    return _owner ? _owner->requestScan(request) : kIOReturnNotReady;
}
IOReturn RTW89BridgeInterface::setWCL_SCAN_ABORT(void *) {
    return _owner ? _owner->abortScan() : kIOReturnNotReady;
}
IOReturn RTW89BridgeInterface::setWCL_ASSOCIATE(apple80211AssocCandidates *request) {
    return _owner ? _owner->requestJoin(request) : kIOReturnNotReady;
}
IOReturn RTW89BridgeInterface::setWCL_JOIN_ABORT(apple80211_wcl_abort_join *) {
    return _owner ? _owner->abortJoin() : kIOReturnNotReady;
}
IOReturn RTW89BridgeInterface::setWCL_LEAVE_NETWORK(apple80211_leave_network *) {
    return _owner ? _owner->requestLeave() : kIOReturnNotReady;
}

/* 6 is what the family takes for "not on a network". */
static const IOReturn kNotAssociated = 6;

IOReturn RTW89BridgeInterface::getCHANNEL(apple80211_channel_data *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    const auto &link = _owner->link();
    if (link.state < RTW89_GLUE_LINK_ASSOCIATED || !link.freq) return kNotAssociated;
    rtw89_glue_channel channel = {};
    for (UInt32 i = 0; i < _channelCount; ++i)
        if (_channels[i].freq == link.freq) channel = _channels[i];
    if (!channel.channel) return kNotAssociated;
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->channel.version = APPLE80211_VERSION;
    data->channel.channel = channel.channel;
    data->channel.flags = rtw89_wcl_channel_flags(&channel);
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getRSSI(apple80211_rssi_data *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    const auto &link = _owner->link();
    if (link.state < RTW89_GLUE_LINK_ASSOCIATED || !link.signal) return kNotAssociated;
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->num_radios = 1;
    data->rssi_unit = APPLE80211_UNIT_DBM;
    data->rssi[0] = data->aggregate_rssi = data->rssi_ext[0] = data->aggregate_rssi_ext = link.signal;
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getRATE(apple80211_rate_data *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    const auto &link = _owner->link();
    const uint32_t mbps = rtw89_wcl_rate_mbps(&link.tx_rate);
    if (link.state < RTW89_GLUE_LINK_ASSOCIATED || !mbps) return kNotAssociated;
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->num_radios = 1;
    data->rate[0] = mbps;
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getMCS(apple80211_mcs_data *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    const auto &link = _owner->link();
    if (link.state < RTW89_GLUE_LINK_ASSOCIATED || !link.tx_rate.kbps || !link.tx_rate.mode) return kNotAssociated;
    data->version = APPLE80211_VERSION;
    data->index = link.tx_rate.mcs;
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getNSS(apple80211_nss_data *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    const auto &link = _owner->link();
    memset(data, 0, sizeof(*data));
    data->version = APPLE80211_VERSION;
    data->nss = link.nss ? link.nss : 2;
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getBEACON_INFO(apple80211_beacon_info_t *data) {
    auto hw = _owner ? _owner->hardware() : nullptr;
    if (!data || !hw) return kIOReturnNotReady;
    auto bytes = reinterpret_cast<uint8_t *>(data);
    bzero(bytes, RTW89_WCL_BEACON_INFO_SIZE);
    const uint32_t length = static_cast<uint32_t>(hw->beacon(hw->ctx, bytes + 4, RTW89_WCL_BEACON_FRAME_MAX));
    if (length < 36) return kIOReturnNotReady;
    memcpy(bytes, &length, sizeof(length));
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getWCL_BSS_INFO(apple80211_beacon_msg *data) {
    auto info = _owner ? _owner->bssInfo() : nullptr;
    if (!data || !info) return kIOReturnNotReady;
    memcpy(data, info, RTW89_WCL_BSS_INFO_SIZE);
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getWCL_EXTENDED_BSS_INFO(apple80211_extended_bss_info *data) {
    auto hw = _owner ? _owner->hardware() : nullptr;
    if (!data || !hw || _owner->link().state < RTW89_GLUE_LINK_ASSOCIATED) return kIOReturnNotReady;
    auto bytes = reinterpret_cast<uint8_t *>(data);
    const uint32_t version = APPLE80211_VERSION;
    const uint16_t unsupported = 0xffff;
    bzero(bytes, RTW89_WCL_EXT_BSS_SIZE);
    /* Rate and MCS sets left empty: the versions say so, and no join needs them. */
    memcpy(bytes, &version, sizeof(version));
    memcpy(bytes + RTW89_WCL_EXT_BSS_MCS, &version, sizeof(version));
    memcpy(bytes + RTW89_WCL_EXT_BSS_VHT, &version, sizeof(version));
    memcpy(bytes + RTW89_WCL_EXT_BSS_VHT + 4, &unsupported, sizeof(unsupported));
    memcpy(bytes + RTW89_WCL_EXT_BSS_HE, &version, sizeof(version));
    memcpy(bytes + RTW89_WCL_EXT_BSS_HE + 4, &unsupported, sizeof(unsupported));
    hw->assoc_rsn_ie(hw->ctx, bytes + RTW89_WCL_EXT_BSS_RSN, RTW89_WCL_EXT_BSS_SIZE - RTW89_WCL_EXT_BSS_RSN);
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getWCL_TRAFFIC_COUNTERS(apple80211_wcl_traffic_counters *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    const auto &link = _owner->link();
    rtw89_wcl_traffic counters = {};
    uint64_t now = 0;
    clock_get_uptime(&now);
    absolutetime_to_nanoseconds(now, &counters.timestamp_ns);
    counters.infra_tx = link.tx_frames;
    counters.infra_rx = link.rx_frames;
    memcpy(data, &counters, sizeof(counters));
    return kIOReturnSuccess;
}
IOReturn RTW89BridgeInterface::getCHANNELS_INFO(apple80211_channels_info *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    auto info = reinterpret_cast<rtw89_wcl_channels *>(data);
    bzero(info, sizeof(*info));
    for (UInt32 i = 0; i < _channelCount; ++i) rtw89_wcl_channels_add(info, &_channels[i]);
    memcpy(info->country, _owner->country(), 3);
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}
IOReturn RTW89BridgeInterface::getWCL_CHANNELS_INFO(apple80211ChannelInfo *data) {
    if (!data || !_owner) return kIOReturnBadArgument;
    auto info = reinterpret_cast<rtw89_wcl_channel_list *>(data);
    bzero(info, sizeof(*info));
    for (UInt32 i = 0; i < _channelCount && info->count < 400; ++i) {
        if (_channels[i].freq >= 5925) continue;
        auto &entry = info->channels[info->count++];
        entry.channel = _channels[i].channel;
        entry.channel_spec = rtw89_wcl_channel_spec(&_channels[i]);
        entry.flags = (_channels[i].radar ? 2 : 0) | (_channels[i].passive ? 4 : 0);
    }
    memcpy(info->country, _owner->country(), 3);
    return info->count ? kIOReturnSuccess : kIOReturnNotReady;
}
