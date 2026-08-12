# SSD2683 FAST_BW research and implementation

Date: 2026-08-12
Target: ZecTrix Note4C, 400x300, SSD2683, BWRY (black/white/red/yellow)

## Outcome

The firmware now has two deliberately separate SSD2683 refresh paths:

- `FAST_BW`: every interactive RawDraw update is converted to black/white and
  driven through the controller-matched OTP fast-waveform selection sequence.
- `FULL_COLOR`: the pre-existing standard 2bpp BWRY refresh remains unchanged
  as the recovery path. It runs once after 60 seconds with no new interactive
  request. Every new interaction cancels/restarts that not-yet-started timer.

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

Sources:

- [SSD2683 Rev 0.20 controller PDF](https://v4.cecdn.yun300.cn/100001_1909185148/SSD2683_0.20_proposal.pdf)
- [Good Display SSD2683 download page](https://www.good-display.com/companyfile/2082.html)
- [GDEM042F86 official ESP32 sample](https://www.good-display.com/companyfile/2052.html)
- [GDEM042F86 published 12 s / 20 s timing](https://www.good-display.com/product/1048.html)

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
- `CONFIG_ZECTRIX_EPD_FAST_BW_IDLE_FULL_SECONDS` (default `60`)

## Hardware validation plan

Serial warning logs bracket the actual controller operation:

```text
[FAST_BW] start; colors mapped to black/white
[FAST_BW] complete in N ms; FULL_COLOR idle timer remains armed
[FULL_COLOR] start reason=fast_bw_idle_timeout
[FULL_COLOR] complete in N ms
```

Validate on the Note4C panel in this order:

1. Boot and confirm the unchanged standard full-color image is correct.
2. Perform a single menu move. Confirm only black/white is shown and record the
   `FAST_BW` BUSY duration.
3. Operate buttons continuously for more than one minute. Confirm there is no
   intervening `FULL_COLOR` refresh.
4. Stop input. At 60 seconds after the final request, confirm one full-color
   refresh restores red/yellow and improves residual balance.
5. Press a button at about 55 seconds and confirm the recovery moves another
   60 seconds later.
6. Repeat black-to-white, white-to-black and color-underlay-to-B/W transitions;
   photograph residuals and record temperature and panel/FPC revision.
7. Disable `CONFIG_ZECTRIX_EPD_FAST_BW` and rebuild if this Note4C panel batch
   does not contain a compatible fast profile. Standard `FULL_COLOR` remains
   available independently through `RequestUrgentFullRefresh()`.

## Latency limit and next experiment

The strongest public SSD2683/BWRY evidence currently supports a dedicated OTP
fast profile, but its published target is about 12 seconds. It is therefore a
real waveform improvement and a safe first experiment, not yet evidence for a
1-3 second B/W mode on this exact panel.

If hardware timing remains near 12 seconds, the next safe steps are:

1. Read chip revision (`0x70`) and MTP (`0x92`) without programming it, and
   fingerprint multiple Note4C panel batches.
2. Ask the panel vendor for the SSD2683 reserved-command map or the selector for
   a B/W-only OTP bank. Do not brute-force `0xF6` values on an energized panel.
3. Test PTLW (`0x83`, `PMODE=1`) only as a secondary power/visual experiment;
   the datasheet says all gates still scan, so a large speed improvement is not
   expected.
4. Only after obtaining the exact MTP layout and a vendor waveform file should
   an external/custom LUT route be considered. MTP programming must remain a
   separate factory tool with readback, CRC and explicit recovery controls.

Reset/power truncation of the normal color waveform remains a last-resort
experiment and is not part of this implementation.
