/* SPDX-License-Identifier: GPL-2.0 */
#include "RTW89RegistrationBase.inc"
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOOutputQueue.h>

static void registrationStage(IOService *controller, const char *value, bool pause = true) {
    controller->setProperty("RTW89 Registration Stage", value);
    auto bridge = controller->getProvider();
    if (bridge && bridge->getProvider())
        bridge->getProvider()->setProperty("RTW89 Registration Stage", value);
    IOLog("RTW89NativeRegistration: %s\n", value);
    if (pause) IOSleep(2500);
}

class RTW89RegistrationInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(RTW89RegistrationInterface)
    IO80211Controller *_controller = nullptr;
    ether_addr _address = {{0x02, 0x52, 0x89, 0x22, 0x00, 0x01}};
public:
    bool initializeInfra(IO80211Controller *controller) {
        _controller = controller;
        return IO80211InfraInterface::init();
    }
    bool initializeSkywalk() {
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
    IOReturn setMacAddress(mloAddrArray &) override { return kIOReturnUnsupported; }
    IOReturn enable(unsigned int) override { return kIOReturnNotReady; }
    IOReturn disable(unsigned int) override { return kIOReturnSuccess; }
    bool isCommandProhibited(int) override { return false; }
    bool isChipInterfaceReady() override { return false; }
    bool isInterfaceEnabled() override { return false; }
    int getAssocState() override { return 0; }
#include "../native_interface/UnsupportedOperations.inc"
};
OSDefineMetaClassAndStructors(RTW89RegistrationInterface, IO80211InfraProtocol)

/* The BSD adapter exposes its associated Skywalk provider once BSD prepare
 * succeeds. This is required by native Skywalk consumers of the BSD object;
 * before attachment it retains the normal controller provider relationship.
 * Adapted from AirportRTW WCL AirportRTWEthernetInterface, qcwap / 钟先耀,
 * copyright 2023, GPL-2.0, pinned source 82c78d0b1c300b25a6706c433170c1c635293670.
 * This version omits the upstream HAL/datapath code. */
