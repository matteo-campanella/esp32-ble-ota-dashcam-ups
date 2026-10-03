#!/usr/bin/env python3
"""Read the ESP32's BLE settings and retained voltage history.

This is a read-only diagnostic tool: it neither writes the phone timestamp nor
changes any ESP32 configuration.  It is useful for proving how many history
records the ESP32 exports independently of the mobile app.

Install once:
    py -m pip install bleak

Examples:
    py tools\\ble_sync_dump.py
    py tools\\ble_sync_dump.py --address AA:BB:CC:DD:EE:FF
    py tools\\ble_sync_dump.py --scan-seconds 20
"""

from __future__ import annotations

import argparse
import asyncio
import struct
from datetime import UTC, datetime

from bleak import BleakClient, BleakScanner


DEVICE_NAME = "bleUPS"
UPS_SERVICE_UUID = "d96011fc-8ab0-42d9-93bb-ae202331297a"
SETTINGS_UUID = "235fefc9-58fd-4f84-977a-9a72ae348007"
HISTORY_PAGE_1_UUID = "e6aa2d53-4ed4-43a6-a799-18dbf6a6d3da"
HISTORY_PAGE_2_UUID = "8e64f238-2ffc-4870-bd31-3358f3b5c82d"
HEADER_SIZE = 4
RECORD_SIZE = 11


def parse_page(raw: bytes, expected_page: int) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    if len(raw) < HEADER_SIZE:
        raise ValueError(f"history page {expected_page} is only {len(raw)} bytes")
    version, count, page, total_pages = raw[:HEADER_SIZE]
    if version != 1 or page != expected_page:
        raise ValueError(
            f"unexpected history header: version={version}, page={page}, expected={expected_page}"
        )
    required = HEADER_SIZE + count * RECORD_SIZE
    if len(raw) < required:
        raise ValueError(f"history page {page} is truncated ({len(raw)} bytes, needs {required})")
    records = [
        struct.unpack_from("<IIHB", raw, HEADER_SIZE + index * RECORD_SIZE)
        for index in range(count)
    ]
    return count, total_pages, records


def format_timestamp(epoch_seconds: int) -> str:
    if epoch_seconds == 0:
        return "unknown (not yet time-synchronized)"
    return datetime.fromtimestamp(epoch_seconds, UTC).astimezone().isoformat(timespec="seconds")


async def find_device(address: str | None, scan_seconds: float):
    target = address.casefold() if address else None
    description = address if address else DEVICE_NAME
    print(f"Scanning for {description} for up to {scan_seconds:g} seconds...")
    devices = await BleakScanner.discover(timeout=scan_seconds, return_adv=True)
    for device, advertisement in devices.values():
        advertised_services = {service.lower() for service in advertisement.service_uuids}
        matches_address = target is not None and device.address.casefold() == target
        matches_esp = target is None and (
            device.name == DEVICE_NAME or UPS_SERVICE_UUID in advertised_services
        )
        if matches_address or matches_esp:
            print(f"Found {device.name or DEVICE_NAME} at {device.address} ({advertisement.rssi} dBm)")
            return device
    raise RuntimeError(f"No advertisement found for {description}. Wait for its active BLE window and retry.")


async def dump(address: str | None, scan_seconds: float, connect_timeout: float) -> None:
    target = await find_device(address, scan_seconds)
    # Windows can retain a stale GATT-service cache after ESP firmware changes.
    # Always enumerate the live service table for this diagnostic read.
    async with BleakClient(
        target,
        timeout=connect_timeout,
        winrt={"use_cached_services": False},
    ) as client:
        if not client.is_connected:
            raise RuntimeError("BLE connection failed")
        print(f"Connected to {target}")

        settings = bytes(await client.read_gatt_char(SETTINGS_UUID))
        print(f"Settings: {settings.decode('utf-8', errors='replace')}")

        first_raw = bytes(await client.read_gatt_char(HISTORY_PAGE_1_UUID))
        first_count, total_pages, records = parse_page(first_raw, 0)
        print(f"History page 1: {first_count} record(s), {total_pages} page(s) total")

        if total_pages > 1:
            second_raw = bytes(await client.read_gatt_char(HISTORY_PAGE_2_UUID))
            second_count, second_total_pages, second_records = parse_page(second_raw, 1)
            if second_total_pages != total_pages:
                raise ValueError("history page count changed while reading")
            print(f"History page 2: {second_count} record(s)")
            records.extend(second_records)

        print(f"\nESP32 retained history: {len(records)}/100 sample(s)")
        if not records:
            print("No records exported.")
            return
        print("sequence  timestamp                         battery   low  switch  valid")
        for sequence, epoch, millivolts, flags in records:
            low_battery = "yes" if flags & 0x01 else "no"
            switch_on = "on" if flags & 0x02 else "off"
            valid = "no" if flags & 0x04 else "yes"
            print(
                f"{sequence:8d}  {format_timestamp(epoch):32} "
                f"{millivolts / 1000:7.3f} V  {low_battery:3}  {switch_on:6}  {valid}"
            )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="BLE address shown by a scanner")
    parser.add_argument("--scan-seconds", type=float, default=15.0, help="discovery duration (default: 15)")
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=30.0,
        help="GATT connection timeout in seconds (default: 30)",
    )
    args = parser.parse_args()
    asyncio.run(dump(args.address, args.scan_seconds, args.connect_timeout))


if __name__ == "__main__":
    main()
