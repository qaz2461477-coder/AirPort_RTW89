// SPDX-License-Identifier: GPL-2.0
#include "RTW89HardwareTransport.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <sys/errno.h>

extern "C" {
void rtw89_logring_init(void);
void rtw89_logring_free(void);
size_t rtw89_logring_copy(char *out, size_t max);
}

OSDefineMetaClassAndStructors(RTW89HardwareTransport, OSObject)

#define LOG(fmt, ...) IOLog("RTW89HardwareTransport: " fmt "\n", ##__VA_ARGS__)

bool RTW89HardwareTransport::init()
{
    if (!OSObject::init())
        return false;
    rtw89_logring_init();
    _eventLock = IOLockAlloc();
    _commandLock = IOLockAlloc();
    if (!_eventLock || !_commandLock)
        return false;
    _ops.size = sizeof(_ops);
    _ops.version = RTW89_HW_API_VERSION;
    _ops.ctx = this;
    _ops.get_info = opGetInfo;
    _ops.bind = opBind;
    _ops.set_power = opSetPower;
    _ops.radio_on = opRadioOn;
    _ops.power_error = opPowerError;
    _ops.request_scan = opRequestScan;
    _ops.scan_entry = opScanEntry;
    _ops.channels = opChannels;
    _ops.link = opLink;
    _ops.find_bss = opFindBss;
    _ops.beacon = opBeacon;
    _ops.assoc_rsn_ie = opAssocRsnIe;
    _ops.join = opJoin;
    _ops.join_error = opJoinError;
    _ops.leave = opLeave;
    _ops.set_mac = opSetMac;
    _ops.mac_error = opMacError;
    _ops.tx_alloc = opTxAlloc;
    _ops.tx = opTx;
    _ops.tx_cancel = opTxCancel;
    _ops.tx_room = opTxRoom;
    _ops.quiesce = opQuiesce;
    return true;
}

void RTW89HardwareTransport::free()
{
    close();
    if (_eventLock) {
        IOLockFree(_eventLock);
        _eventLock = nullptr;
    }
    if (_commandLock) {
        IOLockFree(_commandLock);
        _commandLock = nullptr;
    }
    rtw89_logring_free();
    OSObject::free();
}

void RTW89HardwareTransport::report(const char *stage)
{
    LOG("%s", stage);
    if (_stage)
        _stage(_stageContext, stage);
}

/* ------------------------------------------------------------------ */
/*  Platform services for the Linux side                                */
/* ------------------------------------------------------------------ */

uint32_t RTW89HardwareTransport::cfgRead(void *ctx, unsigned int offset, unsigned int width)
{
    IOPCIDevice *pci = static_cast<RTW89HardwareTransport *>(ctx)->_pci;

    /* The extended accessors reach all 4 KB of PCIe config space; the driver
     * uses registers above 0xff (ASPM/L1 controls at 0x7xx). */
    switch (width) {
    case 1:  return pci->extendedConfigRead8(offset);
    case 2:  return pci->extendedConfigRead16(offset);
    default: return pci->extendedConfigRead32(offset);
    }
}

void RTW89HardwareTransport::cfgWrite(void *ctx, unsigned int offset, unsigned int width,
                                      uint32_t value)
{
    IOPCIDevice *pci = static_cast<RTW89HardwareTransport *>(ctx)->_pci;

    switch (width) {
    case 1:  pci->extendedConfigWrite8(offset, (UInt8)value); break;
    case 2:  pci->extendedConfigWrite16(offset, (UInt16)value); break;
    default: pci->extendedConfigWrite32(offset, value); break;
    }
}

/*
 * Wired, physically contiguous, below 4 GB. The physical address is used as
 * the bus address, which is right as long as no IOMMU remaps this device: the
 * case on this machine (macOS has no AMD-Vi support).
 */
