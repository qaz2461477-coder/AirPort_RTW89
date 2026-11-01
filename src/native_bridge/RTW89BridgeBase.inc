/* Derived from the tested 0.2.0 base. Differs in class names, version and the
 * provider, which also owns the card now (RTW89HardwareTransport). GPL-2.0. */
/* SPDX-License-Identifier: GPL-2.0 */
#include <IOKit/IOService.h>
#include <libkern/version.h>
#include <libkern/OSAtomic.h>
struct ifnet_init_eparams;
struct packet_info_tag;
struct apple80211_debug_command;
#include <Airport/macOS26/IO80211ControllerV2.h>
#include <Airport/macOS26/IO80211InfraProtocol.h>
#include <Airport/macOS26/FaultReporter.h>
#include "RTW89HardwareTransport.hpp"
#include "rtw89_wcl_join.h"

#define RTW89_BRIDGE_VERSION "0.7.2"

/* IONetworkController::start compares the child's and provider's work loops.
 * A native IO80211WorkQueue has no dedicated getThread() thread. A shared
 * provider avoids the legacy thread-policy path for an independent workloop.
 * The provider also is the one owner of the card, through the transport. The
 * card is probed before registerService(), so the controller never starts
 * without the real address and firmware to publish. */
class RTW89BridgeProvider : public IOService {
    OSDeclareDefaultStructors(RTW89BridgeProvider)
    IO80211WorkQueue *_queue = nullptr;
    RTW89HardwareTransport *_transport = nullptr;
    /* Checkpoints on the card itself, read by the load journal. */
    static void bridgeStage(void *nub, const char *value) {
        static_cast<IOService *>(nub)->setProperty("RTW89 Bridge Stage", value);
        IOLog("RTW89BridgeProvider: %s\n", value);
        /* Diagnostic delay: let the userspace journal fsync this checkpoint
         * before the next kernel call. No driver lock is held here. */
        IOSleep(500);
    }
    /* On the card, not on this object: it is gone after a failed start, and
     * that is when these are needed. */
    void publishLog(IOService *nub) {
        OSString *log = _transport->copyLog();
        if (log) {
            nub->setProperty("RTW89 Log", log);
            log->release();
        }
    }
    void publishHardware(IOService *nub, const rtw89_glue_info &info) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", info.mac[0], info.mac[1],
                 info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        nub->setProperty("RTW89 Hardware MAC", mac);
        nub->setProperty("RTW89 Firmware Version", info.fw_version);
        nub->setProperty("RTW89 Chip Cut", info.chip_cut, 32);
        nub->setProperty("RTW89 RFE Type", info.rfe_type, 32);
        nub->setProperty("RTW89 TX Streams", info.tx_streams, 32);
        nub->setProperty("RTW89 RX Streams", info.rx_streams, 32);
    }
    void closeHardware() {
        if (!_transport) return;
        _transport->close();
        OSSafeReleaseNULL(_transport);
    }
    bool openHardware(IOService *nub) {
        rtw89_glue_info info = {};
        _transport = OSTypeAlloc(RTW89HardwareTransport);
        if (!_transport || !_transport->init()) {
            OSSafeReleaseNULL(_transport);
            bridgeStage(nub, "hardware-transport-failed");
            return false;
        }
        if (!_transport->open(this, nub, bridgeStage, nub)) {
            nub->setProperty("RTW89 Hardware Error", static_cast<unsigned long long>(_transport->probeError()), 32);
            publishLog(nub);
            closeHardware();
            return false;
        }
        publishLog(nub);
        if (!_transport->ops()->get_info(_transport->ops()->ctx, &info) ||
            !rtw89_hw_info_publishable(&info)) {
            /* Published to see what the probe read; nothing goes to macOS. */
            publishHardware(nub, info);
            bridgeStage(nub, "hardware-identity-rejected");
            closeHardware();
            return false;
        }
        publishHardware(nub, info);
        bridgeStage(nub, "hardware-identity-verified");
        return true;
    }