class RTW89RegistrationBSD : public IOEthernetInterface {
    OSDeclareDefaultStructors(RTW89RegistrationBSD)
    IO80211SkywalkInterface *_native = nullptr;
    IOCommandGate *_gate = nullptr;
    bool _bsdAttached = false;
    static IOReturn prepareGated(OSObject *, void *arg, void *, void *, void *) {
        auto self = static_cast<RTW89RegistrationBSD *>(arg);
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
        registrationStage(controller, "bsd-data-link-attach-enter", false);
        IOReturn result = IOEthernetInterface::attachToDataLinkLayer(options, parameter);
        if (result != kIOReturnSuccess) {
            registrationStage(controller, "bsd-data-link-attach-failed", false);
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
        registrationStage(controller, "bsd-native-prepare-enter", false);
        result = _gate->runAction(prepareGated, this);
        if (result != kIOReturnSuccess) {
            controller->setProperty("RTW89 BSD Prepare Error", static_cast<unsigned long long>(result), 32);
            registrationStage(controller, "bsd-native-prepare-failed", false);
            IOEthernetInterface::detachFromDataLinkLayer(options, parameter);
            return result;
        }
        _bsdAttached = true;
        _native->registerService();
        controller->setProperty("RTW89 BSD Name", name);
        controller->setProperty("RTW89 BSD Publication Complete", kOSBooleanTrue);
        registrationStage(controller, "native-interface-published-radio-off", false);
        return kIOReturnSuccess;
    }
    void detachFromDataLinkLayer(IOOptionBits options, void *parameter) override {
        IOEthernetInterface::detachFromDataLinkLayer(options, parameter);
        _bsdAttached = false;
    }
    IOService *getProvider() const override {
        return _bsdAttached ? _native : IOEthernetInterface::getProvider();
    }
};
OSDefineMetaClassAndStructors(RTW89RegistrationBSD, IOEthernetInterface)

class RTW89RegistrationController : public RTW89RegistrationBase {
    OSDeclareDefaultStructors(RTW89RegistrationController)
    RTW89RegistrationInterface *_native = nullptr;
    IONetworkInterface *_bsd = nullptr;
    IOCommandGate *_bsdGate = nullptr;
    bool _serviceAttached = false;
    bool _logicalAttached = false;
    bool _nativeStarted = false;
    bool _baseStarted = false;

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
        OSSafeReleaseNULL(medium);
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
    bool start(IOService *provider) override {
        registrationStage(this, "registration-layout-check");
        setProperty("RTW89 BSD Header Size", sizeof(IOEthernetInterface), 32);
        setProperty("RTW89 BSD Runtime Size", IOEthernetInterface::metaClass->getClassSize(), 32);
        if (IOEthernetInterface::metaClass->getClassSize() != sizeof(IOEthernetInterface) ||
            IOSkywalkNetworkInterface::metaClass->getClassSize() != sizeof(IOSkywalkNetworkInterface) ||
            IOSkywalkEthernetInterface::metaClass->getClassSize() != sizeof(IOSkywalkEthernetInterface)) {
            registrationStage(this, "registration-layout-mismatch");
            return false;
        }
        if (!RTW89RegistrationBase::start(provider)) {
            registrationStage(this, "registration-base-start-failed");
            return false;
        }
        _baseStarted = true;
        UInt8 builtIn = 0;
        setProperty("built-in", &builtIn, sizeof(builtIn));
        setProperty("RTW89 Registration Version", "0.2.0");
        if (provider->getProvider()) provider->getProvider()->setProperty("RTW89 Registration Version", "0.2.0");
        setProperty("RTW89 Diagnostic Radio Off", kOSBooleanTrue);
        registrationStage(this, "registration-medium-setup");
        if (!setupMedium()) { registrationStage(this, "registration-medium-failed"); return false; }
        _bsdGate = IOCommandGate::commandGate(this);
        if (!_bsdGate || getWorkQueue()->addEventSource(_bsdGate) != kIOReturnSuccess) {
            registrationStage(this, "registration-bsd-gate-failed"); return false;
        }
        registrationStage(this, "registration-interface-allocation");
        _native = new RTW89RegistrationInterface;
        if (!_native) { registrationStage(this, "registration-allocation-failed"); return false; }
        registrationStage(this, "registration-infra-init");
        if (!_native->initializeInfra(this)) { registrationStage(this, "registration-infra-init-failed"); return false; }
        registrationStage(this, "registration-skywalk-init");
        if (!_native->initializeSkywalk()) { registrationStage(this, "registration-skywalk-init-failed"); return false; }
        _native->setInterfaceRole(1);
        _native->setInterfaceId(1);
        _native->setProperty("RTW89 Diagnostic Radio Off", kOSBooleanTrue);
        registrationStage(this, "registration-service-attach");
        if (!_native->attach(this)) { registrationStage(this, "registration-service-attach-failed"); return false; }
        _serviceAttached = true;
        registrationStage(this, "registration-native-logical-attach");
        if (!IO80211Controller::attachInterface(_native, this)) { registrationStage(this, "registration-logical-attach-failed"); return false; }
        _logicalAttached = true;
        registrationStage(this, "registration-bsd-object-attach");
        if (!IONetworkController::attachInterface(&_bsd, false)) { registrationStage(this, "registration-bsd-object-failed"); return false; }
        registrationStage(this, "registration-info-prepare");
        if (!prepareRegistration()) { registrationStage(this, "registration-info-failed"); return false; }
        registrationStage(this, "registration-native-start");
        if (!_native->start(this)) { registrationStage(this, "registration-native-start-failed"); return false; }
        _nativeStarted = true;
        setLinkStatus(kIONetworkLinkValid);
        registrationStage(this, "registration-bsd-publish-request");
        _bsd->registerService();
        registerService();
        return true;
    }
    void stop(IOService *provider) override {
        cleanup();
        if (_baseStarted) { RTW89RegistrationBase::stop(provider); _baseStarted = false; }
    }
    void free() override {
        cleanup();
        RTW89RegistrationBase::free();
    }
    IONetworkInterface *createInterface() override {
        auto interface = new RTW89RegistrationBSD;
        if (!interface) return nullptr;
        if (!interface->initialize(this, _native, _bsdGate)) { interface->release(); return nullptr; }
        return interface;
    }
    IOReturn getHardwareAddress(IOEthernetAddress *address) override {
        if (!address) return kIOReturnBadArgument;
        const UInt8 bytes[] = {0x02, 0x52, 0x89, 0x22, 0x00, 0x01};
        memcpy(address->bytes, bytes, sizeof(bytes));
        return kIOReturnSuccess;
    }
    const OSString *newVendorString() const override { return OSString::withCString("Realtek"); }
    const OSString *newModelString() const override { return OSString::withCString("RTL8922AE Diagnostic"); }
    bool isCommandProhibited(int) override { return false; }
    IOReturn enable(IONetworkInterface *) override { return kIOReturnNotReady; }
    IOReturn disable(IONetworkInterface *) override { return kIOReturnSuccess; }
    UInt32 outputPacket(mbuf_t packet, void *) override {
        if (packet) mbuf_freem(packet);
        return kIOReturnOutputDropped;
    }
    IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *data) override {
        return versionData(data, "RTW89 native registration 0.2.0");
    }
    IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *data) override {
        return versionData(data, "RTL8922AE diagnostic");
    }
    IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *data) override {
        if (!data) return kIOReturnBadArgument;
        memset(data, 0, sizeof(*data));
        data->version = APPLE80211_VERSION;
        return kIOReturnSuccess;
    }
    IOReturn getPOWER(IO80211SkywalkInterface *, apple80211_power_data *data) override {
        if (!data) return kIOReturnBadArgument;
        memset(data, 0, sizeof(*data));
        data->version = APPLE80211_VERSION;
        data->num_radios = 1;
        return kIOReturnSuccess;
    }
    IOReturn setPOWER(IO80211SkywalkInterface *, apple80211_power_data *data) override {
        if (!data || !data->num_radios || data->num_radios > APPLE80211_MAX_RADIO) return kIOReturnBadArgument;
        for (unsigned i = 0; i < data->num_radios; ++i)
            if (data->power_state[i]) return kIOReturnNotReady;
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(RTW89RegistrationController, RTW89RegistrationBase)
