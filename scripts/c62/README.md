<!--
 - SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 -
 - SPDX-License-Identifier: GPL-3.0-or-later
-->

# Retevis C62: build, flash, capture RX baseband

## 1. Build with Docker

You only need Docker and git. The image holds the toolchain (Zephyr SDK
0.16.8, `west`, CMake, Ninja). Your source tree stays on your disk and is
mounted into the container. ListenAI's `lisa` tool is not needed.

The C62 builds against ListenAI's Zephyr 3.4 fork. `west` puts it, and the
CSK6 modules, next to the OpenRTX checkout, so make a folder to hold both:

```bash
mkdir c62-ws && cd c62-ws
git clone -b c62/tetra https://github.com/sp5wwp/OpenRTX.git

# Build the image once (5-10 minutes, ~2 GB)
docker build -t openrtx-c62 OpenRTX/scripts/c62

# Build the firmware. The first run downloads the Zephyr fork (~1.5 min).
docker run --rm -it --user "$(id -u):$(id -g)" \
    -v "$PWD":/work openrtx-c62
```

The outputs are:

| File | Flash at |
|---|---|
| `OpenRTX/build_c62/zephyr/zephyr.bin` | `0x000000` |
| `OpenRTX/build_c62/zephyr/dsp_firmware.bin` (downloaded, hash-checked) | `0x100000` |

Run the same `docker run` command after each code change. The build is
incremental.

**Build options** go after the image name, for example:

```bash
docker run --rm -it --user "$(id -u):$(id -g)" \
    -v "$PWD":/work openrtx-c62 c62-build -DC62_REG43_SWEEP=ON
```

| Option | Default | Effect |
|---|---|---|
| `C62_WIDE_RX` | `ON` | M17-mode RX uses BK4819 `REG_43 = 0x7e28`, the widest documented filter (see section 4) |
| `C62_BASEBAND_DUMP` | `OFF` | Send raw 48 kHz RX audio over the UART. The console log is muted after audio starts |
| `C62_BASEBAND_DUMP_BAUD` | `115200` | UART baud rate during the dump |
| `C62_BASEBAND_DUMP_BURST` | `ON` | Record 1 MiB (10.4 s) to PSRAM, send it, repeat |
| `C62_BASEBAND_DUMP_FORMAT` | `1` | `1` = 16-bit samples, `2` = 12-bit packed (13.9 s per burst) |
| `C62_TETRA` | `ON` | TETRA receive mode "T" (section 5), with libtetra |
| `C62_TETRA_SPEECH` | `ON` | Play clear TETRA speech, with OpenACELP (section 5) |
| `C62_DMR` | `ON` | DMR receive mode "DMR" (section 6) |
| `C62_DMR_VOICE` | `AUTO` | DMR speech with mbelib: on when it was fetched (section 6) |
| `C62_AM` | `ON` | AM receive mode "AM", for the air band (section 7) |
| `C62_ACARS` | `ON` | ACARS receive mode "ACARS" (section 13) |
| `C62_AIS` | `ON` | AIS receive mode "AIS" (section 14) |
| `C62_FM_BROADCAST` | `ON` | Broadcast FM radio on the BK1080, MONI + # (section 15) |
| `C62_SPECTRUM` | `ON` | Spectrum view, MONI + 0 on the VFO screen (section 9) |
| `C62_RICH_UI` | `ON` | Main screen with a mode badge, the receiver state (RX, TX, SYNC, LOCK), call cards and a segmented S-meter, also in the macro menu (squelch level and threshold in FM and AM); macro keys 7 and 8 set the frequency step instead of the brightness. `OFF` keeps the default OpenRTX screen and macro menu |
| `C62_REG43_SWEEP` | `OFF` | Cycle `REG_43` through 7 test values in M17 RX |
| `C62_REG43_SWEEP_MS` | `1400` | Recorded time per sweep value (one burst covers all 7 values) |

