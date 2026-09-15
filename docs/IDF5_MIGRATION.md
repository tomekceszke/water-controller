# ESP-IDF 5.1.1 → 5.4.2 migration (bootloader, partition table, firmware 3.x)

The production board sits in a sealed enclosure, so the whole migration runs **over the air** with a one-shot
migrator image (home-idf `hi_migrator`, ported from gate-controller where it migrated production on 2026-09-15).
Rehearse on the spare board first.

## Why the bootloader and partition table change

| | Legacy (in flash) | Target |
|---|---|---|
| Bootloader | v5.1.1, no app rollback | v5.4.2 with native OTA rollback |
| Partition table | factory 1M, ota_0 1M @0x110000, ota_1 1M @0x210000 | ota_0 2M @0x10000, ota_1 1.875M @0x210000, coredump 64K @0x3F0000 |
| App | ~957 KB (96 % of a slot) | ~980 KB, room to grow |

nvs (0x9000), otadata (0xd000) and phy_init (0xf000) keep their offsets. Flash settings are identical in both
bootloaders (DIO, 40 MHz, 4 MB).

## Stages

1. The legacy firmware downloads `water-controller.bin` from the OTA server at boot, or on `POST /su` with the
   legacy Authorization header. At this step the file is the **migrator**.
2. The migrator runs from whichever slot the legacy OTA used. If that is not the legacy ota_1 (0x210000), it copies
   itself there and reboots.
3. `GET /migrator` shows checks. Nothing is written before they all pass:
   - blob SHA-256;
   - flash settings;
   - table MD5;
   - nvs/otadata/phy offsets;
   - ota_1 = running image;
   - last reset not a brownout.
4. `POST /migrator/commit` (admin header), watched on the UDP log:
   - the partition table and then the bootloader are written, verified, retried up to 3 times;
   - on persistent failure the old content is restored.
5. The new bootloader boots the migrator, which OTAs the **firmware** (same URL, now the real firmware) into ota_0.
6. Firmware 3.x verifies itself (Tier 1 task alive + WiFi within 300 s, at most 3 counted resets) or boots the migrator again.

**Water during the migration:**
- The migrator drives the valve line open (the same level the legacy firmware drives at every boot) and lights both LEDs.
- **Tier 1 protection is not running while the migrator runs** (minutes).
- Firmware 3.x starts with the valve open (no stored state yet) and protects from then on.

Intermediate states stay bootable. Only an interrupted erase/write of the table or bootloader region (under a
second) bricks the board; recovery then needs USB.

## Rehearsal on the spare board

The spare builds use the file name `water-controller-spare.bin`. While the OTA server runs, production can therefore
never download a rehearsal image, even if it reboots.

```sh
# 1. Legacy image (ESP-IDF 5.1.1, eim) with SU_URL .../water-controller-spare.bin, flashed like production
cd legacy/water-controller-5.1.1-spare     # copy of the snapshot sources + secrets, gitignored
source ~/.espressif/tools/activate_idf_v5.1.1.sh && idf.py build && idf.py -p /dev/cu.usbserial-0001 -b 115200 flash

# 2. Rehearsal migrator and firmware with the same file name
tools/build_release.sh --spare               # -> releases/spare/

# 3. Serve the migrator, trigger the legacy OTA, watch the UDP log
scp releases/water-migrator.bin 192.168.11.15:apps/ota-server/builds/water-controller-spare.bin
ssh 192.168.11.15 'cd apps/ota-server && python3 ota_server.py'
curl -X POST -H 'Authorization: <legacy value>' --data 1 http://<spare-ip>/su
nc -ul 1338

# 4. GET /migrator until stage "ready", then POST /migrator/commit; serve the firmware under the same name
# 5. Read back 0x1000 (0x7000) and 0x8000 (0xC00) over USB: identical to the embedded blobs
```

## Production checklist

- [ ] Rehearsal passed, image hashes recorded below
- [ ] Owner's web password hash in `credentials.h` (not the spare test password), ntfy topics set
- [ ] Nobody uses water; valve open; stable mains
- [ ] USB adapter and access to the enclosure possible in case a write is interrupted
- [ ] BigQuery export right before the cutover (`server/migrate`), delta import after
- [ ] After: `/admin/hw-status` version 3.x, events arrive in hc-data, bucket test matches the calibration

## Rehearsal results (2026-09-15, spare board ESP32-D0WDQ6, MAC ec:62:60:83:a2:b0)

| Step | Result |
|---|---|
| Legacy 5.1.1 image (bootloader, two_ota table `d1c0e9d0…`, app `05b873a2…` 992 KB) flashed over USB, boots from `factory` | OK |
| Legacy `POST /su` downloads the migrator (830 KB) and sends DELETE | OK |
| Legacy bootloader boots the 5.4.2 migrator from ota_0 @0x110000; it relocates itself to ota_1 @0x210000 | OK |
| Checks: bootloader blob 24 048 B + sha256, flash settings (DIO, 4 MB/40 m), table MD5, nvs/otadata/phy identical, ota_1 = running image, no brownout | all ok, stage `ready` |
| `POST /migrator/commit`: partition table 59 ms, bootloader 422 ms, both verified | OK |
| New bootloader boots the migrator, which installs firmware 3.0.0 into ota_0 (978 KB in 18 s) | OK |
| Firmware first boot: valve kept open (no stored state), Tier 1 running at 0.6 s, image verified after 4 s | OK |
| Read back over USB: bootloader and partition table byte-identical to the embedded blobs | OK |
| Crashing image (`WATER_TEST_PANIC`, 3.0.1) via OTA: 4 counted panics, then rollback to 3.0.0 | OK |
| OTA 3.0.0 → 3.0.1 with the valve **closed**: valve still closed after the update, image verified | OK |

Rehearsal findings:
- `hi_ota` reported a failed DELETE after the server had answered 200. The simple OTA server replies without a body. Fixed in home-idf 0.1.4.
- On the Mac, opening the USB serial port resets this board (DTR/RTS). Don't open a serial monitor during a commit.
- The bootloader embeds its compile time, so every build has a different bootloader hash. The migrator verifies the blobs of its own build.

Not tested: power cut during each stage, brownout during the first boot (covered by the same `hi_health` logic as the
gate migration), migration from a legacy image that runs from ota_0 (the migrator then lands in ota_1 directly and skips relocation).

## Production procedure

1. `tools/build_release.sh` (production file name) with the owner's credentials; record the sha256 of both images.
2. `server/migrate/bq_export.py` + `import.sh` (history up to the cutover).
3. `scp releases/water-migrator.bin 192.168.11.15:apps/ota-server/builds/water-controller.bin`, then start
   `ota_server.py` on .15.
4. Legacy: `POST /su` with the legacy Authorization header (or wait for a legacy reboot).
5. `GET http://192.168.11.244/migrator` until `ready` with all checks ok.
6. `scp releases/water-controller.bin 192.168.11.15:apps/ota-server/builds/water-controller.bin` **before** the commit.
7. `POST /migrator/commit` with the admin header. The firmware follows within a minute.
8. Verify:
   - `/admin/hw-status` shows 3.x;
   - PWA login works;
   - `water/<mac>/status online` appears in hc-data;
   - a short flow shows up as an event;
   - valve open.
9. Stop the OTA server. Rerun `server/migrate` for the history delta. Stop the GCP function after 2 weeks.
