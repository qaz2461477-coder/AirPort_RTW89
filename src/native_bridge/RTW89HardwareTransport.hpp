/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RTW89HardwareTransport: the one owner of the card in the native Wi-Fi kext.
 *
 * What AirPort_RTW89 does for the Linux side (config space, BAR 2, DMA memory,
 * the interrupt) without its Ethernet interface or its legacy front. The
 * network objects belong to the front, which reaches this through the table in
 * rtw89_hw_bridge.h. Nothing here knows about IO80211, so this file compiles
 * without the Tahoe headers.
 *
 * The platform services are copied from src/kext/AirPortRTW89.cpp rather than
 * shared with it: that file is the hardware-proven Ethernet baseline and stays
 * as it was tested.
 */
#pragma once

#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <kern/thread_call.h>

#include "rtw89_hw_bridge.h"

class IOInterruptEventSource;
class IOMemoryMap;
class IOPCIDevice;
class IOWorkLoop;

class RTW89HardwareTransport : public OSObject {
    OSDeclareDefaultStructors(RTW89HardwareTransport)

public:
    /* A checkpoint on the way through open(), for the load journal. */
    typedef void (*Stage)(void *ctx, const char *stage);

    /*
     * Claim @nub for @client and run the driver's probe: power on, firmware,
     * efuse, power off again. The radio stays off until the front asks. Sleeps
     * for seconds; call it from start(). False leaves nothing behind.
     */
    bool open(IOService *client, IOService *nub, Stage stage, void *stageContext);
    /* Undo open(). After this the card is not to be probed again this boot. */
    void close();

    const struct rtw89_hw_ops *ops() const { return &_ops; }
    /* The negative errno of a failed probe, otherwise 0. */
    int probeError() const { return _probeError; }
    uint16_t deviceID() const { return _deviceID; }
    /* The driver's log ring so far. The caller releases it. */
    OSString *copyLog() const;

    bool init() override;
    void free() override;

private:
    /* rtw89_glue_platform; ctx is the instance. */
    static uint32_t cfgRead(void *ctx, unsigned int offset, unsigned int width);
    static void cfgWrite(void *ctx, unsigned int offset, unsigned int width, uint32_t value);
    static void *dmaAlloc(void *ctx, size_t size, uint64_t *busAddr, void **cookie);
    static void dmaFree(void *ctx, void *cookie);
    static void irqEnable(void *ctx, bool enable);
    static void linkChanged(void *ctx);
    static void scanDone(void *ctx, bool aborted);

    /* rtw89_hw_ops; ctx is the instance. */
    static bool opGetInfo(void *ctx, struct rtw89_glue_info *info);
    static int opBind(void *ctx, const struct rtw89_hw_front *front);
    static int opSetPower(void *ctx, bool on);
    static bool opRadioOn(void *ctx);
    static int opPowerError(void *ctx);
    static int opRequestScan(void *ctx);
    static bool opScanEntry(void *ctx, unsigned int index, struct rtw89_glue_bss *out, uint8_t *ies,
                            size_t ies_max, size_t *ies_len, uint16_t *beacon_int);
    static unsigned int opChannels(void *ctx, struct rtw89_glue_channel *out, unsigned int max);
    static void opLink(void *ctx, struct rtw89_glue_link *link);
    static bool opFindBss(void *ctx, const uint8_t bssid[6], struct rtw89_glue_bss *out, uint8_t *ies,
                          size_t ies_max, size_t *ies_len, uint16_t *beacon_int);
    static size_t opBeacon(void *ctx, uint8_t *frame, size_t max);
    static size_t opAssocRsnIe(void *ctx, uint8_t *ie, size_t max);
    static int opJoin(void *ctx, const uint8_t *ssid, size_t ssid_len, const uint8_t bssid[6],
                      const char *passphrase, size_t passphrase_len, const uint8_t *pmk);
    static int opJoinError(void *ctx);
    static void opLeave(void *ctx);
    static int opSetMac(void *ctx, const uint8_t mac[6]);
    static int opMacError(void *ctx);
    static void *opTxAlloc(void *ctx, size_t len, uint8_t **frame);
    static int opTx(void *ctx, void *handle);
    static void opTxCancel(void *ctx, void *handle);
    static unsigned int opTxRoom(void *ctx);
    static void rxFrame(void *ctx, const uint8_t *frame, size_t len);
    static void txWake(void *ctx);
    static void opQuiesce(void *ctx);

