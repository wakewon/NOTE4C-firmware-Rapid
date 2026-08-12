# SSD2683 FAST_BW research and implementation

Date: 2026-08-12
Target: ZecTrix Note4C, 400x300, SSD2683, BWRY (black/white/red/yellow)

## Outcome

The firmware now has two deliberately separate SSD2683 refresh paths:

- `ULTRA_BW`: every interactive RawDraw update is converted to black/white,
  driven through the controller-matched OTP fast-waveform selection sequence,
  and executed with experimental fixed-120 Hz / 2 ms controller timing.
- `FULL_COLOR`: the pre-existing standard 2bpp BWRY refresh remains unchanged
  as the recovery path. It runs once after 60 seconds with no new interactive
  request. Every new interaction cancels/restarts that not-yet-started timer.

The original approximately 12-second vendor timing remains a Kconfig fallback.
No refresh-count threshold inserts a color refresh during continuous use. The
implementation does not reset or remove panel power midway through a waveform.

## Existing firmware path

Before this work the RawDraw framebuffer was already semantic 2bpp and occupied
`400 * 300 * 2 / 8 = 30,000` bytes. LVGL/RawDraw updates maintained a dirty
rectangle, a full-frame `prev_buffer`, a tear-free `tx_buf` snapshot and a frame
diff ratio. The refresh task nevertheless contained this unconditional policy:

```cpp
if (!should_full && IsFourColorPanel()) {
    should_full = true;
}
```

For SSD2683, the old full path sends the 30 KB target image using DTM (`0x10`),
then performs PON (`0x04`), DRF (`0x12`), POF (`0x02`) and deep sleep (`0x07`).
At a 40 MHz SPI clock, raw framebuffer transfer is only a small fraction of the
multi-second BUSY period. Reducing a dirty rectangle therefore cannot by itself
produce the desired interaction latency.

The existing `EPD_DisplayPart()` interleaves previous and next 1bpp values into
two bits per pixel. That is a useful transition representation for some older
monochrome controller paths, but it is not the SSD2683 DTM format documented in
the controller datasheet. SSD2683 DTM accepts one target gray/color code per
pixel (`00`, `01`, `10`, `11`); DRF combines that target SRAM with its selected
LUT internally. The new four-color fast path therefore does not use the old
transition interleave function.

## What selects the SSD2683 waveform

The relevant public SSD2683 Rev 0.20 controls are:

| Stage | Command/register | Role |
| --- | --- | --- |
| Reset/load | PSR `0x00`, `LUT_EN` | `0` auto-loads LUT and analog settings from the 3840-byte MTP; `1` selects MCU-provided analog settings |
| Target image | DTM `0x10` | Stores four 2bpp target pixels per byte in SRAM |
| Execute | DRF `0x12` | Drives source/VCOM according to SRAM target data and the selected LUT |
| Timing | PLL `0x30` | Selects 12.5-120 Hz gate/source frame rate and dynamic-frame-rate mode |
| Temperature | CCSET `0xE0`, TSSET `0xE6` | Selects internal sensing or a manual temperature value used by waveform selection |
| Window | PTLW `0x83` | Defines a partial window, but the datasheet explicitly says gates scan both inside and outside it |
| Inspect | RMTP `0x92` | Read-only access to MTP content (first returned byte is dummy) |
| Program | PGM/APG `0x90/0x91` | Permanently programs MTP and requires reset; intentionally unused here |

The public command table does not document a general, reversible MCU command
for uploading a replacement 3840-byte waveform LUT. Programming MTP would be a
panel-specific and potentially irreversible operation, so it is outside the
safe experiment path.

Unlike SSD1683 monochrome controllers, SSD2683 does not expose a documented
`0x32` LUT-write command. Copying an SSD1683 transition/LUT implementation into
this driver would therefore target the wrong command architecture. The public
SSD2683 DTM format carries only the new 2bpp target state; any old-to-new
transition handling is inside the selected controller LUT.

## Evidence-backed fast profile