public:
    IO80211WorkQueue *nativeQueue() const { return _queue; }
    const rtw89_hw_ops *hardwareOps() const { return _transport ? _transport->ops() : nullptr; }
    uint16_t hardwareDevice() const { return _transport ? _transport->deviceID() : 0; }
    /* Refresh the driver's log on the card. A thread that may sleep. */
    void refreshLog() {
        if (_transport && getProvider()) publishLog(getProvider());
    }
    IOWorkLoop *getWorkLoop() const override { return _queue; }
    bool start(IOService *provider) override {
        /* Never start a second controller beside the prior manual tests, and
         * never probe the card twice in one boot: the efuse then reads as zero.
         * The physical test properties disappear on reboot. Module approval
         * may still be requested while the earlier tests are running. */
        if (provider && provider->getProperty("RTW89 Provider Version")) {
            provider->setProperty("RTW89 Bridge Stage", "bridge-reboot-required-before-replacing-tests");
            IOLog("RTW89BridgeProvider: reboot required before controller replacement\n");
            return false;
        }
        if (!provider || version_major != 25 ||
            IO80211WorkQueue::metaClass->getClassSize() != sizeof(IO80211WorkQueue)) return false;
        if (!IOService::start(provider)) return false;
        _queue = IO80211WorkQueue::workQueue();
        if (!_queue) return false;
        setProperty("RTW89 Provider Version", RTW89_BRIDGE_VERSION);
        /* Set before the card is touched: a failed probe needs a reboot too. */
        provider->setProperty("RTW89 Provider Version", RTW89_BRIDGE_VERSION);
        provider->setProperty("RTW89 Native Stage", "provider-shared-queue-ready");
        if (!openHardware(provider)) return false;
        registerService();
        return true;
    }
    void stop(IOService *provider) override {
        /* The controller, a client of this provider, has quiesced by now. */
        closeHardware();
        IOService::stop(provider);
    }
    void free() override {
        closeHardware();
        OSSafeReleaseNULL(_queue);
        IOService::free();
    }
};
OSDefineMetaClassAndStructors(RTW89BridgeProvider, IOService)

/* The native controller's common part. No PCI access, interface attachment or
 * radio control here. Unsupported operations fail explicitly. */
class RTW89BridgeBase : public IO80211Controller {
    OSDeclareDefaultStructors(RTW89BridgeBase)
    IO80211WorkQueue *_queue = nullptr;
    CCPipe *_pipes[3] = {};
    bool _pipeStarted[3] = {};
    CCLogStream *_logger = nullptr;
    CCStream *_faultStream = nullptr;
    CCFaultReporter *_coreFault = nullptr;
    IO80211FaultReporter *_fault = nullptr;
    bool _started = false;
    OSData *_callbackData = nullptr;
    volatile SInt32 *_callbackWords = nullptr;
    enum Callback { WorkLoopCreate, WorkQueueCreate, WorkLoopGet, WorkQueueGet,
                    LoggerGet, FaultReporterGet, LogPipesGet, CallbackCount };
    void callback(Callback which) const {
        /* Preallocated data owns its buffer and is retained by the provider.
         * Getters only increment atomics: no registry locks, allocation or sleep. */
        if (_callbackWords) OSIncrementAtomic(&_callbackWords[which]);
    }
    bool setupCallbackCounters(IOService *provider) {
        SInt32 zero[CallbackCount] = {};
        _callbackData = OSData::withBytes(zero, sizeof(zero));
        if (!_callbackData) return false;
        _callbackWords = const_cast<SInt32 *>(static_cast<const SInt32 *>(_callbackData->getBytesNoCopy()));
        return setProperty("RTW89 Native Callbacks", _callbackData) &&
               provider->setProperty("RTW89 Native Callbacks", _callbackData) &&
               provider->getProvider()->setProperty("RTW89 Native Callbacks", _callbackData);
    }