void *RTW89HardwareTransport::dmaAlloc(void *ctx, size_t size, uint64_t *busAddr, void **cookie)
{
    IOBufferMemoryDescriptor *desc;
    IOByteCount segLen = 0;
    addr64_t phys;

    desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        size, 0x00000000FFFFF000ULL);
    if (!desc)
        return nullptr;

    if (desc->prepare() != kIOReturnSuccess) {
        desc->release();
        return nullptr;
    }

    phys = desc->getPhysicalSegment(0, &segLen);
    if (!phys || segLen < size) {
        desc->complete();
        desc->release();
        return nullptr;
    }

    *busAddr = phys;
    *cookie = desc;
    return desc->getBytesNoCopy();
}

void RTW89HardwareTransport::dmaFree(void *ctx, void *cookie)
{
    IOBufferMemoryDescriptor *desc = static_cast<IOBufferMemoryDescriptor *>(cookie);

    desc->complete();
    desc->release();
}

void RTW89HardwareTransport::irqEnable(void *ctx, bool enable)
{
    IOInterruptEventSource *source = static_cast<RTW89HardwareTransport *>(ctx)->_interrupt;

    if (!source)
        return;
    if (enable)
        source->enable();
    else
        source->disable();
}

/* ------------------------------------------------------------------ */
/*  Claiming the card                                                   */
/* ------------------------------------------------------------------ */

bool RTW89HardwareTransport::mapDevice()
{
    /* A device nobody has driven yet may have been left in D3. */
    IOByteCount pmCap = 0;

    if (_pci->extendedFindPCICapability(kIOPCIPowerManagementCapability, &pmCap) && pmCap) {
        UInt16 pmcsr = _pci->extendedConfigRead16(pmCap + 4);

        if (pmcsr & 0x3) {
            LOG("device was in D%u, moving it to D0", pmcsr & 0x3);
            _pci->extendedConfigWrite16(pmCap + 4, pmcsr & ~0x3);
            IOSleep(20);
        }
    }

    _pci->setMemoryEnable(true);
    _pci->setBusMasterEnable(true);

    /* rtw89 keeps its registers in BAR 2. */
    _mmio = _pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
    if (!_mmio)
        return false;
    LOG("BAR 2 mapped, %llu bytes", (unsigned long long)_mmio->getLength());
    return true;
}

bool RTW89HardwareTransport::setupInterrupt()
{
    int index = 0, msiIndex = -1, type = 0;

    /* Prefer MSI: the legacy line is often not routed on these boards. */
    while (_pci->getInterruptType(index, &type) == kIOReturnSuccess) {
        if (type & kIOInterruptTypePCIMessaged) {
            msiIndex = index;
            break;
        }
        index++;
    }

    _workLoop = IOWorkLoop::workLoop();
    if (!_workLoop)
        return false;

    /* The action runs on the work loop thread, not at interrupt level, which
     * is what the driver's handler needs (it takes sleeping locks). */
    _interrupt = IOInterruptEventSource::interruptEventSource(
        this,
        OSMemberFunctionCast(IOInterruptEventSource::Action, this,
                             &RTW89HardwareTransport::interruptOccurred),
        _pci, msiIndex >= 0 ? msiIndex : 0);
    if (!_interrupt)
        return false;

    if (_workLoop->addEventSource(_interrupt) != kIOReturnSuccess)
        return false;

    LOG("using %s interrupt (index %d)", msiIndex >= 0 ? "MSI" : "legacy",
        msiIndex >= 0 ? msiIndex : 0);
    return true;
}

void RTW89HardwareTransport::interruptOccurred(IOInterruptEventSource *source, int count)
{
    rtw89_glue_interrupt();
}

