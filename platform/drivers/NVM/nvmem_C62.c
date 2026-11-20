/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "interfaces/nvmem.h"
#include "interfaces/platform.h"
#include "core/nvmem_access.h"
#include "core/nvmem_device.h"
#include "core/utils.h"
#include "core/crc.h"
#include "rtx/rtx.h"
#include "drivers/NVM/flash_zephyr.h"
#include "drivers/NVM/eeep.h"

/*
 * Settings and VFO of the Retevis C62, kept across power cycles in an
 * emulated EEPROM at the start of the OpenRTX storage partition of the
 * internal flash (see c62.dts): the factory calibration and channel areas
 * are never touched.
 *
 * The C62 MCU stays powered when the knob is turned off: OpenRTX sees the
 * battery reading drop, saves here and resets.
 */
#define EEEP_SIZE 16384 /* 4 pages of 4 kB */

/*
 * Record addresses carry the size of the structure stored: a firmware with
 * a different settings_t or channel_t ignores the old records and starts
 * from the defaults, instead of reading them wrongly.
 */
#define VFO_ADDR (0x1000 + sizeof(channel_t))
#define SETTINGS_ADDR (0x2000 + sizeof(settings_t))
#ifdef CONFIG_DMR_TX
#define DMR_ADDR (0x3000 + sizeof(dmrSettings_t))
#endif
#ifdef CONFIG_TETRA_AFC
#define AFC_ADDR (0x4000 + sizeof(int16_t))
#endif

/*
 * The flash takes writes in whole blocks of WRITE_BLOCK bytes: records are
 * padded to them (eeep's record header is one block).
 */
#define WRITE_BLOCK DT_PROP(DT_CHOSEN(zephyr_flash), write_block_size)
#define PADDED(x) ((((x) + WRITE_BLOCK - 1) / WRITE_BLOCK) * WRITE_BLOCK)
#define VFO_SIZE PADDED(sizeof(channel_t))
#define SETTINGS_SIZE PADDED(sizeof(settings_t))

/* EEPROM emulation limits: records under 255 bytes */
BUILD_ASSERT(VFO_SIZE < 255, "channel_t too large for eeep");
BUILD_ASSERT(SETTINGS_SIZE < 255, "settings_t too large for eeep");
#ifdef CONFIG_DMR_TX
#define DMR_SIZE PADDED(sizeof(dmrSettings_t))
BUILD_ASSERT(DMR_SIZE < 255, "dmrSettings_t too large for eeep");

union dmrRecord {
    dmrSettings_t settings;
    uint8_t raw[DMR_SIZE];
};
#endif

union vfoRecord {
    channel_t channel;
    uint8_t raw[VFO_SIZE];
};

union settingsRecord {
    settings_t settings;
    uint8_t raw[SETTINGS_SIZE];
};

/* The flash controller: the "flash" alias names the memory node */
static struct nvmInfo flashInfo;
static const struct nvmDevice eflash = {
    .priv = DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller)),
    .ops = &zephyr_flash_ops,
    .info = &flashInfo,
};

EEEP_DEVICE_DEFINE(eeep)

static const struct nvmPartition memPartitions[] = {
    {
        .offset = FIXED_PARTITION_OFFSET(storage_partition),
        .size = EEEP_SIZE,
    },
};

BUILD_ASSERT(EEEP_SIZE <= FIXED_PARTITION_SIZE(storage_partition),
             "EEPROM emulation larger than the storage partition");

static const struct nvmDescriptor areas[] = {
    {
        .name = "Internal flash",
        .dev = &eflash,
        .baseAddr = 0x00000000,
        .size = DT_REG_SIZE(DT_CHOSEN(zephyr_flash)),
        .nbPart = ARRAY_SIZE(memPartitions),
        .partitions = memPartitions,
    },
    {
        .name = "Virtual EEPROM",
        .dev = &eeep,
        .baseAddr = 0x00000000,
        .size = 0xFFFFFFFF, /* No physical size */
        .nbPart = 0,
        .partitions = NULL,
    },
};

const struct nvmTable nvmTab = {
    .areas = areas,
    .nbAreas = ARRAY_SIZE(areas),
};

/* CRC of what is stored, to write only what changed */
static uint16_t settingsCrc;
static uint16_t vfoCrc;
#ifdef CONFIG_DMR_TX
static uint16_t dmrCrc;
#endif
#ifdef CONFIG_TETRA_AFC
static int16_t afcStored;
static bool afcKnown;
#endif
static bool ready;

void nvm_init()
{
    int ret = zephirFlash_init(&eflash);
    if (ret == 0)
        ret = eeep_init(&eeep, 0, 0);

    /*
     * Records of a size the flash cannot take leave the next free address
     * off the write blocks, and every later write fails: start afresh.
     */
    if (ret == 0 && (eeepData_eeep.writeAddr % WRITE_BLOCK) != 0) {
        printk("NVM: storage misaligned, erasing it\n");
        ret = nvm_devErase(&eflash, memPartitions[0].offset, EEEP_SIZE);
        if (ret == 0)
            ret = eeep_init(&eeep, 0, 0);
    }
    ready = (ret == 0);
    printk("NVM: init %s (%d)\n", ready ? "ok" : "failed", ret);
}

void nvm_terminate()
{
    if (ready)
        eeep_terminate(&eeep);
    ready = false;
}

void nvm_readCalibData(void *buf)
{
    (void)buf;
}

void nvm_readHwInfo(hwInfo_t *info)
{
    (void)info;
}

/**
 * Check that a stored frequency can be tuned.
 */
