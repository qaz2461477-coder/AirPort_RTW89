/* SPDX-License-Identifier: GPL-2.0 */
#include <IOKit/IOService.h>
#include <libkern/version.h>
/* Only opaque pointer parameter types from newer private headers are needed. */
struct ifnet_init_eparams;
struct packet_info_tag;
struct apple80211_debug_command;
#include <Airport/macOS26/IO80211ControllerV2.h>
#include <Airport/macOS26/IO80211InfraProtocol.h>

/* Only inspects class metadata. Never allocates Apple network objects,
 * registers network interfaces, opens PCI devices, or calls network methods.
 * A size match is necessary, but is not proof of complete ABI compatibility. */
class RTW89NativeProbe : public IOService {
    OSDeclareDefaultStructors(RTW89NativeProbe)
public:
    bool start(IOService *provider) override {
        if (version_major != 25 || !IOService::start(provider))
            return false;
        bool same = true;
#define CHECK_CLASS(cls) do { \
        unsigned actual = cls::metaClass->getClassSize(); \
        setProperty(#cls " Header Size", (unsigned long long)sizeof(cls), 32); \
        setProperty(#cls " Runtime Size", (unsigned long long)actual, 32); \
        same = same && actual == sizeof(cls); \
    } while (0)
        CHECK_CLASS(IO80211Controller);
        CHECK_CLASS(IO80211SkywalkInterface);
        CHECK_CLASS(IO80211InfraProtocol);
#undef CHECK_CLASS
        setProperty("Class Sizes Match", same ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("Probe Stage", "metadata-complete");
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(RTW89NativeProbe, IOService)