bool RTW89HardwareTransport::open(IOService *client, IOService *nub, Stage stage,
                                  void *stageContext)
{
    struct rtw89_glue_platform platform = {};
    struct rtw89_glue_device device = {};

    if (_client || _pci)
        return false;
    _stage = stage;
    _stageContext = stageContext;

    _pci = OSDynamicCast(IOPCIDevice, nub);
    if (!_pci) {
        report("hardware-provider-not-pci");
        return false;
    }
    _pci->retain();

    device.vendor = _pci->configRead16(kIOPCIConfigVendorID);
    device.device = _pci->configRead16(kIOPCIConfigDeviceID);
    device.subsystem_vendor = _pci->configRead16(kIOPCIConfigSubSystemVendorID);
    device.subsystem_device = _pci->configRead16(kIOPCIConfigSubSystemID);
    device.revision = _pci->configRead8(kIOPCIConfigRevisionID);
    _deviceID = device.device;
    LOG("found %04x:%04x (subsystem %04x:%04x, rev %02x)", device.vendor, device.device,
        device.subsystem_vendor, device.subsystem_device, device.revision);
    if (!rtw89_glue_supports(device.vendor, device.device)) {
        report("hardware-device-not-supported");
        close();
        return false;
    }

    report("hardware-pci-open");
    if (!_pci->open(client)) {
        report("hardware-pci-open-failed");
        close();
        return false;
    }
    _client = client;

    report("hardware-pci-map");
    if (!mapDevice()) {
        report("hardware-pci-map-failed");
        close();
        return false;
    }
    device.mmio_base = reinterpret_cast<volatile void *>(_mmio->getVirtualAddress());
    device.mmio_len = _mmio->getLength();

    report("hardware-interrupt-setup");
    if (!setupInterrupt()) {
        report("hardware-interrupt-setup-failed");
        close();
        return false;
    }

    _eventCall = thread_call_allocate(eventsCall, this);
    _powerCall = thread_call_allocate(powerCall, this);
    _tickCall = thread_call_allocate(tickCall, this);
    if (!_eventCall || !_powerCall || !_tickCall) {
        report("hardware-event-thread-failed");
        close();
        return false;
    }

    platform.ctx = this;
    platform.cfg_read = cfgRead;
    platform.cfg_write = cfgWrite;
    platform.dma_alloc = dmaAlloc;
    platform.dma_free = dmaFree;
    platform.irq_enable = irqEnable;
    platform.link_changed = linkChanged;
    platform.scan_done = scanDone;
    platform.rx_frame = rxFrame;
    platform.tx_wake = txWake;

    report("hardware-probe-enter");
    _probeError = rtw89_glue_probe(&platform, &device);
    if (_probeError) {
        LOG("probe failed: %d", _probeError);
        report("hardware-probe-failed");
        close();
        return false;
    }
    _probed = true;
    report("hardware-probe-complete");
    /* macOS decides when to join and when to join again. */
    rtw89_glue_set_rejoin(false);
    /* A chip left running goes on through a restart into the firmware and the
     * next boot, which then hangs. */
    _powerNotifier = registerPrioritySleepWakeInterest(powerEvent, this);
    /* The interrupt source stays disabled: probe leaves the chip powered down.
     * The glue enables it (irqEnable) when the radio is brought up. */
    return true;
}

void RTW89HardwareTransport::close()
{
    quiesceEvents();
    stopPower();

    if (_probed) {
        _probed = false;
        /* Brings the radio down first if it is up. */
        rtw89_glue_remove();
        /* The compat workqueue threads signal that they are done just before
         * they exit; let them get out of this kext's code before it can be
         * unloaded. */
        IOSleep(200);
    }

    /* The driver is gone, so nothing can enter the call any more. */
    if (_eventCall) {
        if (thread_call_cancel(_eventCall))
            OSDecrementAtomic(&_eventRuns);
        while (_eventRuns)
            IOSleep(1);
        thread_call_free(_eventCall);
        _eventCall = nullptr;
    }

    if (_interrupt) {
        _interrupt->disable();
        if (_workLoop)
            _workLoop->removeEventSource(_interrupt);
        _interrupt->release();
        _interrupt = nullptr;
    }
    if (_workLoop) {
        _workLoop->release();
        _workLoop = nullptr;
    }
    if (_mmio) {
        _mmio->release();
        _mmio = nullptr;
    }
    if (_pci) {
        if (_client) {
            _pci->setBusMasterEnable(false);
            _pci->close(_client);
            _client = nullptr;
        }
        _pci->release();
        _pci = nullptr;
    }
}

