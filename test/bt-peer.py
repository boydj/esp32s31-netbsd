#!/usr/bin/env python3
"""
bt-peer.py: a Bluetooth world for testing espbt(4) in esp-emulator.

esp-emu hands the co-processor firmware's HCI traffic (its VHCI) to a
controller on TCP port 9544; this serves that controller with Google's
Bumble, on a virtual link shared with a second, classic (BR/EDR) device:

  "Bumble Peer" AA:BB:CC:DD:EE:01, discoverable and connectable, with a
  Serial Port service on RFCOMM channel 1 that echoes what it receives,
  upper-cased.

Then NetBSD can do btconfig inquiry, sdpquery and rfcomm_sppd against it.
Start this first, then esp-emu with --elf espcp.elf --ble-hci
tcp:localhost:9544.

usage: bt-peer.py [--port N]
"""

import argparse
import asyncio
import logging
import os
import sys

from bumble import core, hci, rfcomm, sdp
from bumble.controller import Controller
from bumble.device import Device
from bumble.host import Host
from bumble.link import LocalLink
from bumble.transport import open_transport

logging.basicConfig(level=logging.DEBUG if os.environ.get("BUMBLE_DEBUG")
                    else logging.WARNING)

PEER_ADDR = "AA:BB:CC:DD:EE:01"
PEER_NAME = "Bumble Peer"


def ack_ok(self, command):
    params = hci.HCI_GenericReturnParameters(data=bytes([0]))
    if isinstance(command, hci.HCI_SyncCommand):
        return params
    self.send_hci_packet(hci.HCI_Command_Complete_Event(
        num_hci_command_packets=1, command_opcode=command.op_code,
        return_parameters=params))
    return None


# Bumble's controller doesn't know every configuration command a host
# sends while it sets up (link policy, baseband, vendor): ack them, as
# esp-emulator's own bumble_classic_peer.py does.
_on_hci_command = Controller.on_hci_command


def on_hci_command(self, command):
    if (command.op_code >> 10) in (0x02, 0x3F):
        return ack_ok(self, command)
    handler = getattr(self, f"on_{command.name.lower()}", None)
    if (command.op_code >> 10) == 0x03 and handler is None:
        return ack_ok(self, command)
    return _on_hci_command(self, command)


Controller.on_hci_command = on_hci_command


# Nor does it do inquiry: report the other classic devices on the link.
def on_hci_inquiry_command(self, command):
    self._send_hci_command_status(hci.HCI_ErrorCode.SUCCESS, command.op_code)
    peers = [c for c in self.link.controllers if c is not self]

    def results():
        for c in peers:
            self.send_hci_packet(hci.HCI_Inquiry_Result_Event(
                bd_addr=[c.public_address], page_scan_repetition_mode=[1],
                reserved_0=[0], reserved_1=[0],
                class_of_device=[0x5a020c],     # phone, smartphone
                clock_offset=[0]))
        self.send_hci_packet(hci.HCI_Inquiry_Complete_Event(status=0))
    asyncio.get_running_loop().call_later(1.0, results)


Controller.on_hci_inquiry_command = on_hci_inquiry_command


def on_hci_read_clock_offset_command(self, command):
    self._send_hci_command_status(hci.HCI_ErrorCode.SUCCESS, command.op_code)
    self.send_hci_packet(hci.HCI_Read_Clock_Offset_Complete_Event(
        status=hci.HCI_ErrorCode.SUCCESS,
        connection_handle=command.connection_handle, clock_offset=0))


if not hasattr(Controller, "on_hci_read_clock_offset_command"):
    Controller.on_hci_read_clock_offset_command = on_hci_read_clock_offset_command


def spp_records(handle, channel):
    return [
        sdp.ServiceAttribute(
            sdp.SDP_SERVICE_RECORD_HANDLE_ATTRIBUTE_ID,
            sdp.DataElement.unsigned_integer_32(handle)),
        sdp.ServiceAttribute(
            sdp.SDP_BROWSE_GROUP_LIST_ATTRIBUTE_ID,
            sdp.DataElement.sequence([
                sdp.DataElement.uuid(sdp.SDP_PUBLIC_BROWSE_ROOT)])),
        sdp.ServiceAttribute(
            sdp.SDP_SERVICE_CLASS_ID_LIST_ATTRIBUTE_ID,
            sdp.DataElement.sequence([
                sdp.DataElement.uuid(core.BT_SERIAL_PORT_SERVICE)])),
        sdp.ServiceAttribute(
            sdp.SDP_PROTOCOL_DESCRIPTOR_LIST_ATTRIBUTE_ID,
            sdp.DataElement.sequence([
                sdp.DataElement.sequence([
                    sdp.DataElement.uuid(core.BT_L2CAP_PROTOCOL_ID)]),
                sdp.DataElement.sequence([
                    sdp.DataElement.uuid(core.BT_RFCOMM_PROTOCOL_ID),
                    sdp.DataElement.unsigned_integer_8(channel)]),
            ])),
        sdp.ServiceAttribute(
            0x0100,  # service name (primary language base)
            sdp.DataElement.text_string(b"Bumble echo")),
    ]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9544)
    a = ap.parse_args()

    link = LocalLink()
    transport = await open_transport(f"tcp-server:_:{a.port}")
    Controller("emu", host_source=transport.source, host_sink=transport.sink,
               link=link, public_address="00:11:22:33:44:55")

    peer_ctrl = Controller("peer", link=link, public_address=PEER_ADDR)
    host = Host()
    host.controller = peer_ctrl
    peer_ctrl.host = host
    dev = Device(name=PEER_NAME, host=host)
    dev.classic_enabled = True

    def on_dlc(dlc):
        print(f"[+] RFCOMM DLC open: {dlc}", flush=True)

        def on_data(data):
            print(f"[+] RFCOMM got {data!r}", flush=True)
            dlc.write(data.upper())
        dlc.sink = on_data

    server = rfcomm.Server(dev)
    # Bumble answers a DLC's parameter negotiation with its own frame size
    # even when the initiator asked for less, which NetBSD (rightly)
    # refuses; NetBSD asks for 127.
    channel = server.listen(on_dlc, 1, max_frame_size=127)
    dev.sdp_service_records = {0x00010001: spp_records(0x00010001, channel)}
    dev.on("connection", lambda c: print(f"[+] connection from {c.peer_address}",
                                         flush=True))
    await dev.power_on()
    await dev.set_discoverable(True)
    await dev.set_connectable(True)
    print(f"[*] controller on :{a.port}; peer {PEER_ADDR} \"{PEER_NAME}\" "
          f"with SPP on channel {channel}", flush=True)
    await asyncio.get_running_loop().create_future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