    void stage(IOService *provider, const char *value) {
        setProperty("RTW89 Native Stage", value);
        if (provider) provider->setProperty("RTW89 Native Stage", value);
        if (provider && provider->getProvider())
            provider->getProvider()->setProperty("RTW89 Native Stage", value);
        IOLog("RTW89BridgeBase: %s\n", value);
        /* Diagnostic delay: let the userspace journal fsync this checkpoint
         * before the next kernel call. No radio/interrupt lock is held here. */
        IOSleep(500);
    }
    bool setupLogs() {
        const char *names[] = {"DriverLogs", "DatapathEvents", "StateSnapshots"};
        for (unsigned i = 0; i < 3; ++i) {
            CCPipeOptions options = {};
            options.pipe_type = i == 2 ? 0x200000001ULL : 0;
            options.log_data_type = i == 0 ? 1 : i == 1 ? 0 : 2;
            options.pipe_size = i == 2 ? 128 : 0x200000;
            options.min_log_size_notify = i == 2 ? 0 : 0xccccc;
            options.notify_threshold = i == 2 ? 0 : 1000;
            strlcpy(options.name, "rtw89-native", sizeof(options.name));
            strlcpy(options.file_name, names[i], sizeof(options.file_name));
            strlcpy(options.directory_name, "WiFi", sizeof(options.directory_name));
            if (i == 0) { options.pad9 = 0x1000000; options.pad10 = 2; }
            if (i == 1) { options.pad9 = 2; options.pad10 = 0x2800000; }
            _pipes[i] = CCPipe::withOwnerNameCapacity(this, "com.rtw89.moderncontroller", names[i], &options);
            if (!_pipes[i]) return false;
            _pipeStarted[i] = _pipes[i]->CCPipe::startPipe();
            if (!_pipeStarted[i]) return false;
        }
        CCStreamOptions options = {};
        CCStream *stream = CCStream::withPipeAndName(_pipes[0], "RTW89Native", &options);
        _logger = OSDynamicCast(CCLogStream, stream);
        if (!_logger) { OSSafeReleaseNULL(stream); return false; }
        options.stream_type = 1;
        options.console_level = ~0ULL;
        _faultStream = CCStream::withPipeAndName(_pipes[2], "FaultReporter", &options);
        CCDataStream *data = OSDynamicCast(CCDataStream, _faultStream);
        if (!data) return false;
        _coreFault = CCFaultReporter::withStreamWorkloop(data, _queue);
        if (!_coreFault) return false;
        _fault = IO80211FaultReporter::allocWithParams(_coreFault);
        return OSDynamicCast(CommonFaultReporter, _fault) != nullptr;
    }
    void releaseLogs() {
        OSSafeReleaseNULL(_fault);
        OSSafeReleaseNULL(_coreFault);
        OSSafeReleaseNULL(_faultStream);
        OSSafeReleaseNULL(_logger);
        for (int i = 2; i >= 0; --i) {
            if (_pipeStarted[i]) { _pipes[i]->CCPipe::stopPipe(); _pipeStarted[i] = false; }
            OSSafeReleaseNULL(_pipes[i]);
        }
    }
public:
    bool createWorkLoop() override { callback(WorkLoopCreate); return createWorkQueue(); }
    bool createWorkQueue() override {
        callback(WorkQueueCreate);
        /* Queue creation belongs to the provider. Never substitute a second
         * native queue here: IONetworkController must see shared ownership. */
        return _queue != nullptr;
    }
    IOWorkLoop *getWorkLoop() const override { callback(WorkLoopGet); return _queue; }
    IO80211WorkQueue *getWorkQueue() const override { callback(WorkQueueGet); return _queue; }
    bool start(IOService *provider) override {
        auto bridge = OSDynamicCast(RTW89BridgeProvider, provider);
        if (!bridge || !bridge->getProvider() || !bridge->nativeQueue() ||
            !setupCallbackCounters(provider)) return false;
        _queue = bridge->nativeQueue();
        _queue->retain();
        setProperty("DriverKitDriver", kOSBooleanFalse);
        stage(provider, "layout-check");
        if (version_major != 25 || IO80211Controller::metaClass->getClassSize() != sizeof(IO80211Controller) ||
            IO80211SkywalkInterface::metaClass->getClassSize() != sizeof(IO80211SkywalkInterface) ||
            IO80211InfraProtocol::metaClass->getClassSize() != sizeof(IO80211InfraProtocol)) return false;
        stage(provider, "workqueue-create");
        if (!createWorkQueue()) return false;
        stage(provider, "queue-routing-check");
        bool shared = provider->getWorkLoop() == _queue;
        setProperty("RTW89 Shared Provider WorkLoop", shared ? kOSBooleanTrue : kOSBooleanFalse);
        bool hasThread = _queue->getThread() != nullptr;
        setProperty("RTW89 Native Queue Has Thread", hasThread ? kOSBooleanTrue : kOSBooleanFalse);
        bridge->getProvider()->setProperty("RTW89 Shared Provider WorkLoop", shared ? kOSBooleanTrue : kOSBooleanFalse);
        bridge->getProvider()->setProperty("RTW89 Native Queue Has Thread", hasThread ? kOSBooleanTrue : kOSBooleanFalse);
        if (!shared) {
            stage(provider, "blocked-unshared-native-workqueue");
            return false;
        }
        stage(provider, "corecapture-create");
        if (!setupLogs()) { releaseLogs(); return false; }
        stage(provider, "native-super-start-enter");
        bool nativeStarted = IO80211Controller::start(provider);
        stage(provider, nativeStarted ? "native-super-start-returned-true" : "native-super-start-returned-false");
        if (!nativeStarted) { releaseLogs(); return false; }
        _started = true;
        stage(provider, "native-controller-ready-no-interface");
        /* Derived controller publishes only after both interfaces are ready. */
        return true;
    }
    void stop(IOService *provider) override {
        if (_started) { IO80211Controller::stop(provider); _started = false; }
        releaseLogs();
    }
    void free() override {
        releaseLogs();
        OSSafeReleaseNULL(_queue);
        _callbackWords = nullptr;
        OSSafeReleaseNULL(_callbackData);
        IO80211Controller::free();
    }
    bool isCommandProhibited(int) override { return true; }
    SInt32 handleCardSpecific(IO80211SkywalkInterface*, unsigned long, void*, bool) override { return kIOReturnUnsupported; }
#define UNSUPPORTED(name, type) IOReturn name(IO80211SkywalkInterface*, type*) override { return kIOReturnUnsupported; }
    UNSUPPORTED(getDRIVER_VERSION, apple80211_version_data)
    UNSUPPORTED(getHARDWARE_VERSION, apple80211_version_data)
    UNSUPPORTED(getCARD_CAPABILITIES, apple80211_capability_data)
    UNSUPPORTED(getPOWER, apple80211_power_data)
    UNSUPPORTED(setPOWER, apple80211_power_data)
    UNSUPPORTED(getCOUNTRY_CODE, apple80211_country_code_data)
    UNSUPPORTED(setCOUNTRY_CODE, apple80211_country_code_data)
    UNSUPPORTED(setGET_DEBUG_INFO, apple80211_debug_command)
#undef UNSUPPORTED
    CCLogStream *getLogger() const override { callback(LoggerGet); return _logger; }
    CommonFaultReporter *getFaultReporterFromDriver() override { callback(FaultReporterGet); return _fault; }
    bool getLogPipes(CCPipe **log, CCPipe **data, CCPipe **snapshot) override {
        callback(LogPipesGet);
        /* The caller may request only one of the three outputs. */
        if (log) *log = _pipes[0];
        if (data) *data = _pipes[1];
        if (snapshot) *snapshot = _pipes[2];
        return (log || data || snapshot) &&
               (!log || _pipes[0]) && (!data || _pipes[1]) &&
               (!snapshot || _pipes[2]);
    }
};
OSDefineMetaClassAndStructors(RTW89BridgeBase, IO80211Controller)
