#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Edit the channel memories of the Retevis C62 on the PC.

The memories are a 16 kB table in the internal flash, at 0x2b4000: 128
slots of 128 bytes, M001 to M128, each with a magic, a CRC-CCITT and the
packed channel_t (see platform/drivers/NVM/cps_io_C62.c). This tool reads
that table from the radio with cskburn, shows it by pages of 6 memories as
the radio does, lets you edit, move and delete memories, and writes the
table back. It never writes anywhere else: the factory calibration
(0x3b0000) is never touched.

The radio must be in burn mode for each read or write: radio off, hold the
lower side key, power on, release the key. The screen stays dark.

Every table read from the radio is also kept in the backup folder, by date,
to go back to it.

Examples:
  # The editor window (needs Tk: sudo apt install python3-tk)
  c62_memories.py
  c62_memories.py memories.bin
  # List a table without a window
  c62_memories.py --list memories.bin
"""

import argparse
import binascii
import datetime
import glob
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

# The table in flash: the storage partition (0x2b0000) after the settings
TABLE_ADDR = 0x2B4000
SLOTS = 128
SLOT_SIZE = 128
TABLE_SIZE = SLOTS * SLOT_SIZE
SLOT_MAGIC = 0xC62C4A11
SLOT_HEADER = struct.Struct("<IHH")  # magic, CRC of the channel, its size

# channel_t, packed, as the firmware lays it out (core/cps.h)
CHANNEL_SIZE = 94
OFF_MODE = 0
OFF_FLAGS = 1  # bandwidth : 2, rx_only : 1
OFF_POWER = 2
OFF_RX = 6
OFF_TX = 10
OFF_NAME = 16
OFF_DESCR = 48
OFF_UNION = 89  # fm, dmr or m17 block
STR_SIZE = 32

# Memories shown on a page of the radio's lists
PAGE_ROWS = 6
PAGES = (SLOTS + PAGE_ROWS - 1) // PAGE_ROWS
# Characters of a name the radio shows after "M001 "
NAME_SHOWN = 16

# Frequencies the C62 tunes (platform/targets/c62/platform.c), in Hz
FREQ_MIN = 18_000_000
FREQ_MAX = 660_000_000

MODES = ["None", "FM", "DMR", "M17", "TETRA", "AM", "ACARS", "AIS", "NXDN"]
# Labels the radio puts in front of a memory's name (_ui_modeLabel)
MODE_LABELS = ["", "FM", "DMR", "M17", "T", "AM", "ACARS", "AIS", "NXDN"]
MODE_FM, MODE_DMR, MODE_M17 = 1, 2, 3
BANDWIDTHS = ["12.5 kHz", "25 kHz"]
POWERS = {1000: "1 W", 2500: "2.5 W", 5000: "5 W"}

# CTCSS tones, in tenths of Hz (core/cps.c)
CTCSS = [
    670, 693, 719, 744, 770, 797, 825, 854, 885, 915, 948, 974, 1000, 1035,
    1072, 1109, 1148, 1188, 1230, 1273, 1318, 1365, 1413, 1462, 1514, 1567,
    1598, 1622, 1655, 1679, 1713, 1738, 1773, 1799, 1835, 1862, 1899, 1928,
    1966, 1995, 2035, 2065, 2107, 2181, 2257, 2291, 2336, 2418, 2503, 2541,
]

BURN_MODE_HINT = ("Put the radio in burn mode: radio off, hold the lower "
                  "side key, power on, release the key. The screen stays "
                  "dark.")


def crc_ccitt(data):
    """CRC-16 of the firmware's crc_ccitt(): polynomial 0x1021, from 0."""
    return binascii.crc_hqx(bytes(data), 0)


def ctcss_label(index):
    return "%.1f" % (CTCSS[index] / 10) if index < len(CTCSS) else "?"