CMake remembers options in `build_c62`. To drop one, pass it again with its
default value or delete `OpenRTX/build_c62`.

Notes:

- Windows: run this from a WSL2 shell, with the folder inside WSL.
- Apple silicon: the image builds natively for arm64.
- Without Docker: the same steps are in `build.sh`. Install the Zephyr SDK
  0.16.8 `arm-zephyr-eabi` toolchain, `west`, `cmake`, `ninja`, `gperf`,
  `dtc` and `meson`, then follow the script.

## 2. Flash

Use [`cskburn`](https://github.com/LISTENAI/cskburn/releases) over the
Kenwood programming cable. To enter burn mode: radio off, hold the lower
side key, power on, release the key. The screen stays dark. Don't press the
main PTT while flashing: it shares the boot ROM's UART RX line. `cskburn`
resets the radio after each command, so enter burn mode again before each
one.

```bash
# Once, before anything else: back up your radio's own flash (it is per-unit)
cskburn -s /dev/ttyUSB0 -C 6 -b 115200 -v --read 0x0:0x400000:backup.bin

cskburn -s /dev/ttyUSB0 -C 6 -b 115200 -v --verify-all \
    0x100000 OpenRTX/build_c62/zephyr/dsp_firmware.bin \
    0x000000 OpenRTX/build_c62/zephyr/zephyr.bin
```

## 3. Capture the RX baseband

The dump is off by default: build with `-DC62_BASEBAND_DUMP=ON` (section 1)
and flash that firmware first. It uses the UART TX line on the Kenwood
connector, which is the line your programming cable receives on, and mutes
the console log once audio starts. Put the radio in **M17 mode** on the
wanted frequency. M17 mode applies the flat audio path: AF bypass, no
emphasis, no DC filters and the wide `REG_43`. Then:

```bash
pip install pyserial
python3 OpenRTX/scripts/c62_bbdump.py -p /dev/ttyUSB0 -t 200 -o tetra
```

A 115200-baud cable moves about 11.5 kB/s, and 48 kHz 16-bit audio is
96 kB/s, so the radio works in bursts. It records 10.4 s without gaps into
PSRAM, then takes about 91 s to send them. Recording starts again once the
burst is sent. `-t 200` gets about two bursts. Each burst becomes its own
file set:

- `tetra_burst001.wav`: 48 kHz mono 16-bit, for listening, sox or Audacity.
- `tetra_burst001.f32`: float32 in [-1, 1), for a GNU Radio *File Source*
  (float, 48 kHz).
- `tetra_burst001.csv`: per 5 ms packet, the sample index, `REG_43` value,
  RSSI (dBm) and flags.

The summary line reports peak and RMS level and a count of clipped samples.
**Check that clipping is 0 on a real TETRA signal.** Its discriminator output
swings much further than the ±2 kHz test tones. If it clips, lower the BK4819
RX gain (`REG_48`, in `m17Fields` in `radio_C62.cpp`) or `C62_ADC_RX_GAIN_*`
in `hwconfig.h`.

Use `--raw capture.bin` to also keep the raw bytes, and `-i capture.bin` to
parse them again later. Stray bytes from the boot log or the DSP are skipped
by resynchronising on the packet header and checking a CRC. The packet format
is documented in `platform/drivers/audio/baseband_dump_c62.h`.

With a faster USB-UART adapter (CH340, FT232R, CP2102N), build with
`-DC62_BASEBAND_DUMP_BURST=OFF -DC62_BASEBAND_DUMP_BAUD=1500000` for a
continuous stream. Pass the same `-b` to the script. 921600 baud is enough
with `-DC62_BASEBAND_DUMP_FORMAT=2`.

## 4. RX bandwidth (BK4819 REG_43)

From the BK4819 V3 register list:

| Bits | Field |
|---|---|
| `<14:12>` | RF (channel) filter bandwidth: 1.7, 2.0, 2.5, 3.0, 3.75, 4.0, 4.25, 4.5 kHz |
| `<11:9>` | Same filter, used instead when the signal is **weak** |
| `<8:6>` | AF TX LPF2 (TX only) |
| `<5:4>` | BW mode: 0 = 12.5 kHz, 1 = 6.25 kHz, 2 = 25 kHz. Bit 5 doubles the filter bandwidth and the IF |
| `<3>` | Undocumented, set in every known firmware |
| `<2>` | +6 dB after the FM demodulator |

The measured `0x7828` (25 kHz mode) left `<11:9>` at 3.75 kHz. The chip then
narrows the channel filter on weak signals, which is when a wide-band digital
signal can least afford it. `C62_WIDE_RX` uses `0x7e28` (4.5 kHz × 2 in both
cases), the widest documented setting. Above that are only undocumented bits.
`C62_REG43_SWEEP` measures them with your multitone setup. Each burst then
holds 1.4 s each of `7808 7828 7e28 7e2c 7e38 7e20 fe28`. Split them with
`c62_bbdump.py ... --split-reg43` and skip about 50 ms at each value change.

## 5. TETRA mode

Press **5** on the main screen to cycle the modes: FM, M17, T, DMR, AM. In **T**
mode the radio receives a TETRA TMO downlink (a base station, pi/4-DQPSK)
and decodes it on the radio with
[libtetra](https://github.com/sp5wwp/libtetra). It is receive only: PTT does
nothing. Tune to the base station's downlink frequency as usual.

The main screen shows:

| State | Line 1 | Line 2 | Rest |
|---|---|---|---|
| No signal | Demodulator figures | `T searching` | Frequency |
| Locked to a cell | Last SDS text (scrolling, 30 s) or demodulator figures | `T <MCC>-<MNC> LA <n>` | Frequency |
| Call | Talking party SSI | Called group (`G`) or individual SSI | Cell, then `Voice` / `Idle` / `Encrypted` / `Encrypted?` |

The green LED is on while the radio is locked to a cell. A call stays on
screen for 10 s after its last activity unless its release is decoded.
Speech starts 150 ms after the first traffic slot: TETRA leaves a gap of
113 ms in every multiframe (frame 18), which the buffer must cover. A
dash means the network has not sent that identity since the radio started
listening, typically because the call was set up before.

The demodulator figures on line 1, e.g. `e11% -234Hz o0 r1`:

- `e`: RMS phase error in % of pi/4. About 10 to 20% on a clean signal;
  decoding fails above about 45%, where the demodulator starts again.
- the carrier offset in Hz.
- `o`: overruns, blocks of samples lost because the radio could not keep
  up. This should stay at 0: if it counts up, the radio is short of CPU
  time, and speech will break up.
- `r`: restarts: of the burst synchronisation, at start and after an
  overrun, and of the demodulator, when it re-acquires or follows a jump in
  the signal's timing. A replayed recording causes one at every loop. A
  demodulator restart keeps the burst synchronisation, and with it the cell,
  the call and the SDS being received: libtetra finds its bursts again
  within two symbols. Without synchronisation, or after three re-acquisitions
  within 5 s, the demodulator starts afresh. The first 240 ms after tuning
  are skipped while the radio audio settles.

How it works: the BK4819 runs the same flat, wide receive path as M17
(`REG_43 = 0x7e28`, AF bypass). The 48 kHz discriminator output goes to
`openrtx/src/protocols/TETRA/TetraDemod.c`: integrate-and-dump over each
symbol, a timing loop for the sample clock offset, a running fit of gain
and carrier offset, and a 9-tap equalizer seeded with taps measured on the
C62 (`CONFIG_TETRA_EQ_INIT` in `hwconfig.h`). Its soft bits go to libtetra.

Speech: clear (not encrypted) speech is played on the speaker. libtetra
decodes each traffic slot into two speech frames, and
[OpenACELP](https://github.com/sp5wwp/OpenACELP) decodes them into 8 kHz
audio, with its built-in TETRA codebooks (EN 300 395-2). The speaker is
released after one second without speech. `-DC62_TETRA_SPEECH=OFF` builds
without it.

The network time the cell broadcasts keeps running on the radio's tick
once received, also outside TETRA: the receive-only modes show it on the
top bar of the main screen in place of `RX only`. It is lost at power
off.

Only calls whose signalling was received in clear are played, as on the
ESP32-P4 SDR: end-to-end encrypted calls (flagged in their setup) show
`Encrypted`. Air-encrypted calls, whose signalling cannot be read, and
calls set up before the radio tuned in show `Encrypted?` and stay silent.
Their traffic would only decode to noise.

**\*** on the VFO or memory screen cycles the TETRA pages; on a page, a
number goes straight to another (shown on the top bar, `3/8`), **0** back
to the main one:

- **Main**: the cell, the call, the last SDS text or status on line 1.
- **Diagnostics** (1): sync, phase error (10-20 % is a clean signal, 58 % is
  noise), carrier offset, lost sample blocks (`o`) and resyncs (`r`), the
  cell, the last call signalled (usage marker, timeslots) and the last
  traffic slot, the traffic not played and why (unknown call, encrypted,
  data), and the speech frames queued, bad and played. No PC needed.
- **SDS** (2): the last 10 short data messages, newest first; **UP** / **DOWN**
  browse them, **ESC** goes back. Status, text (simple, immediate, SDS-TL,
  with user data header), concatenated messages (UDH and SDS-TL, shown
  once complete, or after two minutes with the parts received), LIP
  positions, delivery reports. End-to-end encrypted messages are listed
  without their contents. Decoding ported from the ESP32-P4 SDR.
- **Groups** (3): the groups heard in clear signalling, most recent first,
  with their calls (`c`), floor grants (`t`), `enc` if the last call was
  encrypted, and their age; under the list, the picked group's last
  talker and the radios seen in it. **ENTER** (green key) filters the
  picked group: only its clear calls are played, the others count as
  `Filtered` on the call card and in the debug page (`flt`). The filtered
  group is drawn green and marked `F` in the top bar; ENTER on it again
  removes the filter.
- **Radios** (4): the radios heard (callers, talkers, SDS senders,
  registrations), most recent first, with their last group and age;
  under the list, the picked radio's last activity, its calls and SDS,
  and its last position from a location report. UP / DOWN pick, ESC goes
  back, on both pages; they are cleared on a new frequency.
- **Calls** (5): the calls heard in clear signalling, most recent first,
  with their called party (`G` group, `I` individual), last talker and
  age, `now` while active; under the list, the picked call's state,
  length, encryption and talkers.
- **Cell** (6): network, colour code, location area, downlink and uplink,
  carrier, band, offset and duplex, services, air encryption, load,
  system code and sharing, hyperframe and CCK, and the network date and
  time.
- **Neighbours** (7): the cells the serving one announces, by frequency,
  with their load (`lo`, `med`, `hi`), `S` if synchronised, and their
  network or location area when not the serving cell's.
- **Symbols** (8): the phase change of the last 256 symbols on a circle
  (four clusters at +-45 and +-135 degrees on a clean signal), and the
  histogram of the raw discriminator values with marks at the four levels
  the demodulator expects. Marks off the peaks mean a wrong offset or
  gain: the demodulator then estimates them again at its next
  re-acquisition, within a few seconds.

A firmware built with the baseband dump keeps it working in T mode, to
capture what the radio receives.

## 6. DMR mode

**DMR** follows **T** under key **5**. The radio receives DMR in the clear:
repeater outputs (both timeslots), simplex and hotspots. It is receive
only: PTT does nothing. Tune to the repeater output or simplex frequency.

The 48 kHz discriminator output, through the same flat, wide path as M17,
goes to a software 4FSK demodulator (`openrtx/src/protocols/DMR/`): RRC
matched filter, sync search by correlation against the 8 sync words, levels
and offset fitted on the sync symbols (no AGC), then CACH, slot type,
BPTC(196,96) + RS(12,9) LC headers and terminators, CSBK, EMB and embedded
LC with the talker alias. The polarity is found by itself.

| State | Line 1 | Line 2 | Rest |
|---|---|---|---|
| No signal | Demodulator figures | `DMR searching` | Frequency |
| Locked | Demodulator figures | `DMR RPT CC<n>` (repeater), `MS` (simplex) or `DM` (direct mode) | Frequency |
| Call | Talker alias, else source ID | `TG <n>`, or the called radio ID | Source ID or slot and colour code, then `Voice` / `Encrypted` |

The demodulator figures on line 1, `L<level> <dc> o<lost>`: the level of the
outer symbols and the DC offset, in ADC units, the sample blocks lost
because the radio could not keep up (should stay 0), and `inv` when the
symbols arrive inverted. The green LED is on while the radio is locked.
Call events (start, IDs, alias, end) are also printed on the console.

**Voice.** DMR speech is AMBE+2, decoded with
[mbelib](https://github.com/lwvmobile/mbelib) (ISC licence). The AMBE+2
algorithm is covered by patents in some countries, so mbelib is not fetched
by default. Check that using it is lawful where you are, then:

```bash
OpenRTX/scripts/c62/fetch_mbelib.sh      # from the workspace folder
```

and build again: the build log says `DMR ON (voice ON)`. Only one slot is
played: the first one to start talking, until its call ends. Encrypted
calls are shown but not played. Line 4 shows the vocoder time per 20 ms
frame (`Voice 3.2ms`). A firmware built with mbelib contains it.

## 7. AM mode (air band)

**AM** follows **DMR** under key **5**. It receives AM with the BK4819's own
AM detector, through the same audio path and RF squelch as FM, with the
12.5 kHz channel filter. It is receive only.

The C62's VHF front end is meant for 136-174 MHz: in the air band the
sensitivity is likely lower.

The BK4819's AGC is tuned for FM: on a strong AM carrier it leaves the IF
overdriven and the AM detector clips. In AM the driver freezes the chip's
AGC and sets the RX gain itself from the RSSI ("AM fix"): it attenuates at
once when the measured RSSI goes above -89 dBm, and gives the gain back in
steps after 0.5 s below -99 dBm. The S-meter and the squelch use the RSSI
corrected for that attenuation. Each gain change is printed on the
console, at most once a second:

```
AM gain -37 dB, RSSI -62 dBm
```

## 8. Frequency range

The radio receives on 18-660 MHz, the BK4819's continuous tuning range:
the VHF LNA is used up to 174 MHz, the UHF LNA above. Both are made for
136-174 and 400-480 MHz, so the sensitivity drops outside those bands.
It only transmits on 136-174 and 400-480 MHz.

## 9. Spectrum view

On the VFO screen, **MONI + 0** opens a spectrum of the RSSI around the VFO
frequency: 80 points over 1 MHz (12.5 kHz apart) at first, swept again and
again, about 0.3 s per sweep. The operating mode is suspended meanwhile: no
audio, no transmission. The gain is fixed and the channel filter is the
12.5 kHz one. Each point waits 3.2 ms for the RSSI to settle, as on the
UV-K5. A bar rises at once with the signal and falls back slowly, so that
the noise does not flicker; the 60 dB high scale starts just below the
noise floor and moves only when the floor moves by 6 dB.

Under the bars, a waterfall shows the raw sweeps, the newest at the top,
about 13 s of history: black at the noise floor, then blue, cyan, yellow,
red and white 40 dB above it. Two yellow ticks mark the marker's column.
A new range starts it again.

| Key | Action |
|---|---|
| **UP** / **DOWN** | Move the marker one point |
| **0** | Marker to the strongest point |
| **\*** / **#** | Zoom in / out around the marker (100 kHz to 20 MHz) |
| **P1** / **P2** | The next span down / up, side by side with this one |
| **5** | Type a centre frequency, `___.____` MHz: ENTER sweeps around it, ESC cancels |
| **ENTER** | Centre the view on the marker |
| **PTT** | Tune the VFO to the marker and go back to it |
| **ESC** | Go back to the VFO, unchanged |

PTT keeps the TX offset of the VFO. It does not transmit: TX stays off
until the next key on the VFO screen. Points wider apart than the channel
filter (spans above 1 MHz) can miss narrow signals between them.

## 10. Saved settings

The settings and the VFO (frequency, mode, power, bandwidth, tones) are
saved when the radio is turned off with the knob, and loaded at power on.
The C62's MCU stays powered with the knob off: OpenRTX sees the battery
reading drop, saves and resets. Removing the battery, or letting it run
flat, does not save.

They live in an emulated EEPROM in the first 16 kB of the OpenRTX storage
partition of the flash (0x2b0000, see `c62.dts`), away from the factory
calibration (0x3b0000) and channels (0x3c0000). The console shows each
load and save:

```
NVM: VFO 390212500 Hz, mode 4
NVM: saved VFO 145500000 Hz mode 5 (0), settings (0)
```

A firmware whose settings or channel structure differs starts from the
defaults.

## 11. Channel memories

**P2** on the VFO screen saves the VFO in a memory: a list of the
memories opens, with a new one at the end, picked by default.

| Key | Action |
|---|---|
| **UP** / **DOWN** | Pick a memory to overwrite, or the new one |
| **ENTER** | Save the VFO there, named after its mode and frequency (`T 390.2125`) |
| **#** twice | Delete the memory picked: the next ones move up |
| **ESC** / **P2** | Back to the VFO |

The red key (ESC) on the VFO screen switches to the memories, UP and DOWN
go through them, and the menu's Channels list them. A memory holds the
whole channel: mode, RX and TX frequencies, power, bandwidth and tones.
The main screen starts its information line with `VFO`, or with the
memory number (`M001`) in yellow.

Up to 128 memories live after the saved settings in the OpenRTX storage
partition (0x2b4000, 16 kB); each carries a magic and a CRC. Saving or
deleting erases and writes 4 kB flash sectors with interrupts off: the
audio may stutter for a moment.

## 12. Flashlight

**MONI + \*** turns the white flashlight LED on and off.

## 13. ACARS mode

**ACARS** follows **AM** under key **5**. It receives the AM detector's
audio, flattened (no de-emphasis or voice filters), and decodes ACARS
(ARINC 618: 2400 bit/s MSK on 1200 and 2400 Hz) in software. Main VHF
ACARS channels in Europe: 131.525, 131.725 and 131.825 MHz. It is receive
only, and leaving it with `C62_ACARS=OFF` removes it entirely.

The main screen shows the messages received and the frames dropped on a
bad CRC, the newest message's aircraft, flight, label and text. The green
LED is on while a frame comes in, and the screen wakes for each message.
**\*** opens the message list (16 kept, newest first): **UP/DOWN** pick a
message, **\*** or **ESC** close it.

The decoder looks for the SYN SYN sync word at every sampling phase and
under six tone-to-bit mappings, the coherent MSK of real transmitters
among them. Characters failing parity are repaired when flipping bits
makes the CRC good (up to two per frame).

The Flipper Zero test files of the
[UV-K1/K5 custom firmware](https://github.com/armel/uv-k1-k5v3-firmware-custom/tree/main/App/apps/acarsrx/test)
(OOK on 433.650 MHz) decode with the radio in ACARS mode on 433.650 MHz.
Their frames use a letter block identifier with a flight number, so the
flight shows inside the text.

## 14. AIS mode

**AIS** follows **ACARS** under key **5**. It receives the FM
discriminator through the same wide, flat path as TETRA and decodes AIS
(ITU-R M.1371: 9600 bit/s GMSK, NRZI, HDLC) in software. Tune the VFO to
**AIS1, 161.975 MHz** or **AIS2, 162.025 MHz**: one channel at a time. It
is receive only, and building with `C62_AIS=OFF` removes it entirely.

Stations are kept in a table of 32, merged by MMSI, the oldest replaced
when it is full: positions from messages 1, 2, 3 (class A), 18 and 19
(class B), 4 (base stations), 9 (SAR aircraft), 21 (aids to navigation)
and 27 (long range); names, call signs, ship types, dimensions and
destinations from 5, 19, 21 and 24. Positions marked not available are
not shown.

The main screen shows the stations, the reports received, those of them
repaired and the frames dropped on a bad CRC (`rx 24 fix 3 bad 7`, or
`24/3/7` when long), and the
newest station: its name (or MMSI until a static report names it), class
and age, then its position, speed, course, navigational status and
destination scrolling. The green LED flashes for each report and the
screen wakes. **\*** opens the station page: one station whole (MMSI,
call sign, position, SOG / COG / heading, status and type, destination,
dimensions and draught, reports), **UP/DOWN** go through them, newest
first, **\*** or **ESC** close it.

The receiver takes the centre and amplitude of the signal from its
highest and lowest levels over the last 13 bits, so a carrier offset or a
burst starting after noise settles within a few bits; three slicers with
different hysteresis, each with its own bit clock, decode every burst and
the first valid frame counts. The bit clock follows the transitions
quickly while searching and slowly within a frame, so that noise does not
make it slip a bit. A frame failing its CRC, or cut short by a false flag
(one level wrong is two bits wrong after NRZI, and can fake a flag), is
repaired: its 20 least sure levels are flipped, alone and in pairs, and
the bits read up to each known message length until the CRC is good and
the length fits the message type. On the host test bench this decodes
about twice as many noisy frames as before, with no false frame from
noise.

The Flipper Zero test files of the
[UV-K1/K5 custom firmware](https://github.com/armel/uv-k1-k5v3-firmware-custom/tree/main/App/apps/aisrx/test)
(2-FSK on 433.650 MHz) decode with the radio in AIS mode on 433.650 MHz:
all of them but `ais_badcrc.sub`, which counts as `bad`. `ais_inland.sub`
(message 8, Inland AIS) only adds a report to its MMSI.

## 15. FM radio

The C62 has a BK1080 broadcast FM receiver whose audio goes to the
speaker amplifier directly. On the VFO or memory screen, **MONI + #**
opens the FM radio: the operating mode is suspended (no audio from the
main receiver, no transmission) and the BK1080 plays, the speaker held on
whatever the beeps and voice prompts do. **ESC** goes back to the mode
as it was. The screen may go off meanwhile: the radio keeps playing. The
station and volume are kept until the radio is turned off, 100.1 MHz and
10 at first.

| Key | Action |
|---|---|
| **UP** / **DOWN** | ±100 kHz, around the 87.5-108 MHz band |
| **P1** / **P2** | Seek the next station down / up |
| **\*** / **#** | Volume down / up, 0-15 (the BK1080's own) |
| **0-9** | Type a frequency in tenths of MHz (`1001` is 100.1, `875` is 87.5): ENTER tunes, ESC cancels |
| **ESC** | Back to the VFO or memory screen |

A seek stops on a station as the UV-K5 firmware does: SNR above 2, RSSI
10 or more, the AFC not railed and the frequency error near zero; or
after a whole turn of the band. The screen shows the frequency, the
state (`Station`, `No station`, `Seeking up/down`), a signal bar and the
chip's RSSI and SNR.

Bus traffic near the audio path is heard as clicks: the BK1080 is read
once a second, and the screen redraws at once only for a new frequency,
station state or seek, the signal figures at most every 3 s.