OSString *RTW89HardwareTransport::copyLog() const
{
    const size_t max = 64 * 1024;
    char *text = static_cast<char *>(IOMallocZero(max + 1));
    OSString *log;

    if (!text)
        return nullptr;
    text[rtw89_logring_copy(text, max)] = 0;
    log = OSString::withCString(text);
    IOFree(text, max + 1);
    return log;
}

/* ------------------------------------------------------------------ */
/*  Towards the front                                                   */
/* ------------------------------------------------------------------ */

bool RTW89HardwareTransport::opGetInfo(void *ctx, struct rtw89_glue_info *info)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed && rtw89_glue_get_info(info);
}

int RTW89HardwareTransport::opBind(void *ctx, const struct rtw89_hw_front *front)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    if (!rtw89_hw_front_compatible(front))
        return -EINVAL;
    if (!me->_probed)
        return -ENODEV;
    if (me->_bound || me->_eventsClosed)
        return -EBUSY;
    memcpy(&me->_front, front, sizeof(me->_front));
    me->_bound = true;
    me->armTick();
    return 0;
}

int RTW89HardwareTransport::opSetPower(void *ctx, bool on)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    if (!me->_probed || me->_eventsClosed)
        return -ENODEV;
    me->_powerWanted = on;
    me->_powerPending = 1;
    me->queueCommand();
    return 0;
}

int RTW89HardwareTransport::opRequestScan(void *ctx)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    if (!me->_probed || me->_eventsClosed)
        return -ENODEV;
    me->_scanPending = 1;
    me->queueCommand();
    return 0;
}

bool RTW89HardwareTransport::opScanEntry(void *ctx, unsigned int index, struct rtw89_glue_bss *out,
                                         uint8_t *ies, size_t ies_max, size_t *ies_len,
                                         uint16_t *beacon_int)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed &&
           rtw89_glue_scan_entry(index, out, ies, ies_max, ies_len, beacon_int);
}

unsigned int RTW89HardwareTransport::opChannels(void *ctx, struct rtw89_glue_channel *out,
                                                unsigned int max)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed ? rtw89_glue_channels(out, max) : 0;
}

void RTW89HardwareTransport::opLink(void *ctx, struct rtw89_glue_link *link)
{
    memset(link, 0, sizeof(*link));
    if (static_cast<RTW89HardwareTransport *>(ctx)->_probed)
        rtw89_glue_link(link);
}

bool RTW89HardwareTransport::opFindBss(void *ctx, const uint8_t bssid[6], struct rtw89_glue_bss *out,
                                       uint8_t *ies, size_t ies_max, size_t *ies_len,
                                       uint16_t *beacon_int)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed &&
           rtw89_glue_find_bss(bssid, out, ies, ies_max, ies_len, beacon_int);
}

size_t RTW89HardwareTransport::opBeacon(void *ctx, uint8_t *frame, size_t max)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed ? rtw89_glue_beacon(frame, max) : 0;
}

size_t RTW89HardwareTransport::opAssocRsnIe(void *ctx, uint8_t *ie, size_t max)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed ? rtw89_glue_assoc_rsn_ie(ie, max) : 0;
}

int RTW89HardwareTransport::opJoin(void *ctx, const uint8_t *ssid, size_t ssid_len,
                                   const uint8_t bssid[6], const char *passphrase,
                                   size_t passphrase_len, const uint8_t *pmk)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    if (!me->_probed || me->_eventsClosed)
        return -ENODEV;
    if (ssid_len > sizeof(me->_join.ssid) || passphrase_len > sizeof(me->_join.passphrase))
        return -EINVAL;
    /* The front allows one join at a time, and the command thread only reads
     * the slot once this is set. */
    if (me->_joinPending)
        return -EBUSY;
    memcpy(me->_join.ssid, ssid, ssid_len);
    me->_join.ssid_len = ssid_len;
    memcpy(me->_join.bssid, bssid, sizeof(me->_join.bssid));
    me->_join.passphrase_len = passphrase ? passphrase_len : 0;
    if (passphrase)
        memcpy(me->_join.passphrase, passphrase, passphrase_len);
    me->_join.have_pmk = pmk != nullptr;
    if (pmk)
        memcpy(me->_join.pmk, pmk, sizeof(me->_join.pmk));
    me->_joinError = 0;
    me->_joinPending = 1;
    me->queueCommand();
    return 0;
}

