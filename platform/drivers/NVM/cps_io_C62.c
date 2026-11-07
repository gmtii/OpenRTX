/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <string.h>
#include "interfaces/cps_io.h"
#include "core/crc.h"

/*
 * Channel memories of the Retevis C62, saved from the radio itself, in the
 * OpenRTX storage partition of the internal flash, after the settings
 * (nvmem_C62.c): the factory calibration and channel areas are never
 * touched. No contacts and no banks.
 *
 * The channels are kept contiguous, 0 to count - 1, as the UI lists them
 * until the first that cannot be read: deleting one moves the next ones
 * down. Each slot holds a magic, a CRC and the channel_t; the table is
 * rewritten in RAM and the changed 4 kB sectors erased and written again.
 */
#define CHANNELS_OFFSET 0x4000 /* After the settings' 16 kB */
#define SLOT_SIZE 128
#define SECTOR_SIZE 4096
#define MAX_CHANNELS 128
#define TABLE_SIZE (MAX_CHANNELS * SLOT_SIZE)
#define SLOT_MAGIC 0xC62C4A11

struct slot {
    uint32_t magic;
    uint16_t crc;  /* Of the channel */
    uint16_t size; /* sizeof(channel_t) when written */
    channel_t channel;
} __attribute__((packed));

BUILD_ASSERT(sizeof(struct slot) <= SLOT_SIZE, "channel_t too large");
BUILD_ASSERT(CHANNELS_OFFSET + TABLE_SIZE
                 <= FIXED_PARTITION_SIZE(storage_partition),
             "channel table larger than the storage partition");

static const struct device *const flash =
    DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

/* The whole table while it is being changed: PSRAM, not zeroed */
static __attribute__((section(".psram_section"))) uint8_t table[TABLE_SIZE];

static uint32_t tableAddress(void)
{
    return FIXED_PARTITION_OFFSET(storage_partition) + CHANNELS_OFFSET;
}

/**
 * Read a slot and check it.
 */
static bool readSlot(uint16_t pos, channel_t *channel)
{
    struct slot s;
    if (pos >= MAX_CHANNELS)
        return false;
    if (flash_read(flash, tableAddress() + pos * SLOT_SIZE, &s, sizeof(s)) < 0)
        return false;
    if (s.magic != SLOT_MAGIC || s.size != sizeof(channel_t)
        || s.crc != crc_ccitt(&s.channel, sizeof(channel_t)))
        return false;
    if (channel != NULL)
        memcpy(channel, &s.channel, sizeof(channel_t));
    return true;
}

/**
 * Number of channels: the slots valid from the first one.
 */
static uint16_t countChannels(void)
{
    uint16_t n = 0;
    while (n < MAX_CHANNELS && readSlot(n, NULL))
        n++;
    return n;
}

static void putSlot(uint16_t pos, const channel_t *channel)
{
    struct slot *s = (struct slot *)&table[pos * SLOT_SIZE];
    memset(s, 0xff, SLOT_SIZE);
    s->magic = SLOT_MAGIC;
    s->size = sizeof(channel_t);
    memcpy(&s->channel, channel, sizeof(channel_t));
    s->crc = crc_ccitt(&s->channel, sizeof(channel_t));
}

static int loadTable(void)
{
    return flash_read(flash, tableAddress(), table, TABLE_SIZE);
}

/**
 * Write back the sectors holding slots first to last.
 */
static int storeTable(uint16_t first, uint16_t last)
{
    uint32_t from = (first * SLOT_SIZE) / SECTOR_SIZE;
    uint32_t to = (last * SLOT_SIZE) / SECTOR_SIZE;
    for (uint32_t sec = from; sec <= to; sec++) {
        uint32_t addr = tableAddress() + sec * SECTOR_SIZE;
        int ret = flash_erase(flash, addr, SECTOR_SIZE);
        if (ret == 0)
            ret = flash_write(flash, addr, &table[sec * SECTOR_SIZE],
                              SECTOR_SIZE);
        if (ret < 0) {
            printk("CPS: flash write failed (%d)\n", ret);
            return ret;
        }
    }
    return 0;
}

int cps_open(char *cps_name)
{
    (void)cps_name;
    return 0;
}

void cps_close()
{
}

int cps_create(char *cps_name)
{
    (void)cps_name;
    return 0;
}

int cps_readContact(contact_t *contact, uint16_t pos)
{
    (void)contact;
    (void)pos;
    return -1;
}

int cps_readChannel(channel_t *channel, uint16_t pos)
{
    return readSlot(pos, channel) ? 0 : -1;
}

int cps_readBankHeader(bankHdr_t *b_header, uint16_t pos)
{
    (void)b_header;
    (void)pos;
    return -1;
}

int cps_readBankData(uint16_t bank_pos, uint16_t pos)
{
    (void)bank_pos;
    (void)pos;
    return -1;
}

int cps_writeContact(contact_t contact, uint16_t pos)
{
    (void)contact;
    (void)pos;
    return -1;
}

int cps_writeChannel(channel_t channel, uint16_t pos)
{
    uint16_t count = countChannels();
    if (pos > count || pos >= MAX_CHANNELS)
        return -1; /* Overwrite one, or add one at the end */
    if (loadTable() < 0)
        return -1;
    putSlot(pos, &channel);
    return storeTable(pos, pos);
}

int cps_writeBankHeader(bankHdr_t b_header, uint16_t pos)
{
    (void)b_header;
    (void)pos;
    return -1;
}

int cps_writeBankData(uint32_t ch, uint16_t bank_pos, uint16_t pos)
{
    (void)ch;
    (void)bank_pos;
    (void)pos;
    return -1;
}

int cps_insertContact(contact_t contact, uint16_t pos)
{
    (void)contact;
    (void)pos;
    return -1;
}

int cps_insertChannel(channel_t channel, uint16_t pos)
{
    uint16_t count = countChannels();
    if (pos > count || count >= MAX_CHANNELS)
        return -1;
    if (loadTable() < 0)
        return -1;
    memmove(&table[(pos + 1) * SLOT_SIZE], &table[pos * SLOT_SIZE],
            (count - pos) * SLOT_SIZE);
    putSlot(pos, &channel);
    return storeTable(pos, count);
}

int cps_insertBankHeader(bankHdr_t b_header, uint16_t pos)
{
    (void)b_header;
    (void)pos;
    return -1;
}

int cps_insertBankData(uint32_t ch, uint16_t bank_pos, uint16_t pos)
{
    (void)ch;
    (void)bank_pos;
    (void)pos;
    return -1;
}

int cps_deleteContact(uint16_t pos)
{
    (void)pos;
    return -1;
}

int cps_deleteChannel(channel_t channel, uint16_t pos)
{
    (void)channel;
    uint16_t count = countChannels();
    if (pos >= count)
        return -1;
    if (loadTable() < 0)
        return -1;
    memmove(&table[pos * SLOT_SIZE], &table[(pos + 1) * SLOT_SIZE],
            (count - 1 - pos) * SLOT_SIZE);
    memset(&table[(count - 1) * SLOT_SIZE], 0xff, SLOT_SIZE);
    return storeTable(pos, count - 1);
}

int cps_deleteBankHeader(uint16_t pos)
{
    (void)pos;
    return -1;
}

int cps_deleteBankData(uint16_t bank_pos, uint16_t pos)
{
    (void)bank_pos;
    (void)pos;
    return -1;
}
