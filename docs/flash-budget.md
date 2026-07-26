# Flash budget — what every feature costs

The DP32G030 gives this firmware **61440 bytes** of program area, and the APRS
suite takes about a sixth of it. Every feature added since has been paid for by
turning something else off, so this table exists to make that trade explicit
instead of discovering it from a linker error.

Measured **2026-07-27** on v1.3 with `sh utils/flag_cost_sweep.sh`: one clean
build per flag, flipped away from its edition default, diffed against the
baseline amateur-band image (`make`, **61268 bytes**, 172 free). Toolchain
`arm-none-eabi-gcc 10.3.1`, the version CI is pinned to.

"Cost" always means *what this feature costs you*: for a flag that ships ON it
is what you reclaim by cutting it, for one that ships OFF it is what you pay to
add it. A negative number means the feature makes the image **smaller** than the
alternative — read the coupling note below before trusting one.

## The big one

| feature | flag | bytes |
|---|---|---|
| the whole APRS suite | `ENABLE_APRS` | **9520** |

Without APRS the image is 51748 bytes. Nothing else here is in the same league,
which is why the APRS edition ships with so much stock functionality off.

## Enabled today — reclaim by cutting

| feature | flag | bytes |
|---|---|---|
| AM broadcast/airband AGC compensation | `ENABLE_AM_FIX` | 616 |
| RSSI bar | `ENABLE_RSSI_BAR` | 564 |
| UART remote control / display mirror | `ENABLE_UART_RC` | 440 |
| scan ranges | `ENABLE_SCAN_RANGES` | 384 |
| narrower bandwidth option | `ENABLE_FEAT_F4HWN_NARROWER` | 192 |
| copy channel → VFO | `ENABLE_COPY_CHAN_TO_VFO` | 120 |
| more sensitive squelch | `ENABLE_SQUELCH_MORE_SENSITIVE` | 96 |
| F4HWN CTR | `ENABLE_FEAT_F4HWN_CTR` | 76 |
| flashlight | `ENABLE_FLASHLIGHT` | 68 |
| wide RX | `ENABLE_WIDE_RX` | 64 |
| F4HWN CA | `ENABLE_FEAT_F4HWN_CA` | 64 |
| F4HWN INV | `ENABLE_FEAT_F4HWN_INV` | 20 |
| keep channel names | `ENABLE_KEEP_MEM_NAME` | 0 |
| faster channel scan | `ENABLE_FASTER_CHANNEL_SCAN` | 0 |
| F4HWN spectrum | `ENABLE_FEAT_F4HWN_SPECTRUM` | 0 — see note |
| resume previous state | `ENABLE_FEAT_F4HWN_RESUME_STATE` | 0 — see note |
| custom menu layout | `ENABLE_CUSTOM_MENU_LAYOUT` | **−84** (saves) |
| no code-scan timeout | `ENABLE_NO_CODE_SCAN_TIMEOUT` | **−28** (saves) |
| amateur-band restriction | `ENABLE_AMATEUR_BAND_ONLY` | **−52** — see note |

## Disabled today — pay to add

✗ means it does not fit the amateur image (172 free). The all-band image has
120 free, so it clears even fewer of these.

| feature | flag | bytes | |
|---|---|---|---|
| FM radio | `ENABLE_FMRADIO` | 4260 | ✗ |
| DTMF calling | `ENABLE_DTMF_CALLING` | 3232 | ✗ |
| aircopy | `ENABLE_AIRCOPY` | 1952 | ✗ |
| F4HWN game | `ENABLE_FEAT_F4HWN_GAME` | 1948 | ✗ |
| NOAA | `ENABLE_NOAA` | 1140 | ✗ |
| alarm | `ENABLE_ALARM` | 916 | ✗ |
| VOX | `ENABLE_VOX` | 804 | ✗ |
| F4HWN rescue-ops | `ENABLE_FEAT_F4HWN_RESCUE_OPS` | 680 | ✗ |
| small-bold font | `ENABLE_SMALL_BOLD` | 588 | ✗ |
| F4HWN sleep | `ENABLE_FEAT_F4HWN_SLEEP` | 528 | ✗ |
| REGA | `ENABLE_REGA` | 484 | ✗ |
| mic audio bar | `ENABLE_AUDIO_BAR` | 392 | ✗ |
| extra UART commands | `ENABLE_EXTRA_UART_CMD` | 264 | ✗ |
| RX/TX timer | `ENABLE_FEAT_F4HWN_RX_TX_TIMER` | 224 | ✗ |
| 1750 Hz tone burst | `ENABLE_TX1750` | 208 | ✗ |
| frequency calibration menu | `ENABLE_F_CAL_MENU` | 164 | |
| F4HWN volume | `ENABLE_FEAT_F4HWN_VOL` | 148 | |
| GMRS/FRS/MURS | `ENABLE_FEAT_F4HWN_GMRS_FRS_MURS` | 108 | |
| show charge level | `ENABLE_SHOW_CHARGE_LEVEL` | 88 | |
| backlight-min temp off | `ENABLE_BLMIN_TMP_OFF` | 68 | |
| reset channel | `ENABLE_FEAT_F4HWN_RESET_CHANNEL` | 60 | |
| charging indicator | `ENABLE_FEAT_F4HWN_CHARGING_C` | 32 | |
| PMR | `ENABLE_FEAT_F4HWN_PMR` | 28 | |
| bypass raw demodulators | `ENABLE_BYP_RAW_DEMODULATORS` | 4 | |
| power-on password | `ENABLE_PWRON_PASSWORD` | 4 | |
| boot beeps | `ENABLE_BOOT_BEEPS` | 0 | |
| CTCSS tail phase shift | `ENABLE_CTCSS_TAIL_PHASE_SHIFT` | 0 | |
| reduce low/mid TX power | `ENABLE_REDUCE_LOW_MID_TX_POWER` | 0 | |
| reverse battery symbol | `ENABLE_REVERSE_BAT_SYMBOL` | 0 | |
| TX when AM | `ENABLE_TX_WHEN_AM` | −12 (saves) | |
| fill-in digipeater | `ENABLE_APRS_DIGI` | −96 — see note | |