int RTW89HardwareTransport::opJoinError(void *ctx)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_joinError;
}

void RTW89HardwareTransport::opLeave(void *ctx)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    if (!me->_probed || me->_eventsClosed)
        return;
    /* A join still waiting is what is being left: the command thread runs a
     * leave before a join, so it would otherwise go ahead. */
    if (OSCompareAndSwap(1, 0, &me->_joinPending))
        bzero(&me->_join, sizeof(me->_join));
    me->_leavePending = 1;
    me->queueCommand();
}

int RTW89HardwareTransport::opSetMac(void *ctx, const uint8_t mac[6])
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    if (!me->_probed || me->_eventsClosed)
        return -ENODEV;
    memcpy(me->_mac, mac, sizeof(me->_mac));
    me->_macError = 0;
    me->_macPending = 1;
    me->queueCommand();
    return 0;
}

int RTW89HardwareTransport::opMacError(void *ctx)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_macError;
}

void *RTW89HardwareTransport::opTxAlloc(void *ctx, size_t len, uint8_t **frame)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed ? rtw89_glue_tx_alloc(len, frame) : nullptr;
}

int RTW89HardwareTransport::opTx(void *ctx, void *handle)
{
    return rtw89_glue_tx(handle);
}

void RTW89HardwareTransport::opTxCancel(void *ctx, void *handle)
{
    rtw89_glue_tx_cancel(handle);
}

unsigned int RTW89HardwareTransport::opTxRoom(void *ctx)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed ? rtw89_glue_tx_room() : 0;
}

bool RTW89HardwareTransport::opRadioOn(void *ctx)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_probed && rtw89_glue_is_up();
}

int RTW89HardwareTransport::opPowerError(void *ctx)
{
    return static_cast<RTW89HardwareTransport *>(ctx)->_powerError;
}

void RTW89HardwareTransport::opQuiesce(void *ctx)
{
    static_cast<RTW89HardwareTransport *>(ctx)->quiesceEvents();
}

void RTW89HardwareTransport::quiesceEvents()
{
    /* A run that starts from now on delivers nothing, and taking the lock
     * waits for one that is delivering. */
    _eventsClosed = true;
    if (!_eventLock)
        return;
    while (_dataBusy)
        IOSleep(1);
    IOLockLock(_eventLock);
    _bound = false;
    memset(&_front, 0, sizeof(_front));
    IOLockUnlock(_eventLock);
}

/* The counter goes up first: quiesceEvents() sets the flag, then waits for it. */
void RTW89HardwareTransport::rxFrame(void *ctx, const uint8_t *frame, size_t len)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    OSIncrementAtomic(&me->_dataBusy);
    if (me->_bound && !me->_eventsClosed)
        me->_front.rx_frame(me->_front.ctx, frame, len);
    OSDecrementAtomic(&me->_dataBusy);
}

void RTW89HardwareTransport::txWake(void *ctx)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(ctx);

    OSIncrementAtomic(&me->_dataBusy);
    if (me->_bound && !me->_eventsClosed)
        me->_front.tx_wake(me->_front.ctx);
    OSDecrementAtomic(&me->_dataBusy);
}

void RTW89HardwareTransport::powerCall(thread_call_param_t self, thread_call_param_t)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(self);

    me->applyCommands();
    OSDecrementAtomic(&me->_powerRuns);
}

void RTW89HardwareTransport::queueCommand()
{
    OSIncrementAtomic(&_powerRuns);
    /* true: one is queued already, and it reads the latest wish */
    if (thread_call_enter(_powerCall))
        OSDecrementAtomic(&_powerRuns);
}

/* Bring the radio to what was last asked for and start a scan if one was asked
 * for, unless the machine is going down or asleep. Two runs may overlap: the
 * lock orders them. */