class Memory:
    """
    A channel_t, kept as its bytes: the fields the editor does not know
    (description, location, scan lists) are written back as they were.
    """

    def __init__(self, raw=None):
        if raw is None:
            raw = bytearray(CHANNEL_SIZE)
            raw[OFF_MODE] = MODE_FM
            raw[OFF_FLAGS] = 1  # 25 kHz
            struct.pack_into("<I", raw, OFF_POWER, 1000)
            struct.pack_into("<II", raw, OFF_RX, 145_500_000, 145_500_000)
        assert len(raw) == CHANNEL_SIZE
        self.raw = bytearray(raw)

    def copy(self):
        return Memory(self.raw)

    def _u32(self, off):
        return struct.unpack_from("<I", self.raw, off)[0]

    def _set_u32(self, off, value):
        struct.pack_into("<I", self.raw, off, value)

    def _nibbles(self, off):
        b = self.raw[off]
        return b & 0x0F, b >> 4

    def _set_nibbles(self, off, low, high):
        self.raw[off] = (low & 0x0F) | ((high & 0x0F) << 4)

    @property
    def mode(self):
        return self.raw[OFF_MODE]

    @mode.setter
    def mode(self, value):
        if value == self.raw[OFF_MODE]:
            return
        self.raw[OFF_MODE] = value
        # The block after the name is read by the new mode: clear it
        self.raw[OFF_UNION:OFF_UNION + 5] = bytes(5)
        if value == MODE_DMR:
            self.dmr_cc = (1, 1)
            self.dmr_timeslot = 1
        elif value == MODE_M17:
            self.raw[OFF_UNION + 1] = 1  # Voice, no encryption

    @property
    def bandwidth(self):
        return self.raw[OFF_FLAGS] & 0x03

    @bandwidth.setter
    def bandwidth(self, value):
        self.raw[OFF_FLAGS] = (self.raw[OFF_FLAGS] & ~0x03) | (value & 0x03)

    @property
    def rx_only(self):
        return bool(self.raw[OFF_FLAGS] & 0x04)

    @rx_only.setter
    def rx_only(self, value):
        self.raw[OFF_FLAGS] = (self.raw[OFF_FLAGS] & ~0x04) | (4 if value
                                                               else 0)

    @property
    def power(self):
        return self._u32(OFF_POWER)

    @power.setter
    def power(self, mw):
        self._set_u32(OFF_POWER, mw)

    @property
    def rx(self):
        return self._u32(OFF_RX)

    @rx.setter
    def rx(self, hz):
        self._set_u32(OFF_RX, hz)

    @property
    def tx(self):
        return self._u32(OFF_TX)

    @tx.setter
    def tx(self, hz):
        self._set_u32(OFF_TX, hz)

    @property
    def name(self):
        raw = bytes(self.raw[OFF_NAME:OFF_NAME + STR_SIZE])
        return raw.split(b"\0", 1)[0].decode("ascii", "replace")

    @name.setter
    def name(self, text):
        data = text.encode("ascii")[:STR_SIZE - 1]
        self.raw[OFF_NAME:OFF_NAME + STR_SIZE] = data.ljust(STR_SIZE, b"\0")

    def shown_name(self):
        """The name as the radio lists it: old ones get their mode first."""
        name = self.name
        if name.startswith(" "):
            return self.mode_label() + name
        return name

    def mode_label(self):
        m = self.mode
        return MODE_LABELS[m] if m < len(MODE_LABELS) else ""

    def auto_name(self):
        """The name the radio gives a memory saved with P2."""
        mhz, rest = divmod(self.rx, 1_000_000)
        return "%s %u.%04u" % (self.mode_label(), mhz, rest // 100)

    # FM: tone enable in bit 0, tone index in bits 1-7, RX then TX
    def _tone(self, off):
        b = self.raw[off]
        return (b >> 1) if (b & 1) else None

    def _set_tone(self, off, index):
        self.raw[off] = 0 if index is None else ((index << 1) | 1)

    @property
    def rx_tone(self):
        return self._tone(OFF_UNION)

    @rx_tone.setter
    def rx_tone(self, index):
        self._set_tone(OFF_UNION, index)

    @property
    def tx_tone(self):
        return self._tone(OFF_UNION + 1)

    @tx_tone.setter
    def tx_tone(self, index):
        self._set_tone(OFF_UNION + 1, index)

    # DMR: RX and TX color codes, timeslot
    @property
    def dmr_cc(self):
        return self._nibbles(OFF_UNION)

    @dmr_cc.setter
    def dmr_cc(self, rx_tx):
        self._set_nibbles(OFF_UNION, rx_tx[0], rx_tx[1])

    @property
    def dmr_timeslot(self):
        return self.raw[OFF_UNION + 1]

    @dmr_timeslot.setter
    def dmr_timeslot(self, ts):
        self.raw[OFF_UNION + 1] = ts

    # M17: RX and TX channel access numbers
    @property
    def m17_can(self):
        return self._nibbles(OFF_UNION)

    @m17_can.setter
    def m17_can(self, rx_tx):
        self._set_nibbles(OFF_UNION, rx_tx[0], rx_tx[1])

    def mode_name(self):
        m = self.mode
        return MODES[m] if m < len(MODES) else "Mode %u" % m

    def power_label(self):
        p = self.power
        return POWERS.get(p, "%u mW" % p)

    def details(self):
        """The mode's own settings, short."""
        m = self.mode
        if m == MODE_FM:
            parts = []
            if self.rx_tone is not None:
                parts.append("RX " + ctcss_label(self.rx_tone))
            if self.tx_tone is not None:
                parts.append("TX " + ctcss_label(self.tx_tone))
            return ", ".join(parts)
        if m == MODE_DMR:
            rx, tx = self.dmr_cc
            return "CC %u/%u TS %u" % (rx, tx, self.dmr_timeslot)
        if m == MODE_M17:
            rx, tx = self.m17_can
            return "CAN %u/%u" % (rx, tx)
        return ""

    def problems(self):
        """Why the radio would not load it, empty if it would."""
        out = []
        for what, f in (("RX", self.rx), ("TX", self.tx)):
            if not FREQ_MIN <= f <= FREQ_MAX:
                out.append("%s frequency out of 18-660 MHz" % what)
        return out


class TableError(Exception):
    pass


def parse_table(data):
    """
    The memories of a table read from the radio, None for the empty slots.

    @raise TableError: the table is not one, or its slots hold a channel_t
                       of another size (another firmware).
    """
    if len(data) != TABLE_SIZE:
        raise TableError("%u bytes, a memory table has %u"
                         % (len(data), TABLE_SIZE))
    memories = []
    for i in range(SLOTS):
        slot = data[i * SLOT_SIZE:(i + 1) * SLOT_SIZE]
        magic, crc, size = SLOT_HEADER.unpack_from(slot)
        if magic != SLOT_MAGIC:
            memories.append(None)
            continue
        if size != CHANNEL_SIZE:
            raise TableError("M%03u holds a %u-byte channel, this tool knows "
                             "the %u-byte one: another firmware version"
                             % (i + 1, size, CHANNEL_SIZE))
        raw = slot[SLOT_HEADER.size:SLOT_HEADER.size + CHANNEL_SIZE]
        # A bad CRC is an empty slot for the radio too
        memories.append(Memory(raw) if crc == crc_ccitt(raw) else None)
    return memories


def build_table(memories):
    """The table to write: empty slots erased (0xff), as the radio does."""
    out = bytearray(b"\xff" * TABLE_SIZE)
    for i, mem in enumerate(memories):
        if mem is None:
            continue
        slot = bytearray(b"\xff" * SLOT_SIZE)
        SLOT_HEADER.pack_into(slot, 0, SLOT_MAGIC, crc_ccitt(mem.raw),
                              CHANNEL_SIZE)
        slot[SLOT_HEADER.size:SLOT_HEADER.size + CHANNEL_SIZE] = mem.raw
        out[i * SLOT_SIZE:(i + 1) * SLOT_SIZE] = slot
    return bytes(out)


def config_path():
    base = os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config"))
    return os.path.join(base, "c62-memories.json")


def load_config():
    """The port and cskburn picked last time."""
    try:
        with open(config_path()) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_config(config):
    try:
        os.makedirs(os.path.dirname(config_path()), exist_ok=True)
        with open(config_path(), "w") as f:
            json.dump(config, f, indent=1)
    except OSError:
        pass


def find_cskburn():
    found = shutil.which("cskburn")
    return found or ""


def cskburn_read_cmd(cskburn, port, path):
    return [cskburn, "-s", port, "-C", "6", "-b", "115200", "-v",
            "--read", "0x%x:0x%x:%s" % (TABLE_ADDR, TABLE_SIZE, path)]


def cskburn_write_cmd(cskburn, port, path):
    return [cskburn, "-s", port, "-C", "6", "-b", "115200", "-v",
            "--verify-all", "0x%x" % TABLE_ADDR, path]


def backup_dir():
    base = os.environ.get("XDG_DATA_HOME",
                          os.path.expanduser("~/.local/share"))
    return os.path.join(base, "c62-memories")


def list_table(path):
    with open(path, "rb") as f:
        memories = parse_table(f.read())
    for i, mem in enumerate(memories):
        if mem is None:
            continue
        print("M%03u  p%-2u %-20s %-6s %11.5f %11.5f %-6s %-8s %s"
              % (i + 1, i // PAGE_ROWS, mem.shown_name(), mem.mode_name(),
                 mem.rx / 1e6, mem.tx / 1e6, mem.power_label(),
                 BANDWIDTHS[mem.bandwidth] if mem.bandwidth < 2 else "?",
                 mem.details()))


def run_gui(path):
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox, ttk
    except ImportError:
        sys.exit("Tk is missing: sudo apt install python3-tk "
                 "(or use --list)")

    class Editor:
        def __init__(self, root):
            self.root = root
            self.memories = [None] * SLOTS
            self.clipboard = None
            self.dirty = False
            self.busy = False
            self.path = None
            self.selected = 0

            root.title("C62 memories")
            root.minsize(900, 560)
            self._build_bar()
            self._build_table()
            self._build_form()
            self._build_log()
            self.refresh()
            self.select(0)
            root.protocol("WM_DELETE_WINDOW", self.quit)
            for d in range(10):
                root.bind("<Alt-Key-%d>" % d,
                          lambda e, d=d: self.go_page(d))

        # Layout --------------------------------------------------------

        def _build_bar(self):
            bar = ttk.Frame(self.root, padding=6)
            bar.pack(fill="x")
            ttk.Label(bar, text="Port").pack(side="left")
            ports = sorted(glob.glob("/dev/ttyUSB*")
                           + glob.glob("/dev/ttyACM*"))
            config = load_config()
            self.port = tk.StringVar(value=config.get("port") or (
                ports[0] if ports else "/dev/ttyUSB0"))
            ttk.Combobox(bar, textvariable=self.port, values=ports,
                         width=14).pack(side="left", padx=(4, 10))
            ttk.Label(bar, text="cskburn").pack(side="left")
            self.cskburn = tk.StringVar(value=config.get("cskburn")
                                        or find_cskburn())
            ttk.Entry(bar, textvariable=self.cskburn, width=28).pack(
                side="left", padx=4)
            ttk.Button(bar, text="...", width=3,
                       command=self.pick_cskburn).pack(side="left")
            self.btn_write = ttk.Button(bar, text="Write radio",
                                        command=self.write_radio)
            self.btn_write.pack(side="right")
            self.btn_read = ttk.Button(bar, text="Read radio",
                                       command=self.read_radio)
            self.btn_read.pack(side="right", padx=4)
            ttk.Separator(bar, orient="vertical").pack(side="right",
                                                       fill="y", padx=6)
            ttk.Button(bar, text="Save .bin",
                       command=self.save_file).pack(side="right")
            ttk.Button(bar, text="Open .bin",
                       command=self.open_file).pack(side="right", padx=4)

        def _build_table(self):
            mid = ttk.Frame(self.root, padding=(6, 0))
            mid.pack(fill="both", expand=True)

            pages = ttk.Frame(mid)
            pages.pack(fill="x", pady=(0, 4))
            ttk.Label(pages, text="Page").pack(side="left")
            self.page = tk.StringVar(value="All")
            values = ["All"] + [str(p) for p in range(PAGES)]
            cb = ttk.Combobox(pages, textvariable=self.page, values=values,
                              width=5, state="readonly")
            cb.pack(side="left", padx=4)
            cb.bind("<<ComboboxSelected>>", lambda e: self.refresh())
            for p in range(10):
                ttk.Button(pages, text=str(p), width=2,
                           command=lambda p=p: self.go_page(p)).pack(
                               side="left")
            self.count = ttk.Label(pages, text="")
            self.count.pack(side="right")

            cols = ("slot", "page", "name", "mode", "rx", "tx", "power",
                    "bw", "details")
            heads = ("Memory", "Page", "Name", "Mode", "RX MHz", "TX MHz",
                     "Power", "Width", "Settings")
            widths = (70, 45, 170, 60, 95, 95, 60, 70, 150)
            self.tree = ttk.Treeview(mid, columns=cols, show="headings",
                                     selectmode="browse", height=14)
            for c, h, w in zip(cols, heads, widths):
                self.tree.heading(c, text=h)
                self.tree.column(c, width=w, anchor="w",
                                 stretch=(c in ("name", "details")))
            self.tree.tag_configure("empty", foreground="grey")
            self.tree.tag_configure("bad", foreground="red")
            self.tree.tag_configure("pagestart", background="#eef2f7")
            sb = ttk.Scrollbar(mid, orient="vertical",
                               command=self.tree.yview)
            self.tree.configure(yscrollcommand=sb.set)
            self.tree.pack(side="left", fill="both", expand=True)
            sb.pack(side="left", fill="y")
            self.tree.bind("<<TreeviewSelect>>", self.on_select)

        def _build_form(self):
            form = ttk.LabelFrame(self.root, text="Memory", padding=6)
            form.pack(fill="x", padx=6, pady=6)
            self.form_title = tk.StringVar()
            ttk.Label(form, textvariable=self.form_title,
                      font=("TkDefaultFont", 10, "bold")).grid(
                          row=0, column=0, columnspan=8, sticky="w")

            self.v_name = tk.StringVar()
            self.v_mode = tk.StringVar()
            self.v_rx = tk.StringVar()
            self.v_tx = tk.StringVar()
            self.v_power = tk.StringVar()
            self.v_bw = tk.StringVar()
            self.v_rxonly = tk.BooleanVar()
            self.v_a_rx = tk.StringVar()
            self.v_a_tx = tk.StringVar()
            self.v_ts = tk.StringVar()

            def field(row, col, text, widget):
                ttk.Label(form, text=text).grid(row=row, column=col,
                                                sticky="e", padx=(8, 4),
                                                pady=2)
                widget.grid(row=row, column=col + 1, sticky="w", pady=2)
                return widget

            field(1, 0, "Name", ttk.Entry(form, textvariable=self.v_name,
                                          width=22))
            ttk.Button(form, text="Auto", width=5,
                       command=self.auto_name).grid(row=1, column=2,
                                                    sticky="w")
            mode = field(1, 3, "Mode", ttk.Combobox(
                form, textvariable=self.v_mode, values=MODES[1:], width=8,
                state="readonly"))
            mode.bind("<<ComboboxSelected>>", lambda e: self.mode_fields())
            field(1, 5, "Power", ttk.Combobox(
                form, textvariable=self.v_power,
                values=list(POWERS.values()), width=8))
            field(2, 0, "RX MHz", ttk.Entry(form, textvariable=self.v_rx,
                                            width=14))
            field(2, 3, "TX MHz", ttk.Entry(form, textvariable=self.v_tx,
                                            width=14))
            field(2, 5, "Width", ttk.Combobox(
                form, textvariable=self.v_bw, values=BANDWIDTHS, width=8,
                state="readonly"))
            ttk.Checkbutton(form, text="RX only",
                            variable=self.v_rxonly).grid(row=2, column=7,
                                                         sticky="w",
                                                         padx=8)

            # The mode's own settings: tones, color codes or CANs
            self.l_a_rx = ttk.Label(form)
            self.l_a_rx.grid(row=3, column=0, sticky="e", padx=(8, 4))
            self.c_a_rx = ttk.Combobox(form, textvariable=self.v_a_rx,
                                       width=12, state="readonly")
            self.c_a_rx.grid(row=3, column=1, sticky="w")
            self.l_a_tx = ttk.Label(form)
            self.l_a_tx.grid(row=3, column=3, sticky="e", padx=(8, 4))
            self.c_a_tx = ttk.Combobox(form, textvariable=self.v_a_tx,
                                       width=8, state="readonly")
            self.c_a_tx.grid(row=3, column=4, sticky="w")
            self.l_ts = ttk.Label(form, text="Timeslot")
            self.l_ts.grid(row=3, column=5, sticky="e", padx=(8, 4))
            self.c_ts = ttk.Combobox(form, textvariable=self.v_ts,
                                     values=["1", "2"], width=8,
                                     state="readonly")
            self.c_ts.grid(row=3, column=6, sticky="w")

            buttons = ttk.Frame(form)
            buttons.grid(row=4, column=0, columnspan=8, sticky="w",
                         pady=(8, 0))
            for text, cmd in (("Apply", self.apply),
                              ("New here", self.new_here),
                              ("Delete", self.delete),
                              ("Copy", self.copy),
                              ("Cut", self.cut),
                              ("Paste", self.paste)):
                ttk.Button(buttons, text=text, command=cmd).pack(
                    side="left", padx=(0, 4))
            self.hint = ttk.Label(buttons, foreground="#a05000")
            self.hint.pack(side="left", padx=8)

        def _build_log(self):
            self.status = tk.StringVar(value=BURN_MODE_HINT)
            ttk.Label(self.root, textvariable=self.status, padding=(6, 0),
                      wraplength=880).pack(fill="x")
            self.log = tk.Text(self.root, height=6, state="disabled",
                               font=("TkFixedFont", 9))
            self.log.pack(fill="both", padx=6, pady=6)

        # Table ---------------------------------------------------------

        def refresh(self):
            keep = self.selected
            self.tree.delete(*self.tree.get_children())
            page = self.page.get()
            if page == "All":
                slots = range(SLOTS)
            else:
                first = int(page) * PAGE_ROWS
                slots = range(first, min(first + PAGE_ROWS, SLOTS))
            for i in slots:
                mem = self.memories[i]
                tags = []
                if i % PAGE_ROWS == 0 and page == "All":
                    tags.append("pagestart")
                if mem is None:
                    values = ("M%03u" % (i + 1), i // PAGE_ROWS, "-", "", "",
                              "", "", "", "")
                    tags.append("empty")
                else:
                    if mem.problems():
                        tags.append("bad")
                    values = ("M%03u" % (i + 1), i // PAGE_ROWS,
                              mem.shown_name(), mem.mode_name(),
                              "%.5f" % (mem.rx / 1e6),
                              "%.5f" % (mem.tx / 1e6), mem.power_label(),
                              BANDWIDTHS[mem.bandwidth]
                              if mem.bandwidth < 2 else "?",
                              mem.details())
                self.tree.insert("", "end", iid=str(i), values=values,
                                 tags=tags)
            used = sum(m is not None for m in self.memories)
            self.count.configure(text="%u memories%s" % (
                used, ", not saved" if self.dirty else ""))
            if self.tree.exists(str(keep)):
                self.tree.selection_set(str(keep))
                self.tree.see(str(keep))

        def go_page(self, p):
            if p >= PAGES:
                return
            self.page.set(str(p))
            self.selected = p * PAGE_ROWS
            self.refresh()
            self.select(self.selected)

        def on_select(self, _event):
            sel = self.tree.selection()
            if sel:
                self.select(int(sel[0]))

        def select(self, i):
            self.selected = i
            mem = self.memories[i]
            self.form_title.set("M%03u, page %u%s" % (
                i + 1, i // PAGE_ROWS, "  (empty: Apply saves a new one)"
                if mem is None else ""))
            self.load_form(mem if mem is not None else Memory())
            if mem is None:
                self.v_name.set("")

        # Form ----------------------------------------------------------

        def load_form(self, mem):
            self.v_name.set(mem.name)
            self.v_mode.set(mem.mode_name())
            self.v_rx.set("%.5f" % (mem.rx / 1e6))
            self.v_tx.set("%.5f" % (mem.tx / 1e6))
            self.v_power.set(mem.power_label())
            self.v_bw.set(BANDWIDTHS[mem.bandwidth]
                          if mem.bandwidth < 2 else BANDWIDTHS[1])
            self.v_rxonly.set(mem.rx_only)
            self.mode_fields(mem)
            self.hint.configure(text="; ".join(mem.problems()))

        def mode_fields(self, mem=None):
            """The mode's own settings, from mem or left as they are."""
            mode = (MODES.index(self.v_mode.get())
                    if self.v_mode.get() in MODES else MODE_FM)
            tones = ["Off"] + [ctcss_label(t) for t in range(len(CTCSS))]
            nibbles = [str(n) for n in range(16)]
            show = mode in (MODE_FM, MODE_DMR, MODE_M17)
            for w in (self.l_a_rx, self.c_a_rx, self.l_a_tx, self.c_a_tx):
                if show:
                    w.grid()
                else:
                    w.grid_remove()
            if mode == MODE_DMR:
                self.l_ts.grid()
                self.c_ts.grid()
            else:
                self.l_ts.grid_remove()
                self.c_ts.grid_remove()

            if mode == MODE_FM:
                self.l_a_rx.configure(text="RX CTCSS")
                self.l_a_tx.configure(text="TX CTCSS")
                self.c_a_rx.configure(values=tones)
                self.c_a_tx.configure(values=tones)
                if mem is not None and mem.mode == MODE_FM:
                    self.v_a_rx.set("Off" if mem.rx_tone is None
                                    else ctcss_label(mem.rx_tone))
                    self.v_a_tx.set("Off" if mem.tx_tone is None
                                    else ctcss_label(mem.tx_tone))
                elif self.v_a_rx.get() not in tones:
                    self.v_a_rx.set("Off")
                    self.v_a_tx.set("Off")
            elif mode in (MODE_DMR, MODE_M17):
                what = "color code" if mode == MODE_DMR else "CAN"
                self.l_a_rx.configure(text="RX " + what)
                self.l_a_tx.configure(text="TX " + what)
                self.c_a_rx.configure(values=nibbles)
                self.c_a_tx.configure(values=nibbles)
                if mem is not None and mem.mode == mode:
                    rx, tx = mem.dmr_cc if mode == MODE_DMR else mem.m17_can
                    self.v_a_rx.set(str(rx))
                    self.v_a_tx.set(str(tx))
                    self.v_ts.set(str(mem.dmr_timeslot or 1))
                else:
                    default = "1" if mode == MODE_DMR else "0"
                    self.v_a_rx.set(default)
                    self.v_a_tx.set(default)
                    self.v_ts.set("1")

        def form_memory(self):
            """The memory picked with the form's values, or None and why."""
            base = self.memories[self.selected]
            mem = base.copy() if base is not None else Memory()
            try:
                rx = round(float(self.v_rx.get().replace(",", ".")) * 1e6)
                tx = round(float(self.v_tx.get().replace(",", ".")) * 1e6)
            except ValueError:
                return None, "The frequencies are numbers in MHz"
            name = self.v_name.get()
            try:
                name.encode("ascii")
            except UnicodeEncodeError:
                return None, "The radio shows plain ASCII names only"
            # A mode this tool does not know is kept as it is
            mode = (MODES.index(self.v_mode.get())
                    if self.v_mode.get() in MODES else mem.mode)
            mem.mode = mode
            mem.rx = rx
            mem.tx = tx
            mem.bandwidth = BANDWIDTHS.index(self.v_bw.get())
            mem.rx_only = self.v_rxonly.get()
            power = self.v_power.get().strip()
            powers = {v: k for k, v in POWERS.items()}
            if power in powers:
                mem.power = powers[power]
            elif power != mem.power_label():
                try:
                    mem.power = int(power.lower().replace("mw", "").strip())
                except ValueError:
                    return None, "Power: 1 W, 2.5 W, 5 W or a number of mW"
            if mode == MODE_FM:
                def tone(text):
                    if text == "Off":
                        return None
                    return [ctcss_label(t) for t in
                            range(len(CTCSS))].index(text)
                mem.rx_tone = tone(self.v_a_rx.get())
                mem.tx_tone = tone(self.v_a_tx.get())
            elif mode == MODE_DMR:
                mem.dmr_cc = (int(self.v_a_rx.get()), int(self.v_a_tx.get()))
                mem.dmr_timeslot = int(self.v_ts.get())
            elif mode == MODE_M17:
                mem.m17_can = (int(self.v_a_rx.get()),
                               int(self.v_a_tx.get()))
            mem.name = name if name.strip() else mem.auto_name()
            problems = mem.problems()
            if problems:
                return None, "; ".join(problems)
            return mem, ""

        def auto_name(self):
            mem, why = self.form_memory()
            if mem is None:
                self.hint.configure(text=why)
                return
            self.v_name.set(mem.auto_name())

        def set_memory(self, i, mem):
            self.memories[i] = mem
            self.dirty = True
            self.refresh()
            self.select(i)

        def apply(self):
            mem, why = self.form_memory()
            if mem is None:
                self.hint.configure(text=why)
                return
            hint = ""
            if len(mem.shown_name()) > NAME_SHOWN:
                hint = "The radio shows the first %u characters" % NAME_SHOWN
            self.set_memory(self.selected, mem)
            self.hint.configure(text=hint)

        def new_here(self):
            if self.memories[self.selected] is not None:
                if not messagebox.askyesno(
                        "C62 memories", "Replace M%03u with a new memory?"
                        % (self.selected + 1)):
                    return
            mem = Memory()
            mem.name = mem.auto_name()
            self.set_memory(self.selected, mem)

        def delete(self):
            if self.memories[self.selected] is not None:
                self.set_memory(self.selected, None)

        def copy(self):
            mem = self.memories[self.selected]
            if mem is not None:
                self.clipboard = mem.copy()
                self.hint.configure(text="M%03u copied: pick a place and "
                                    "Paste" % (self.selected + 1))

        def cut(self):
            self.copy()
            self.delete()

        def paste(self):
            if self.clipboard is None:
                return
            if self.memories[self.selected] is not None:
                if not messagebox.askyesno(
                        "C62 memories", "Overwrite M%03u?"
                        % (self.selected + 1)):
                    return
            self.set_memory(self.selected, self.clipboard.copy())

        # Files ---------------------------------------------------------

        def load_table(self, data, path=None):
            try:
                memories = parse_table(data)
            except TableError as e:
                messagebox.showerror("C62 memories", str(e))
                return False
            self.memories = memories
            self.path = path
            self.dirty = False
            self.refresh()
            self.select(self.selected)
            return True

        def open_file(self, path=None):
            if not self.discard_ok():
                return
            if path is None:
                path = filedialog.askopenfilename(
                    title="Open a memory table",
                    filetypes=[("Memory table", "*.bin"), ("All", "*")])
            if not path:
                return
            with open(path, "rb") as f:
                if self.load_table(f.read(), path):
                    self.say("Opened %s" % path)

        def save_file(self):
            path = filedialog.asksaveasfilename(
                title="Save the memory table", defaultextension=".bin",
                initialfile=os.path.basename(self.path or "memories.bin"),
                filetypes=[("Memory table", "*.bin")])
            if not path:
                return
            with open(path, "wb") as f:
                f.write(build_table(self.memories))
            self.path = path
            self.dirty = False
            self.refresh()
            self.say("Saved %s" % path)

        def discard_ok(self):
            return (not self.dirty) or messagebox.askyesno(
                "C62 memories", "The changes are not saved. Discard them?")

        def quit(self):
            if self.busy:
                return
            if self.discard_ok():
                self.root.destroy()

        # Radio ---------------------------------------------------------

        def pick_cskburn(self):
            path = filedialog.askopenfilename(title="cskburn")
            if path:
                self.cskburn.set(path)

        def tool(self):
            path = os.path.expanduser(self.cskburn.get().strip())
            if not path or not os.access(path, os.X_OK):
                messagebox.showerror(
                    "C62 memories", "Pick the cskburn program: "
                    "https://github.com/LISTENAI/cskburn/releases")
                return None
            save_config({"port": self.port.get(), "cskburn": path})
            return path

        def say(self, text):
            self.status.set(text)

        def append_log(self, text):
            self.log.configure(state="normal")
            self.log.insert("end", text)
            self.log.see("end")
            self.log.configure(state="disabled")

        def run(self, cmd, done):
            """Run cskburn in the background, its output in the log."""
            self.busy = True
            self.btn_read.state(["disabled"])
            self.btn_write.state(["disabled"])
            self.append_log("$ %s\n" % " ".join(cmd))

            def work():
                try:
                    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT,
                                            text=True, errors="replace")
                    for line in proc.stdout:
                        self.root.after(0, self.append_log, line)
                    code = proc.wait()
                except OSError as e:
                    self.root.after(0, self.append_log, "%s\n" % e)
                    code = -1
                self.root.after(0, finish, code)

            def finish(code):
                self.busy = False
                self.btn_read.state(["!disabled"])
                self.btn_write.state(["!disabled"])
                done(code)

            threading.Thread(target=work, daemon=True).start()

        def read_radio(self):
            tool = self.tool()
            if tool is None or not self.discard_ok():
                return
            if not messagebox.askokcancel("Read radio", BURN_MODE_HINT):
                return
            tmp = tempfile.mkdtemp(prefix="c62mem")
            path = os.path.join(tmp, "memories.bin")
            self.say("Reading the memories…")

            def done(code):
                if code != 0 or not os.path.exists(path):
                    self.say("Reading failed: is the radio in burn mode? "
                             + BURN_MODE_HINT)
                    return
                with open(path, "rb") as f:
                    data = f.read()
                os.makedirs(backup_dir(), exist_ok=True)
                stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
                backup = os.path.join(backup_dir(),
                                      "memories-%s.bin" % stamp)
                with open(backup, "wb") as f:
                    f.write(data)
                shutil.rmtree(tmp, ignore_errors=True)
                if self.load_table(data):
                    self.say("Read %u memories; a copy is in %s"
                             % (sum(m is not None for m in self.memories),
                                backup))

            self.run(cskburn_read_cmd(tool, self.port.get(), path), done)

        def write_radio(self):
            tool = self.tool()
            if tool is None:
                return
            used = sum(m is not None for m in self.memories)
            if not messagebox.askokcancel(
                    "Write radio",
                    "Write these %u memories to the radio, replacing the "
                    "ones it holds? Only the memory table (0x%x, 16 kB) is "
                    "written.\n\n%s" % (used, TABLE_ADDR, BURN_MODE_HINT)):
                return
            tmp = tempfile.mkdtemp(prefix="c62mem")
            path = os.path.join(tmp, "memories.bin")
            with open(path, "wb") as f:
                f.write(build_table(self.memories))
            self.say("Writing the memories…")

            def done(code):
                shutil.rmtree(tmp, ignore_errors=True)
                if code != 0:
                    self.say("Writing failed: is the radio in burn mode? "
                             + BURN_MODE_HINT)
                    return
                self.dirty = False
                self.refresh()
                self.say("Written and verified: power the radio off and "
                         "on to use them")

            self.run(cskburn_write_cmd(tool, self.port.get(), path), done)

    root = tk.Tk()
    editor = Editor(root)
    if path:
        editor.open_file(path)
    root.mainloop()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("file", nargs="?", help="a memory table to open")
    ap.add_argument("--list", action="store_true",
                    help="print the memories of FILE, no window")
    args = ap.parse_args()
    if args.list:
        if not args.file:
            ap.error("--list needs a FILE")
        try:
            list_table(args.file)
        except TableError as e:
            sys.exit(str(e))
        return
    run_gui(args.file)


if __name__ == "__main__":
    main()