Diagnostic and build flags: `ENABLE_AM_FIX_SHOW_DATA` 188 (✗),
`ENABLE_UART_RW_BK_REGS` 80, `ENABLE_OVERLAY` 652 (✗), `ENABLE_SWD` 4,
`ENABLE_AGC_SHOW_DATA` −16, `ENABLE_MIC_PROBE` −124,
`ENABLE_FEAT_F4HWN_DEBUG` −144, `ENABLE_EXPERIMENTAL_CLFAGS` −56.
**`ENABLE_LTO` is worth 3832 bytes** — more than every optional feature here
put together.

`ENABLE_APRS_ACOUSTIC` is deliberately absent: it is unreleased work in
progress, so its cost moves. Measure it yourself if you need the figure, with
the flag's own automatic cuts held off.

## Notes that change how you read the table

- **Some flags are coupled, and the sweep sees the net effect.** Three rows in
  this table are not what they look like:
  - `ENABLE_AMATEUR_BAND_ONLY` reads −52 because turning it off both removes
    the band restriction (−192) *and* restores `RESUME_STATE` (+232), which the
    Makefile ties to it.
  - `ENABLE_FEAT_F4HWN_RESUME_STATE` reads 0 because the amateur baseline
    already has it forced off; flipping its declaration changes nothing. It is
    worth ~232 bytes where it is actually on, i.e. the all-band image.
  - `ENABLE_APRS_DIGI` reads −96 because enabling it forces `AM_FIX` off
    (−616) while adding ~520 of digipeater, so the image gets *smaller*.
- **LTO makes these non-additive.** Cutting two 500-byte features will not free
  1000 bytes, and several flags measure exactly 0 because link-time
  optimisation absorbs them. Always re-measure the combination you ship.
- **`ENABLE_FEAT_F4HWN_SPECTRUM` costs nothing because it does nothing here.**
  It only gates code inside `app/spectrum.c`, which is not compiled while
  `ENABLE_SPECTRUM=0`. It appears in the build's `-D` list and looks enabled;
  the analyzer is not in the image. The analyzer itself needs ~6.5 KB and
  cannot coexist with APRS.
- **✗ rows are estimates.** They overflowed the program area, so the size comes
  from the linker's overflow figure rather than a real binary — good to a few
  bytes, not exact.

## Configurations that do not compile

Found by the sweep, none of them shipped, and all but the first pre-existing:

| flag | error |
|---|---|
| `ENABLE_UART=0` | `-Werror=unused-parameter` on `APRS_EmitRaw`'s arguments, `app/aprs_minimal.c` — ours, two `(void)` casts would fix it |
| `ENABLE_BIG_FREQ=0` | `'att' undeclared`, `ui/main.c:934` |
| `ENABLE_FEAT_F4HWN=0` | `'POWER_ON_DISPLAY_MODE_SOUND' undeclared`, `driver/backlight.c:71` |
| `ENABLE_VOICE=1` | `'Key' undeclared`, `app/main.c:356` |
| `ENABLE_FEAT_F4HWN_SCREENSHOT=1` | `unknown type name 'bool'`, `driver/uart.h:30` |
| `ENABLE_SPECTRUM=1` | `APRS_MAX_PACKET_SIZE` redefined between `app/aprs_minimal.h` and `driver/bk4819.h` |

## Reproducing this

```bash
sh utils/flag_cost_sweep.sh docs/flagcost.tsv    # ~7 minutes, ~45 clean builds
```

The script derives the baseline from a plain `make`, so it follows the edition
defaults automatically. It leaves the default build in the tree when it
finishes. Re-run it after any change that moves the baseline, and update the
date at the top.