void RTW89HardwareTransport::applyCommands()
{
    bool power, scan, join, leave, mac;
    int scanError = 0, joinError = 0;

    IOLockLock(_commandLock);
    power = OSCompareAndSwap(1, 0, &_powerPending);
    scan = OSCompareAndSwap(1, 0, &_scanPending);
    join = OSCompareAndSwap(1, 0, &_joinPending);
    leave = OSCompareAndSwap(1, 0, &_leavePending);
    mac = OSCompareAndSwap(1, 0, &_macPending);
    const bool ready = _probed && !_halting && !_asleep;

    if (power && ready) {
        bool on = _powerWanted;

        if (on && !rtw89_glue_is_up()) {
            _powerError = rtw89_glue_up();
            LOG("radio up: %d", _powerError);
        } else if (!on && rtw89_glue_is_up()) {
            rtw89_glue_down();
            _powerError = 0;
            LOG("radio down");
        }
    }
    if (mac) {
        _macError = ready ? rtw89_glue_set_mac(_mac) : -ENETDOWN;
        LOG("address change: %d", _macError);
    }
    if (leave && ready) {
        rtw89_glue_leave();
        LOG("left the network");
    }
    if (scan) {
        scanError = ready && rtw89_glue_is_up() ? rtw89_glue_scan() : -ENETDOWN;
        LOG("scan requested: %d", scanError);
    }
    if (join) {
        joinError = ready && rtw89_glue_is_up() ? runJoin() : -ENETDOWN;
        bzero(&_join, sizeof(_join));
        LOG("join requested: %d", joinError);
    }
    IOLockUnlock(_commandLock);
    if (power)
        notify(RTW89_HW_EVENT_POWER);
    if (mac)
        notify(RTW89_HW_EVENT_MAC);
    /* Otherwise the driver's scan_done reports the end. */
    if (scanError)
        notify(RTW89_HW_EVENT_SCAN_ABORTED);
    if (joinError) {
        _joinError = joinError;
        notify(RTW89_HW_EVENT_JOIN_FAILED);
    }
}

/* With the command lock held. The derivation takes a while, which is why this
 * is not done in macOS's request. */
int RTW89HardwareTransport::runJoin()
{
    uint8_t derived[32];
    const uint8_t *pmk = nullptr;
    int ret = 0;

    if (_join.passphrase_len) {
        ret = rtw89_glue_derive_pmk(_join.ssid, _join.ssid_len, _join.passphrase,
                                    _join.passphrase_len, derived);
        pmk = derived;
    } else if (_join.have_pmk) {
        pmk = _join.pmk;
    }
    if (!ret)
        ret = rtw89_glue_join_pmk(_join.ssid, _join.ssid_len, _join.bssid, pmk);
    /* The list of networks went with the last scan: look again, once. */
    if (ret == -ENOENT && !rtw89_glue_scan()) {
        for (int i = 0; i < 150 && rtw89_glue_scanning(); i++)
            IOSleep(100);
        ret = rtw89_glue_join_pmk(_join.ssid, _join.ssid_len, _join.bssid, pmk);
    }
    bzero(derived, sizeof(derived));
    return ret;
}

void RTW89HardwareTransport::tickCall(thread_call_param_t self, thread_call_param_t)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(self);

    me->notify(RTW89_HW_EVENT_TICK);
    me->armTick();
    OSDecrementAtomic(&me->_powerRuns);
}

void RTW89HardwareTransport::armTick()
{
    uint64_t deadline;

    if (_eventsClosed || !_tickCall)
        return;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    OSIncrementAtomic(&_powerRuns);
    if (thread_call_enter_delayed(_tickCall, deadline))
        OSDecrementAtomic(&_powerRuns);
}