    bool mapDevice();
    bool setupInterrupt();
    void interruptOccurred(class IOInterruptEventSource *source, int count);
    void report(const char *stage);

    void notify(uint32_t bits);
    void deliverEvents();
    static void eventsCall(thread_call_param_t self, thread_call_param_t);
    void quiesceEvents();

    static void powerCall(thread_call_param_t self, thread_call_param_t);
    void queueCommand();
    void applyCommands();
    int runJoin();
    static void tickCall(thread_call_param_t self, thread_call_param_t);
    void armTick();
    void stopPower();
    static IOReturn powerEvent(void *target, void *refCon, UInt32 messageType,
                               IOService *provider, void *messageArgument, vm_size_t argSize);
    void sleepRadio();
    void wakeRadio();

    struct rtw89_hw_ops _ops = {};

    Stage _stage = nullptr;
    void *_stageContext = nullptr;
    int _probeError = 0;

    IOService *_client = nullptr;       /* set while @_pci is open for it */
    IOPCIDevice *_pci = nullptr;
    uint16_t _deviceID = 0;
    IOMemoryMap *_mmio = nullptr;
    /* The interrupt's own thread. Never the native queue: it has no thread of
     * its own (IO80211WorkQueue::getThread() returns 0) and the handler takes
     * sleeping locks. */
    IOWorkLoop *_workLoop = nullptr;
    IOInterruptEventSource *_interrupt = nullptr;
    bool _probed = false;

    /* The driver reports events with its locks held, and the front may be
     * waiting for those very locks inside a request of its own: they reach it
     * from this thread call instead. */
    thread_call_t _eventCall = nullptr;
    /* Held across a delivery: deliveries come one at a time, and taking it
     * waits for one that is running. thread_call_cancel_wait() would say the
     * same, but it is private KPI. */
    IOLock *_eventLock = nullptr;
    volatile UInt32 _pendingEvents = 0;
    /* Runs of the call that are queued or under way: the last thing a run
     * does is count itself out, so at zero the object can go. */
    volatile SInt32 _eventRuns = 0;
    volatile bool _eventsClosed = false;
    struct rtw89_hw_front _front = {};
    bool _bound = false;

    /* Radio power and scans. rtw89_glue_up() and _down() sleep for seconds and
     * are not reentrant, and a scan waits for them: they all run on this call,
     * under this lock, and the system's power messages take the lock too. */
    thread_call_t _powerCall = nullptr;
    IOLock *_commandLock = nullptr;
    volatile SInt32 _powerRuns = 0;
    volatile bool _powerWanted = false;
    volatile UInt32 _powerPending = 0;
    volatile UInt32 _scanPending = 0;
    /* One join waiting for the command thread; holds the secret until then. */
    volatile UInt32 _joinPending = 0;
    volatile UInt32 _leavePending = 0;
    volatile UInt32 _macPending = 0;
    uint8_t _mac[6] = {};
    volatile int _macError = 0;
    volatile int _joinError = 0;
    struct {
        uint8_t ssid[32];
        size_t  ssid_len;
        uint8_t bssid[6];
        char    passphrase[64];
        size_t  passphrase_len;
        uint8_t pmk[32];
        bool    have_pmk;
    } _join = {};
    /* Threads inside the front's rx_frame or tx_wake. */
    volatile SInt32 _dataBusy = 0;
    thread_call_t _tickCall = nullptr;
    volatile int _powerError = 0;
    IONotifier *_powerNotifier = nullptr;
    volatile bool _halting = false;
    /* between kIOMessageSystemWillSleep and kIOMessageSystemHasPoweredOn */
    volatile bool _asleep = false;
    bool _upBeforeSleep = false;
};