static bool validFreq(freq_t freq)
{
    const hwInfo_t *hw = platform_getHwInfo();
    freq_t lo = (hw->vhf_band ? hw->vhf_minFreq : hw->uhf_minFreq) * 1000000;
    freq_t hi = (hw->uhf_band ? hw->uhf_maxFreq : hw->vhf_maxFreq) * 1000000;
    return (freq >= lo) && (freq <= hi);
}

int nvm_readVfoChannelData(channel_t *channel)
{
    if (!ready)
        return -1;

    union vfoRecord rec = { 0 };
    int ret = nvm_read(1, 0, VFO_ADDR, rec.raw, sizeof(rec.raw));
    const channel_t vfo = rec.channel;
    if (ret < 0) {
        printk("NVM: no VFO stored (%d)\n", ret);
        return -1;
    }

    if (!validFreq(vfo.rx_frequency) || !validFreq(vfo.tx_frequency)
        || vfo.mode == OPMODE_NONE) {
        printk("NVM: VFO stored not valid: %lu/%lu Hz, mode %u\n",
               (unsigned long)vfo.rx_frequency, (unsigned long)vfo.tx_frequency,
               vfo.mode);
        return -1;
    }

    printk("NVM: VFO %lu Hz, mode %u\n", (unsigned long)vfo.rx_frequency,
           vfo.mode);

    *channel = vfo;
    vfoCrc = crc_ccitt(channel, sizeof(channel_t));
    return 0;
}

int nvm_readSettings(settings_t *settings)
{
    if (!ready)
        return -1;

    union settingsRecord rec = { 0 };
    int ret = nvm_read(1, 0, SETTINGS_ADDR, rec.raw, sizeof(rec.raw));
    const settings_t stored = rec.settings;
    if (ret < 0) {
        printk("NVM: no settings stored (%d)\n", ret);
        return -1;
    }

    *settings = stored;
    settingsCrc = crc_ccitt(settings, sizeof(settings_t));
    return 0;
}

int nvm_writeSettings(const settings_t *settings)
{
    if (!ready)
        return -1;

    uint16_t crc = crc_ccitt(settings, sizeof(settings_t));
    if (crc == settingsCrc)
        return 0;

    union settingsRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.settings = *settings;
    int ret = nvm_write(1, 0, SETTINGS_ADDR, rec.raw, sizeof(rec.raw));
    if (ret == 0)
        settingsCrc = crc;
    return ret;
}

int nvm_writeSettingsAndVfo(const settings_t *settings, const channel_t *vfo)
{
    if (!ready) {
        printk("NVM: not saved, no storage\n");
        return -1;
    }

    int ret = 0;
    uint16_t crc = crc_ccitt(vfo, sizeof(channel_t));
    if (crc != vfoCrc) {
        union vfoRecord rec;
        memset(&rec, 0, sizeof(rec));
        rec.channel = *vfo;
        ret = nvm_write(1, 0, VFO_ADDR, rec.raw, sizeof(rec.raw));
        if (ret == 0)
            vfoCrc = crc;
    }
    int sret = nvm_writeSettings(settings);

    printk("NVM: saved VFO %lu Hz mode %u (%d), settings (%d)\n",
           (unsigned long)vfo->rx_frequency, vfo->mode, ret, sret);
    return (ret < 0) ? ret : sret;
}

#ifdef CONFIG_DMR_TX
int nvm_readDmrSettings(dmrSettings_t *settings)
{
    if (!ready)
        return -1;

    union dmrRecord rec = { 0 };
    int ret = nvm_read(1, 0, DMR_ADDR, rec.raw, sizeof(rec.raw));
    const dmrSettings_t stored = rec.settings;
    if (ret < 0) {
        printk("NVM: no DMR settings stored (%d)\n", ret);
        return -1;
    }
    if ((stored.id > DMR_ID_MAX) || (stored.tg > DMR_ID_MAX)
        || (stored.cc > 15) || (stored.slot < 1) || (stored.slot > 2)) {
        printk("NVM: DMR settings not valid\n");
        return -1;
    }

    *settings = stored;
    dmrCrc = crc_ccitt(settings, sizeof(dmrSettings_t));
    return 0;
}

int nvm_writeDmrSettings(const dmrSettings_t *settings)
{
    if (!ready)
        return -1;

    uint16_t crc = crc_ccitt(settings, sizeof(dmrSettings_t));
    if (crc == dmrCrc)
        return 0;

    union dmrRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.settings = *settings;
    int ret = nvm_write(1, 0, DMR_ADDR, rec.raw, sizeof(rec.raw));
    if (ret == 0)
        dmrCrc = crc;
    printk("NVM: saved DMR settings (%d)\n", ret);
    return ret;
}
#endif

#ifdef CONFIG_TETRA_AFC
union afcRecord {
    int16_t hz;
    uint8_t raw[PADDED(sizeof(int16_t))];
};

int nvm_readTetraAfc(int16_t *hz)
{
    if (!ready)
        return -1;

    union afcRecord rec = { 0 };
    int ret = nvm_read(1, 0, AFC_ADDR, rec.raw, sizeof(rec.raw));
    if (ret < 0)
        return -1;
    *hz = rec.hz;
    afcStored = rec.hz;
    afcKnown = true;
    return 0;
}

int nvm_writeTetraAfc(int16_t hz)
{
    if (!ready)
        return -1;
    if (afcKnown && hz == afcStored)
        return 0;

    union afcRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.hz = hz;
    int ret = nvm_write(1, 0, AFC_ADDR, rec.raw, sizeof(rec.raw));
    if (ret == 0) {
        afcStored = hz;
        afcKnown = true;
    }
    printk("NVM: saved TETRA frequency correction %d Hz (%d)\n", hz, ret);
    return ret;
}
#endif
