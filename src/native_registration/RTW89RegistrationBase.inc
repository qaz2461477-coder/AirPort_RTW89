/* Derived from the tested controller source at bab2716. Only class names,
 * version, prior-test guard and deferred publication differ. GPL-2.0. */
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

/* IONetworkController::start compares the child's and provider's work loops.
 * A native IO80211WorkQueue has no dedicated getThread() thread. A shared
 * provider avoids the legacy thread-policy path for an independent workloop.
 * The provider owns only the queue; it never opens or programs the PCI card. */
class RTW89RegistrationProvider : public IOService {
    OSDeclareDefaultStructors(RTW89RegistrationProvider)
    IO80211WorkQueue *_queue = nullptr;
public:
    IO80211WorkQueue *nativeQueue() const { return _queue; }
    IOWorkLoop *getWorkLoop() const override { return _queue; }
    bool start(IOService *provider) override {
        /* Never start a second controller beside the prior manual tests. The
         * physical test properties disappear on reboot. Module approval may
         * still be requested while the earlier tests are running. */
        if (provider && provider->getProperty("RTW89 Provider Version")) {
            provider->setProperty("RTW89 Registration Stage", "registration-reboot-required-before-replacing-tests");
            IOLog("RTW89RegistrationProvider: reboot required before controller replacement\n");
            return false;
        }
        if (!provider || version_major != 25 ||
            IO80211WorkQueue::metaClass->getClassSize() != sizeof(IO80211WorkQueue)) return false;
        if (!IOService::start(provider)) return false;
        _queue = IO80211WorkQueue::workQueue();
        if (!_queue) return false;
        setProperty("RTW89 No Hardware Access", kOSBooleanTrue);
        setProperty("RTW89 Provider Version", "0.2.0");
        provider->setProperty("RTW89 Provider Version", "0.2.0");
        provider->setProperty("RTW89 Native Stage", "provider-shared-queue-ready");
        registerService();
        return true;
    }
    void free() override {
        OSSafeReleaseNULL(_queue);
        IOService::free();
    }
};
OSDefineMetaClassAndStructors(RTW89RegistrationProvider, IOService)

/* First native-controller bring-up step. No PCI access, interface attachment,
 * radio control, or backend connection. Unsupported operations fail explicitly. */
class RTW89RegistrationBase : public IO80211Controller {
    OSDeclareDefaultStructors(RTW89RegistrationBase)
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
        IOLog("RTW89RegistrationBase: %s\n", value);
        /* Diagnostic delay: let the userspace journal fsync this checkpoint
         * before the next kernel call. No radio/interrupt lock is held here. */
        IOSleep(2500);
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
        auto bridge = OSDynamicCast(RTW89RegistrationProvider, provider);
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
        setProperty("RTW89 No Hardware Access", kOSBooleanTrue);
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
OSDefineMetaClassAndStructors(RTW89RegistrationBase, IO80211Controller)
