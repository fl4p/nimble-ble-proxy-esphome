#!/opt/homebrew/bin/python3.14
"""Mac as a BLE GATT peripheral: one notify char carrying a uint32 sequence
number (+ padding), one read char. Notifies at --hz while subscribed.
Writes a status JSON to --status every second: sent, refused (queue full),
subscribed, seq.

usage: macperiph.py --hz 20 --len 20 --status FILE [--name nbp-bench]
"""
import argparse, json, struct, time
import objc
from Foundation import NSObject, NSData, NSTimer, NSRunLoop, NSDate
import CoreBluetooth as CB
from PyObjCTools import AppHelper

ap = argparse.ArgumentParser()
ap.add_argument("--hz", type=float, default=20)
ap.add_argument("--len", type=int, default=20)
ap.add_argument("--status", required=True)
ap.add_argument("--name", default="nbp-bench")
a = ap.parse_args()

SVC = CB.CBUUID.UUIDWithString_("6E5B0001-3A1C-4F6A-9C0E-7B2D3F4A5B6C")
NCH = CB.CBUUID.UUIDWithString_("6E5B0002-3A1C-4F6A-9C0E-7B2D3F4A5B6C")
RCH = CB.CBUUID.UUIDWithString_("6E5B0003-3A1C-4F6A-9C0E-7B2D3F4A5B6C")

class P(NSObject):
    def init(self):
        self = objc.super(P, self).init()
        self.seq = 0; self.sent = 0; self.refused = 0; self.subs = 0; self.reads = 0
        self.pending = None; self.t0 = time.time(); self.state = "init"
        self.pm = CB.CBPeripheralManager.alloc().initWithDelegate_queue_(self, None)
        return self

    def peripheralManagerDidUpdateState_(self, pm):
        self.state = int(pm.state())
        if pm.state() == CB.CBManagerStatePoweredOn:
            props = CB.CBCharacteristicPropertyNotify | CB.CBCharacteristicPropertyRead
            self.nch = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
                NCH, props, None, CB.CBAttributePermissionsReadable)
            self.rch = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
                RCH, CB.CBCharacteristicPropertyRead, None, CB.CBAttributePermissionsReadable)
            svc = CB.CBMutableService.alloc().initWithType_primary_(SVC, True)
            svc.setCharacteristics_([self.nch, self.rch])
            pm.addService_(svc)

    def peripheralManager_didAddService_error_(self, pm, svc, err):
        pm.startAdvertising_({CB.CBAdvertisementDataLocalNameKey: a.name,
                              CB.CBAdvertisementDataServiceUUIDsKey: [SVC]})

    def peripheralManagerDidStartAdvertising_error_(self, pm, err):
        self.adv_err = str(err) if err else None

    def peripheralManager_central_didSubscribeToCharacteristic_(self, pm, c, ch):
        self.subs += 1; self.maxlen = c.maximumUpdateValueLength()

    def peripheralManager_central_didUnsubscribeFromCharacteristic_(self, pm, c, ch):
        self.subs = max(0, self.subs - 1)

    def peripheralManager_didReceiveReadRequest_(self, pm, req):
        self.reads += 1
        v = struct.pack("<Id", self.seq, time.time())
        off = req.offset()
        req.setValue_(NSData.dataWithBytes_length_(v[off:], len(v) - off))
        pm.respondToRequest_withResult_(req, CB.CBATTErrorSuccess)

    def peripheralManagerIsReadyToUpdateSubscribers_(self, pm):
        self.flush()

    def flush(self):
        while self.pending is not None:
            if self.pm.updateValue_forCharacteristic_onSubscribedCentrals_(self.pending, self.nch, None):
                self.sent += 1; self.seq += 1; self.pending = None
                return
            self.refused += 1
            return

    def tick_(self, t):
        if self.subs and self.pending is None:
            b = struct.pack("<I", self.seq) + bytes(max(0, a.len - 4))
            self.pending = NSData.dataWithBytes_length_(b, len(b))
            self.flush()
        elif self.pending is not None:
            self.flush()

    def status_(self, t):
        json.dump({"t": round(time.time() - self.t0, 1), "state": self.state, "subs": self.subs,
                   "sent": self.sent, "seq": self.seq, "refused": self.refused, "reads": self.reads,
                   "adv_err": getattr(self, "adv_err", "pending")},
                  open(a.status, "w"))

p = P.alloc().init()
NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(1.0 / a.hz, p, "tick:", None, True)
NSTimer.scheduledTimerWithTimeInterval_target_selector_userInfo_repeats_(1.0, p, "status:", None, True)
AppHelper.runConsoleEventLoop(installInterrupt=True)