Good Display publishes a 400x300 four-color SSD2683ZA panel, GDEM042F86, with a
20-second standard refresh and a 12-second fast refresh. Its official ESP32
demo uses this dedicated sequence after normal power/timing setup and PON:

```text
EF 01
F6 15
EF 00
E0 02
E6 5A
A5
```

`0xEF`, `0xF6` and command `0xA5` are vendor-reserved in the public Rev 0.20
command table. The implementation consequently copies the sequence exactly,
does not guess neighboring selector values, and never invokes MTP programming.
The sequence is followed by ordinary DTM/DRF and a complete POF/deep-sleep
sequence.

The same official fast demo also programs `PLL (0x30) = 0x08` and
`CDI (0x50) = 0x37`. The datasheet decodes those values as dynamic 12.5 Hz scan
timing and 20 ms gate/source blanking. This explains why selecting the vendor
"fast" bank still takes approximately 12 seconds: it is a real but conservative
four-color waveform, not an interaction-oriented preview mode.

Sources:

- [SSD2683 Rev 0.20 controller PDF](https://v4.cecdn.yun300.cn/100001_1909185148/SSD2683_0.20_proposal.pdf)
- [Good Display SSD2683 download page](https://www.good-display.com/companyfile/2082.html)
- [GDEM042F86 official ESP32 sample](https://www.good-display.com/companyfile/2052.html)
- [GDEM042F86 published 12 s / 20 s timing](https://www.good-display.com/product/1048.html)

## ULTRA_BW timing experiment

The fastest evidence-backed experiment that does not guess reserved commands,
program MTP, or interrupt an active waveform is to keep the known OTP fast bank
but compress each frame using documented timing controls:

| Profile | PLL `0x30` | CDI `0x50` | Purpose |
| --- | --- | --- | --- |
| Vendor fallback | `0x08` | `0x37` | Dynamic 12.5 Hz, 20 ms blanking; published ~12 s behavior |
| 120 Hz isolation | `0x07` | `0x37` | Fixed 120 Hz, retain 20 ms blanking |
| `ULTRA_BW` default | `0x07` | `0x30` | Fixed 120 Hz, minimum documented 2 ms blanking |

For a frame whose switching and blanking phases both occur once, the nominal
period changes from `80 + 20 = 100 ms` to approximately `8.33 + 2 = 10.33 ms`.
If the OTP profile is primarily frame-count based, this is a theoretical 9.7x
scan-stage reduction and could move a 12-second waveform near the requested
1-3 second range. This is an engineering estimate, not a claimed measured
result: OTP phases, power sequencing and dynamic-rate behavior can make actual
hardware timing differ.

Both values are inside the controller's documented selectable range, but the
panel waveform was not qualified for this settling time. Heavy ghosting, weak
contrast, uneven transitions, or little visible change are accepted outcomes
for preview mode. Every later `FULL_COLOR` call performs a hardware reset and
uses the unchanged normal OTP initialization, so experimental PLL/CDI state is
not inherited by the recovery refresh.

## FAST_BW pixel policy

FAST_BW sends a complete target frame because transfer time is not the limiting
factor and because it avoids relying on undocumented partial-SRAM cursor
semantics. Each semantic RawDraw pixel is transformed as follows:

| RawDraw value | FAST_BW target |
| --- | --- |
| Black `00` | Black `00` |
| White `01` | White `01` |
| Yellow `10` | Black `00` |
| Red `11` | Black `00` |

The original semantic framebuffer remains intact. The delayed `FULL_COLOR`
therefore restores red and yellow without rerendering the UI.

## Scheduler behavior

1. A RawDraw UI callback calls `RequestFastBwRefresh()` regardless of whether
   it came from a page switch, menu selection, cursor move or normal button.
2. The request starts/restarts the configured idle deadline (default 60 s) and
   cancels a due-but-not-started idle recovery.
3. The refresh task snapshots the latest framebuffer and runs `FAST_BW`.
4. Further requests received while the SSD2683 is BUSY remain queued and reset
   the same idle deadline. They never increment a counter that triggers full
   refresh.
5. Navigation input is not locked out while FAST_BW is enabled; repeated input
   can continue updating the semantic framebuffer and the task coalesces it to
   the latest queued frame.
6. Once the deadline expires, the task snapshots the latest semantic frame and
   runs one unchanged standard `FULL_COLOR` refresh.
7. A request arriving before the recovery snapshot cancels the idle recovery.
   Once controller work begins, the request queues a subsequent FAST_BW update;
   no active waveform is reset or power-truncated.

Kconfig controls:

- `CONFIG_ZECTRIX_EPD_FAST_BW` (enabled by default for SSD2683)
- `CONFIG_ZECTRIX_EPD_FAST_BW_TIMING_ULTRA` (default: fixed 120 Hz / 2 ms)
- `CONFIG_ZECTRIX_EPD_FAST_BW_TIMING_120HZ` (diagnostic: fixed 120 Hz / 20 ms)
- `CONFIG_ZECTRIX_EPD_FAST_BW_TIMING_VENDOR` (fallback: published 12 s timing)
- `CONFIG_ZECTRIX_EPD_FAST_BW_IDLE_FULL_SECONDS` (default `60`)
- `CONFIG_ZECTRIX_EPD_SSD2683_MTP_DUMP` (disabled by default; read-only reverse-engineering dump)

## Hardware validation plan

Serial warning logs bracket the actual controller operation:

```text
[ULTRA_BW] start timing=ultra-fixed-120Hz-2ms; colors mapped to black/white
[ULTRA_BW] execute timing=... PLL=0x07 CDI=0x30 transfer=N ms
[ULTRA_BW] waveform BUSY=N ms
[ULTRA_BW] complete in N ms; FULL_COLOR idle timer remains armed
[FULL_COLOR] start reason=fast_bw_idle_timeout
[FULL_COLOR] complete in N ms
```

Validate on the Note4C panel in this order:

1. Boot and confirm the unchanged standard full-color image is correct.
2. Perform a single menu move. Confirm only black/white is shown and record both
   the `ULTRA_BW waveform BUSY` and total duration.
3. Operate buttons continuously for more than one minute. Confirm there is no
   intervening `FULL_COLOR` refresh.
4. Stop input. At 60 seconds after the final request, confirm one full-color
   refresh restores red/yellow and improves residual balance.
5. Press a button at about 55 seconds and confirm the recovery moves another
   60 seconds later.
6. Repeat black-to-white, white-to-black and color-underlay-to-B/W transitions;
   photograph residuals and record temperature and panel/FPC revision.
7. If the image is too weak, test `120HZ` to retain 20 ms blanking. If that is
   still unusable, select `VENDOR` without changing the scheduling behavior.
8. Disable `CONFIG_ZECTRIX_EPD_FAST_BW` only if this Note4C panel batch does not
   contain a compatible fast profile. Standard `FULL_COLOR` remains available
   independently through `RequestUrgentFullRefresh()`.

## Read-only OTP/MTP reverse-engineering path

Enable `CONFIG_ZECTRIX_EPD_SSD2683_MTP_DUMP` only for a diagnostic build. At
boot the driver reads REV (`0x70`) and RMTP (`0x92`), logs the three-byte chip
revision, a 32-bit fingerprint, and all 3840 MTP bytes. It never sends PGM or
APG. Convert a captured serial log into the exact binary image with:

```bash
python firmware/scripts/extract_ssd2683_mtp.py monitor.log ssd2683-mtp.bin
```

The binary is the input needed to compare Note4C panel batches, locate repeated
temperature/profile tables, and correlate the reserved `EF/F6/E6/A5` selector
sequence with actual waveform regions. A dump from the user's panel is still
required before the proprietary layout can be decoded responsibly.

## Hardware result: ULTRA timing did not shorten the waveform

First on-device measurement of the `ultra-fixed-120Hz-2ms` profile:

```
[ULTRA_BW] execute timing=ultra-fixed-120Hz-2ms PLL=0x07 CDI=0x30 transfer=180 ms
[ULTRA_BW] waveform BUSY=14050 ms
[ULTRA_BW] complete in 14710 ms
```

Framebuffer transfer is no longer relevant at 180 ms; the entire cost is the
waveform. Raising the scan clock to the documented 120 Hz maximum and cutting
blanking to 2 ms changed nothing, so the waveform duration is not governed by
the PLL/CDI values this path was writing.

The most likely cause is command ordering: `0x30` and `0x50` were sent before
`0xA5`, and `0xA5` loads an OTP waveform bank that carries its own timing
parameters, overwriting them. Two changes now test this in a single flashing
cycle:

* PLL and CDI are re-applied *after* `0xA5` so the loaded bank cannot win.
* `ZECTRIX_EPD_FAST_BW_PLL_SWEEP` programs a different PLL candidate on every
  fast refresh (`0x08 0x07 0x05 0x0E 0x3A 0x3C`) and logs the resulting BUSY
  time for each, producing a PLL-versus-duration curve from six consecutive
  key presses.

Interpretation of that curve:

* BUSY varies with PLL — register timing is now in effect; pick the fastest
  value that still resolves pixels and disable the sweep.
* BUSY is flat at ~14 s for every value — the frame *count* in the OTP bank
  determines the duration and no register can shorten it. The only remaining
  routes are a different OTP bank selector or a register-loaded LUT, which
  requires the MTP dump below.

## Hardware result: PLL works, but the frame count is the wall

The sweep produced a genuinely varying curve, so register timing is in effect:

| PLL  | BUSY     |
|------|----------|
| 0x07 | 14050 ms |
| 0x08 | 16850 ms |
| 0x0E | 16850 ms |
| 0x05 | 19600 ms |

`0x3A` and `0x3C` were not reached because full-color refreshes interrupted the
key sequence; they remain untested and cost nothing but more key presses.

The curve is the answer, and it is not encouraging. `0x07` is the documented
fixed 120 Hz setting, and it still takes 14050 ms, which implies roughly 1700
waveform frames. That is a four-color waveform, not a monochrome one. The
reserved `EF/F6/E6/A5` selector is therefore either not switching banks or the
bank it selects has the same frame count. No register can shorten a waveform
whose frame count comes from OTP, so `ZECTRIX_EPD_SSD2683_MTP_DUMP` is now
enabled by default to capture the actual waveform data.

## Most interactive refreshes were never using FAST_BW

The same log showed four of eight refreshes taking 23 s as
`[FULL_COLOR] reason=explicit_or_strategy`, including a plain page switch. This
was the dominant cause of the "still slow" impression, and it was a driver bug
rather than a panel limit.

The refresh loop consumes `fast_bw_refresh_requested_` at the top of each
iteration. That iteration can still bail out before refreshing when the
framebuffer has not been re-rendered yet (`diff_bits == 0`) or when the diff is
below the tiny-diff threshold. The framebuffer change then arrives one
iteration later as a plain dirty rect with the flag already consumed, and the
four-color branch sent it to the 23 s full-color waveform.

Such a refresh is now promoted to FAST_BW and re-arms the idle timer, since
color recovery is owned by that timer by design. The start log distinguishes
`source=requested` from `source=promoted_dirty_rect`.

## The OTP contains no short waveform: TSSET sweep result

TSSET (0xE6) selects the temperature section of the OTP waveform, and those
sections have different frame counts. Sweeping it at PLL=0x07 is therefore a
direct test for a shorter waveform, and it needs no knowledge of the
proprietary format. Measured on hardware:

| TSSET | Meaning        | BUSY     |
|-------|----------------|----------|
| 0x00  | 0 C            | 23550 ms |
| 0x0A  | 10 C           | 22700 ms |
| 0x19  | 25 C           | 11500 ms |
| 0x28  | 40 C           | 14100 ms |
| 0x32  | 50 C           | 14100 ms |
| 0x5A  | vendor demo    | 14050 ms |

The 25 C section is the shortest waveform in OTP and is now the default
(`ZECTRIX_EPD_FAST_BW_TSSET`), taking the fast refresh from 14050 ms to
11500 ms. The vendor demo's magic 0x5A is not the fastest choice.

This also settles the larger question. Across every temperature section the
floor is 11.5 s, so the OTP holds no monochrome waveform, only four-color ones
of differing lengths. Sub-second refresh is unreachable without replacing the
LUT itself.

Pinning TSSET fixes the waveform to one temperature instead of following the
panel sensor. In a cold room the pixels are under-driven, costing contrast in
the preview; the idle full-color refresh still uses the real temperature.

## The first MTP dump was not corrupted

An earlier revision of this document argued the dump was corrupted by bit
slips at the 8 MHz read clock. That was wrong. Three consecutive passes at
1 MHz with CS held low across the command return byte-identical data with the
same `fnv1a32=084ABC10`, and that data is byte-for-byte identical to the
original 8 MHz dump. The read path was working the whole time.

The CS handling and the separate read clock are still the correct behaviour
and are kept. What was misread as bit slips is genuine periodic structure:
byte-aligned autocorrelation peaks at 535 bytes (88.8 %), 1070 bytes and
88 bytes, against a ~72 % baseline caused by the data being dominated by 0xFF.
That is consistent with repeated per-temperature waveform sections, which the
TSSET sweep above confirms exist.

## Read path notes

`REV=06:01:01`, `dummy=A0`, `fnv1a32=084ABC10`, 3840 bytes. 85 % of bytes have
six or more bits set and every bit position reads as one between 68 % and 93 %
of the time, so the payload is dominated by 0xFF and 0xFB.

Two changes to the read path are kept as correct behaviour even though the
original read already returned valid data:

1. `EPD_SendCommand` deasserts CS, so the command selecting a read was
   terminated before the response. `EPD_ReadRegister` holds CS low across the
   command, the SDIN turnaround and the whole response.
2. Reads ran at the 8 MHz write clock. `ZECTRIX_EPD_READ_CLOCK_HZ` defaults to
   1 MHz.

The dump performs three passes and reports `diff_vs_pass0` over the payload
(the leading dummy byte legitimately varies and is excluded) plus a
`STABLE` / `UNSTABLE_LOWER_READ_CLOCK` verdict, so a bad read can no longer be
mistaken for waveform contents.

## SSD2683 has no register path for loading a waveform

The Rev 0.20 datasheet settles this. The complete user command table is:

```
00 01 02 03 04 06 07 10 12 17 30 40 41 42 43 50 51 61 65 70 80 81 82 83 90 91 92 E0 E3 E4 E6
```

There is no `0x20`-`0x2F` LUT group and no command anywhere in the table that
uploads waveform data from the MCU. The feature list states the waveform lives
in an "Embedded 3840 Bytes MTP", which is exactly the block RMTP returns.

`LUT_EN`, PSR (0x00) second byte B[7], is the only documented lever over the
LUT source: 0 auto-loads the LUT from MTP, 1 skips the load and makes the
*analog* settings follow the MCU. It does not redirect the waveform, and
hardware agrees: with TSSET=0x19 and PLL=0x07, `0x69` (LUT_EN=0) took 11450 ms
and `0xE9` (LUT_EN=1) took 11600 ms. Sweeping the first PSR byte moved nothing
either: `0x2F`, `0x0F`, `0x3F`, `0x6F` and `0xAF` all landed within
11050-11600 ms.

Every volatile avenue is therefore exhausted. The waveform can only be changed
by programming the MTP through `0x90` PGM and `0x91` APG.

### Why MTP programming is not a drop-in next step

* The command table has PGM and APG but **no erase command**. The current MTP
  content is dominated by 0xFF, so programming almost certainly only drives
  bits one way, which means the verified `fnv1a32=084ABC10` dump cannot be
  written back to undo a mistake.
* The datasheet documents the *commands*, not the waveform *encoding*. The
  3840-byte layout is proprietary; the only structure established so far is
  byte-aligned periodicity at 535 and 88 bytes, plus the per-temperature
  sections the TSSET sweep exposed.
* A wrong waveform is burned in permanently and there is no factory image to
  restore, so the failure mode is a permanently degraded panel rather than a
  bad refresh.

Programming the MTP therefore requires an explicit, informed decision and a
recovery plan, not just a code path. It is deliberately not implemented here.

## Where this ended

Shipping configuration, measured on hardware:

| Path                          | Before   | Now      |
|-------------------------------|----------|----------|
| Interactive refresh           | 23020 ms | ~550 ms  |
| Every 6th refresh (complete)  | 23020 ms | 12160 ms |
| Idle color recovery           | 23020 ms | 23020 ms |

Interactive refresh is roughly 40x faster than where this started. Three
changes got there:

1. Dirty-rect refreshes go to FAST_BW instead of being demoted to the
   full-color waveform.
2. TSSET selects the 25 C OTP section, the shortest waveform in the panel.
3. The waveform is cut short at 120 ms and the controller is stopped with a
   reset, because black-and-white content is already legible after the first
   drive. POF cannot do this: it is queued until the refresh finishes, so an
   earlier attempt saved nothing and merely moved the wait.

120 ms was chosen by looking at the panel. 100 ms washed the image out, and
below 150 ms the total is dominated by the fixed ~430 ms of init and transfer
anyway, so shorter cuts buy little while cutting deeper into the strongest
drive phase.

Truncation breaks the waveform's DC balance, which is what causes permanent
image sticking, so every 6th refresh runs to completion. An earlier design
also ran a complete waveform a few seconds after input stopped; it was removed
because it turned every isolated key press into an 11.5 s wait, which was
worse than the ghosting it removed.

The four-color sample interval is 500 ms rather than 12000 ms when FAST_BW is
enabled. The 12 s figure was sized for the 23 s full-color waveform, and it
was throttling key presses by up to 12 s before anything happened.

Per-refresh telemetry sits at DEBUG with a single INFO summary line. The
secondary USB-Serial-JTAG console blocks the logging task while a host is
attached but not draining, so log volume in this path costs input
responsiveness.

Full-color refresh deliberately keeps its own temperature handling. `EPD_Init`
hardware-resets the controller and never writes CCSET/TSSET, so it uses the
internal sensor and adapts to ambient temperature on its own. The reset also
guarantees the fast path's pinned 25 C never leaks into it.

This is the floor for a configuration-only approach. Sub-second refresh needs
a monochrome waveform, the panel's MTP holds only four-color ones, and
SSD2683 offers no way to load a waveform from the MCU. The remaining route,
programming the MTP, was considered and declined: it is irreversible, has no
erase command, and no restorable factory image.

### Diagnostic switches left in place

All default to off. Each was used to produce a measurement in this document
and is kept so the experiments are repeatable:

* `ZECTRIX_EPD_FAST_BW_PLL_SWEEP` - scan-clock sweep.
* `ZECTRIX_EPD_FAST_BW_TSSET_SWEEP` - waveform temperature-section sweep.
* `ZECTRIX_EPD_FAST_BW_TRUNCATE_SWEEP` - truncation-deadline sweep, the one
  experiment that needs a person watching the panel rather than a log.
* `ZECTRIX_EPD_FAST_BW_PSR_SWEEP` - PSR second-byte sweep, including LUT_EN.
* `ZECTRIX_EPD_FAST_BW_TSSET_SWEEP_BOOT` - runs the active sweep at boot,
  needed because an untouched device never produces a dirty rect.
* `ZECTRIX_EPD_SSD2683_MTP_DUMP` - three-pass read-only MTP dump with a
  stability verdict.

### Untested ideas, if this is ever picked up again

* PTLW (`0x83`) partial window. The datasheet says all gates still scan, so a
  large gain is unlikely, but it was never measured.
* An official monochrome waveform table from the panel vendor would make MTP
  programming a far more reasonable proposition than a reverse-engineered one.