void RTW89HardwareTransport::stopPower()
{
    if (_powerNotifier) {
        _powerNotifier->remove();
        _powerNotifier = nullptr;
    }
    if (_tickCall && thread_call_cancel(_tickCall))
        OSDecrementAtomic(&_powerRuns);
    if (_powerCall && thread_call_cancel(_powerCall))
        OSDecrementAtomic(&_powerRuns);
    while (_powerRuns)
        IOSleep(1);
    if (_tickCall) {
        thread_call_free(_tickCall);
        _tickCall = nullptr;
    }
    if (_powerCall) {
        thread_call_free(_powerCall);
        _powerCall = nullptr;
    }
    bzero(&_join, sizeof(_join));
}

/*
 * Shutting down and restarting do not stop drivers, so the chip is brought down
 * here. Sleep takes its power away, and with it the firmware and every register:
 * the radio goes down before and comes back after.
 */
IOReturn RTW89HardwareTransport::powerEvent(void *target, void *refCon, UInt32 messageType,
                                            IOService *provider, void *messageArgument,
                                            vm_size_t argSize)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(target);

    if (messageType == kIOMessageSystemWillSleep) {
        me->sleepRadio();
    } else if (messageType == kIOMessageSystemHasPoweredOn) {
        me->wakeRadio();
    } else if (messageType == kIOMessageSystemWillPowerOff ||
               messageType == kIOMessageSystemWillRestart) {
        me->_halting = true;
        IOLockLock(me->_commandLock);
        if (me->_probed)
            rtw89_glue_down();
        IOLockUnlock(me->_commandLock);
        LOG("the machine is %s: radio off", messageType == kIOMessageSystemWillRestart ?
            "restarting" : "shutting down");
    }
    return kIOReturnSuccess;
}

void RTW89HardwareTransport::sleepRadio()
{
    IOLockLock(_commandLock);
    /* from here on a request from macOS cannot bring the radio up */
    _asleep = true;
    _upBeforeSleep = _probed && rtw89_glue_is_up();
    if (_upBeforeSleep)
        rtw89_glue_suspend();
    IOLockUnlock(_commandLock);
    LOG("going to sleep: radio %s", _upBeforeSleep ? "off" : "was already off");
    if (_upBeforeSleep)
        notify(RTW89_HW_EVENT_POWER);
}

void RTW89HardwareTransport::wakeRadio()
{
    if (!_asleep)
        return;
    IOLockLock(_commandLock);
    _asleep = false;
    if (_upBeforeSleep) {
        _powerError = rtw89_glue_resume();
        LOG("awake: radio on: %d", _powerError);
    }
    IOLockUnlock(_commandLock);
    if (_upBeforeSleep)
        notify(RTW89_HW_EVENT_POWER);
}

/* Any context, driver locks held: only records the event. */
void RTW89HardwareTransport::notify(uint32_t bits)
{
    if (_eventsClosed || !_eventCall)
        return;
    OSBitOrAtomic(bits, &_pendingEvents);
    OSIncrementAtomic(&_eventRuns);
    /* true: one is queued already, and it takes these bits too */
    if (thread_call_enter(_eventCall))
        OSDecrementAtomic(&_eventRuns);
}

void RTW89HardwareTransport::linkChanged(void *ctx)
{
    static_cast<RTW89HardwareTransport *>(ctx)->notify(RTW89_HW_EVENT_LINK);
}

void RTW89HardwareTransport::scanDone(void *ctx, bool aborted)
{
    static_cast<RTW89HardwareTransport *>(ctx)->notify(aborted ? RTW89_HW_EVENT_SCAN_ABORTED :
                                                                 RTW89_HW_EVENT_SCAN_DONE);
}

void RTW89HardwareTransport::eventsCall(thread_call_param_t self, thread_call_param_t)
{
    RTW89HardwareTransport *me = static_cast<RTW89HardwareTransport *>(self);

    me->deliverEvents();
    OSDecrementAtomic(&me->_eventRuns);
}

void RTW89HardwareTransport::deliverEvents()
{
    UInt32 bits;

    IOLockLock(_eventLock);
    do {
        bits = _pendingEvents;
    } while (!OSCompareAndSwap(bits, 0, &_pendingEvents));
    if (bits && _bound && !_eventsClosed)
        _front.events(_front.ctx, bits);
    IOLockUnlock(_eventLock);
}
