/* SPDX-License-Identifier: GPL-2.0 */
#include <IOKit/IOService.h>
#include <libkern/version.h>
struct ifnet_init_eparams;
struct packet_info_tag;
struct apple80211_debug_command;
#include <Airport/macOS26/IO80211ControllerV2.h>
#include <Airport/macOS26/IO80211InfraProtocol.h>

/* No BSD registration, native attachInterface, packet queues, radio or keys.
 * A locally administered diagnostic MAC is used only inside this unpublished
 * object. The owning probe keeps its controller provider alive throughout
 * initialization, attachment and teardown. */
class RTW89UnpublishedInterface : public IO80211InfraProtocol {
    OSDeclareDefaultStructors(RTW89UnpublishedInterface)
    IO80211Controller *_controller = nullptr;
    ether_addr _diagnosticAddress = {{0x02, 0x52, 0x89, 0x22, 0x00, 0x01}};
public:
    bool initializeInfra(IO80211Controller *controller) {
        _controller = controller;
        return IO80211InfraInterface::init();
    }
    bool initializeSkywalk() {
        return _controller && IO80211SkywalkInterface::init(_controller, &_diagnosticAddress);
    }
    IO80211Controller *getController() override { return _controller; }
    CCLogStream *getLogger() const override {
        return _controller ? _controller->getLogger() : nullptr;
    }
    IOReturn getHardwareAddress(ether_addr *address) override {
        if (!address) return kIOReturnBadArgument;
        *address = _diagnosticAddress;
        return kIOReturnSuccess;
    }
    IOReturn setMacAddress(mloAddrArray &) override { return kIOReturnUnsupported; }
    IOReturn enable(unsigned int) override { return kIOReturnNotReady; }
    IOReturn disable(unsigned int) override { return kIOReturnNotReady; }
    bool isCommandProhibited(int) override { return true; }
    bool isChipInterfaceReady() override { return false; }
    bool isInterfaceEnabled() override { return false; }
#include "UnsupportedOperations.inc"
};
OSDefineMetaClassAndStructors(RTW89UnpublishedInterface, IO80211InfraProtocol)

class RTW89NativeInterfaceProbe : public IOService {
    OSDeclareDefaultStructors(RTW89NativeInterfaceProbe)
    RTW89UnpublishedInterface *_interface = nullptr;
    IOService *_physical = nullptr;
    bool _attached = false;
    void stage(IOService *provider, const char *value) {
        setProperty("RTW89 Native Interface Stage", value);
        provider->setProperty("RTW89 Native Interface Stage", value);
        if (_physical) _physical->setProperty("RTW89 Native Interface Stage", value);
        IOLog("RTW89NativeInterfaceProbe: %s\n", value);
        IOSleep(2500);
    }
    void releaseInterface(IOService *provider) {
        if (_attached) {
            _interface->detach(provider);
            _attached = false;
        }
        OSSafeReleaseNULL(_interface);
    }
public:
    bool start(IOService *provider) override {
        auto controller = OSDynamicCast(IO80211Controller, provider);
        if (!controller || version_major != 25 || !provider->getProvider() ||
            !provider->getProvider()->getProvider()) return false;
        _physical = provider->getProvider()->getProvider();
        /* Require the known-good shared-queue topology before touching any
         * additional private Apple interface methods. */
        if (provider->getProperty("RTW89 Shared Provider WorkLoop") != kOSBooleanTrue ||
            !controller->getWorkQueue() ||
            controller->getWorkQueue() != provider->getProvider()->getWorkLoop()) return false;
        if (!IOService::start(provider)) return false;
        setProperty("RTW89 Interface Probe Version", "0.1.0");
        _physical->setProperty("RTW89 Interface Probe Version", "0.1.0");
        stage(provider, "interface-layout-check");
        if (IO80211InfraProtocol::metaClass->getClassSize() != sizeof(IO80211InfraProtocol) ||
            IO80211InfraInterface::metaClass->getClassSize() != sizeof(IO80211InfraInterface) ||
            IO80211SkywalkInterface::metaClass->getClassSize() != sizeof(IO80211SkywalkInterface)) return false;
        stage(provider, "interface-allocation-enter");
        _interface = new RTW89UnpublishedInterface;
        if (!_interface) return false;
        stage(provider, "infra-initialization-enter");
        if (!_interface->initializeInfra(controller)) {
            stage(provider, "infra-initialization-failed");
            return false;
        }
        stage(provider, "skywalk-initialization-enter");
        if (!_interface->initializeSkywalk()) {
            stage(provider, "skywalk-initialization-failed");
            return false;
        }
        stage(provider, "interface-role-enter");
        _interface->setInterfaceRole(1);
        _interface->setInterfaceId(1);
        _interface->setProperty("RTW89 Diagnostic Unpublished", kOSBooleanTrue);
        stage(provider, "interface-service-attach-enter");
        if (!_interface->attach(provider)) {
            stage(provider, "interface-service-attach-failed");
            return false;
        }
        _attached = true;
        setProperty("RTW89 No Radio Access", kOSBooleanTrue);
        setProperty("RTW89 BSD Interface Published", kOSBooleanFalse);
        stage(provider, "native-interface-initialized-attached-not-published");
        registerService();
        return true;
    }
    void stop(IOService *provider) override {
        releaseInterface(provider);
        IOService::stop(provider);
    }
    void free() override {
        releaseInterface(getProvider());
        IOService::free();
    }
};
OSDefineMetaClassAndStructors(RTW89NativeInterfaceProbe, IOService)
