#!/usr/bin/env python3
"""What a Web Bluetooth services filter sees when it looks for the note device.

The relay page asks for `dc5300a0-…`; Chrome and iOS only list a device whose
advertising packet carries that UUID, so this prints the packet's service UUIDs
and says whether the page's filter would match. If the gadget is missing from a
phone's list, run this on a machine with a Bluetooth adapter while the device's
Phone app pairing window is open:

    python3 scan_adv.py            # 12 seconds
    python3 scan_adv.py 20         # longer

Needs `bleak` (`pip install bleak`) and a Bluetooth adapter the kernel can see.
"""
import asyncio
import sys

from bleak import BleakScanner

SERVICE_UUID = "dc5300a0-7a2a-400f-b8f6-e87185576192"  # what the page filters on
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0


async def main() -> None:
    seen = {}

    def on_detect(device, adv):
        seen[device.address] = (device, adv)

    async with BleakScanner(detection_callback=on_detect):
        await asyncio.sleep(SECONDS)

    matches = [(d, a) for d, a in seen.values() if SERVICE_UUID in [u.lower() for u in (a.service_uuids or [])]]
    for device, adv in matches:
        print(f"{device.address}  rssi={adv.rssi:4d}  name={adv.local_name!r}")
        print(f"    uuids={[u.lower() for u in (adv.service_uuids or [])]}")
    if not matches:
        print(f"nothing advertised {SERVICE_UUID} in {SECONDS:.0f} s")
        print("open the gadget's Phone app pairing window first: it only advertises then,")
        print("or while a phone is already paired.")
        for device, adv in list(seen.values())[:10]:
            print(f"  saw {device.address} rssi={adv.rssi:4d} name={adv.local_name!r} "
                  f"uuids={[u.lower() for u in (adv.service_uuids or [])]}")


asyncio.run(main())
