/*
 * eosfs - Coleco ADAM EOS filesystem builder for DDP / DSK block images.
 *
 * Copyright (C) 2026 Thomas Cherryhomes <thom.cherryhomes@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Portable C99. No dependencies beyond the C standard library.
 *
 * Understands the EOS medium layout as implemented by EOS 5 (see the
 * disassembly in learn/) and the DDP (contiguous) / DSK (5:1 interleave)
 * block-to-offset mapping used by the FujiNet ADAM media layer
 * (fujinet-firmware/lib/media/adam/mediaTypeDDP.cpp, mediaTypeDSK.cpp).
 *
 * On-medium format
 * ----------------
 *   block 0            : BOOT block (reserved, 1 block)
 *   blocks 1..D        : DIRECTORY (D "directory blocks", default 3)
 *   blocks D+1 ..      : file data, then free space
 *
 * A directory block holds 39 fixed 26-byte entries (39*26 = 1014 <= 1024).
 * Entry layout (little-endian):
 *   [0..11]  name, terminated with 0x03, space padded
 *   [12]     attribute byte
 *   [13..16] start block (4)          (VOLUME entry: check code 55 AA 00 FF)
 *   [17..18] allocated length, blocks (2)
 *   [19..20] used length, blocks      (2)
 *   [21..22] byte count in last block (2)
 *   [23]     year (year-1900)
 *   [24]     month
 *   [25]     day
 *
 * Attribute bits: 0x01 = "not a file" (BLOCKS LEFT end marker),
 *                 0x04 = deleted, 0x10 = user file, 0x80 = locked/system.
 *
 * The first three entries of the directory are always VOLUME, BOOT and
 * DIRECTORY, followed by the file entries, followed by a BLOCKS LEFT entry
 * (attr 0x01) whose start block / allocated length describe the free extent.
 *
 * File size in bytes = (used_length - 1) * 1024 + last_block_byte_count.
 *
 * This tool keeps the whole filesystem in memory as a model (volume params +
 * a list of files with their data) and rewrites the medium compactly on every
 * mutation. Files are always laid out contiguously right after the directory,
 * which is exactly how EOS appends them on a fresh volume; removing a file
 * compacts the rest. The result is always a clean, valid EOS volume.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#define BLOCK        1024u   /* logical EOS block size                     */
#define ENT            26u   /* directory entry size                       */
#define EPB            39u   /* directory entries per 1K block             */
#define DEF_DIRBLK      3u   /* default number of directory blocks         */

/* directory entry field offsets */
#define O_NAME   0
#define O_ATTR  12
#define O_START 13
#define O_ALLOC 17
#define O_USED  19
#define O_LAST  21
#define O_YEAR  23
#define O_MON   24
#define O_DAY   25

/* attribute bits */
#define A_NOTFILE 0x01u
#define A_DELETED 0x04u
#define A_USER    0x10u
#define A_LOCKED  0x80u

/* system-entry attribute values, from the EOS INIT template */
#define ATTR_VOL_BASE 0x80u  /* OR'd with directory-block count */
#define ATTR_BOOT     0x88u
#define ATTR_DIR      0xC8u

typedef enum { MEDIA_DDP, MEDIA_DSK } media_t;

typedef struct {
    uint8_t  name[12];   /* raw 12-byte directory name field */
    uint8_t  attr;
    uint8_t  y, mo, d;
    uint32_t start;      /* transient: assigned during layout */
    uint8_t *data;
    uint32_t size;       /* bytes */
} File;

typedef struct {
    media_t  media;
    uint32_t nblk;       /* total blocks on the medium */
    uint32_t dirblk;     /* number of directory blocks */
    uint8_t  vol[12];    /* raw volume-name field */
    uint8_t  boot[BLOCK];/* preserved boot block (block 0) */
    File    *f;
    size_t   n, cap;
} Fs;

/* ---------------------------------------------------------------- helpers */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("eosfs: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xcalloc(size_t n)
{
    void *p = calloc(n ? n : 1, 1);
    if (!p) die("out of memory");
    return p;
}

static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void p16(uint8_t *p, uint16_t v) { p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; }
static void p32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

/* DSK 5:1 interleave: byte offsets of a block's lower and upper 512 bytes.
 * Mirrors MediaTypeDSK::_block_to_offsets exactly. */
static void dsk_off(uint32_t blk, uint32_t *o1, uint32_t *o2)
{
    uint32_t r = blk % 4;
    *o1 = blk * BLOCK;
    *o2 = (r == 0 || r == 1) ? blk * BLOCK + 5u * 512u
                             : blk * BLOCK - (4096u - 5u * 512u);
}

/* Read/write one logical 1K block into/out of the raw image buffer. */
static void bread(const uint8_t *img, media_t m, uint32_t blk, uint8_t *out)
{
    if (m == MEDIA_DDP) {
        memcpy(out, img + (size_t)blk * BLOCK, BLOCK);
    } else {
        uint32_t o1, o2;
        dsk_off(blk, &o1, &o2);
        memcpy(out, img + o1, 512);
        memcpy(out + 512, img + o2, 512);
    }
}

static void bwrite(uint8_t *img, media_t m, uint32_t blk, const uint8_t *in)
{
    if (m == MEDIA_DDP) {
        memcpy(img + (size_t)blk * BLOCK, in, BLOCK);
    } else {
        uint32_t o1, o2;
        dsk_off(blk, &o1, &o2);
        memcpy(img + o1, in, 512);
        memcpy(img + o2, in + 512, 512);
    }
}

/* Build a 12-byte directory name field from a C string. */
static void mkname(uint8_t out[12], const char *s)
{
    size_t L = strlen(s), i;
    if (L > 11) L = 11;
    for (i = 0; i < L; i++) out[i] = (uint8_t)s[i];
    out[L] = 0x03;
    for (i = L + 1; i < 12; i++) out[i] = ' ';
}

/* Extract the printable name (up to 0x03) from a 12-byte field. */
static void getname(const uint8_t f[12], char out[13])
{
    int i;
    for (i = 0; i < 12; i++) {
        if (f[i] == 0x03) break;
        out[i] = (char)f[i];
    }
    out[i] = 0;
}

/* Blocks / used / last-byte-count derived from a byte size. */
static void sizefields(uint32_t sz, uint16_t *used, uint16_t *last)
{
    if (sz == 0) { *used = 1; *last = 0; return; }
    uint32_t b = (sz + BLOCK - 1) / BLOCK;
    *used = (uint16_t)b;
    *last = (uint16_t)(sz - (b - 1) * BLOCK);
}

static uint32_t file_blocks(uint32_t sz)
{
    return sz == 0 ? 1u : (sz + BLOCK - 1) / BLOCK;
}

/* ------------------------------------------------------------- boot blocks */

/* EOS reads block 0 into 0xC800 and JP's there unconditionally with B = boot
 * device number (see EOS Fn0 cold boot). There is no bootable signature, so
 * block 0 must always be valid code. These helpers synthesise it. */

#define BOOT_ORG   0xC800u   /* COLD_START_ADDR: where EOS maps block 0 */
#define V_OPEN     0xFCC0u   /* Fn48 open   : A=dev,HL=name,B=mode -> A=file# */
#define V_CLOSE    0xFCC3u   /* Fn50 close  : A=file# */
#define V_READ     0xFCD2u   /* Fn54 read   : A=file#,BC=count,HL=buf */
#define V_WP       0xFCE7u   /* Fn61 go to SmartWriter */
#define BOOT_STACK 0xFE58u   /* private boot stack (as used by SmartBASIC) */

/* Default block 0: DI ; JP SmartWriter. Used for non-bootable media so that a
 * power-on with the disk inserted lands gracefully in SmartWriter. */
static void make_boot_smartwriter(uint8_t blk[BLOCK])
{
    memset(blk, 0, BLOCK);
    blk[0] = 0xF3;               /* DI          */
    blk[1] = 0xC3;               /* JP nn       */
    blk[2] = V_WP & 0xFF;
    blk[3] = (V_WP >> 8) & 0xFF; /* -> 0xFCE7   */
}

/* A file is in BLOAD form if it starts with the 5-byte header 01 00 02 LL HH,
 * where LLHH is the little-endian load address. */
static int is_bload(const uint8_t *d, uint32_t n)
{
    return n >= 5 && d[0] == 0x01 && d[1] == 0x00 && d[2] == 0x02;
}

/* Synthesise a block-0 loader that opens the EOS file named in `namefield`
 * on the boot device, loads `length` bytes at `load`, and jumps to `entry`.
 * For BLOAD files the 5-byte header is read and discarded first. Any EOS
 * error falls back to SmartWriter. This is the splash-free equivalent of
 * smartbasic-1.x/boot-block.asm, generalised to any file size. */
static void make_boot_loader(uint8_t blk[BLOCK], const uint8_t namefield[12],
                             uint16_t load, uint16_t entry,
                             uint32_t length, int bload)
{
    int nl = 0, i;
    size_t o;
    uint16_t fname_addr, fnum_addr;
    unsigned bootoff;

    memset(blk, 0, BLOCK);

    while (nl < 12 && namefield[nl] != 0x03) nl++;  /* chars before 0x03 */

    fname_addr = (uint16_t)(BOOT_ORG + 3);
    fnum_addr  = (uint16_t)(BOOT_ORG + 3 + (nl + 1));
    bootoff    = 3u + (unsigned)(nl + 1) + 1u;      /* DI JR FNAME.03 FNUM */

    blk[0] = 0xF3;                          /* DI */
    blk[1] = 0x18;                          /* JR BOOT */
    blk[2] = (uint8_t)(bootoff - 3);
    for (i = 0; i < nl; i++) blk[3 + i] = namefield[i];
    blk[3 + nl] = 0x03;                     /* FNAME terminator */
    /* FNUM byte stays 0 */

    o = bootoff;
    #define EB(b) (blk[o++] = (uint8_t)(b))
    #define EW(w) (blk[o++] = (uint8_t)((w) & 0xff), \
                   blk[o++] = (uint8_t)(((w) >> 8) & 0xff))

    EB(0x31); EW(BOOT_STACK);   /* LD SP,BOOT_STACK           */
    EB(0x78);                   /* LD A,B      ; boot device  */
    EB(0x06); EB(0x01);         /* LD B,1      ; open for read */
    EB(0x21); EW(fname_addr);   /* LD HL,FNAME                */
    EB(0xCD); EW(V_OPEN);       /* CALL open                  */
    EB(0xC2); EW(V_WP);         /* JP NZ,SmartWriter          */
    EB(0x32); EW(fnum_addr);    /* LD (FNUM),A                */

    if (bload) {                /* discard the 5-byte BLOAD header */
        EB(0x21); EW(load);     /* LD HL,load                 */
        EB(0x01); EW(5);        /* LD BC,5                    */
        EB(0x3A); EW(fnum_addr);/* LD A,(FNUM)                */
        EB(0xCD); EW(V_READ);   /* CALL read                  */
        EB(0xC2); EW(V_WP);     /* JP NZ,SmartWriter          */
    }

    EB(0x21); EW(load);         /* LD HL,load                 */
    EB(0x01); EW((uint16_t)length); /* LD BC,length           */
    EB(0x3A); EW(fnum_addr);    /* LD A,(FNUM)                */
    EB(0xCD); EW(V_READ);       /* CALL read (payload)        */
    EB(0xC2); EW(V_WP);         /* JP NZ,SmartWriter          */
    EB(0x3A); EW(fnum_addr);    /* LD A,(FNUM)                */
    EB(0xCD); EW(V_CLOSE);      /* CALL close                 */
    EB(0xC2); EW(V_WP);         /* JP NZ,SmartWriter          */
    EB(0xC3); EW(entry);        /* JP entry                   */

    #undef EB
    #undef EW
}

/* ------------------------------------------------------------ file model */

static File *fs_add_slot(Fs *fs)
{
    if (fs->n == fs->cap) {
        fs->cap = fs->cap ? fs->cap * 2 : 8;
        fs->f = realloc(fs->f, fs->cap * sizeof(File));
        if (!fs->f) die("out of memory");
    }
    memset(&fs->f[fs->n], 0, sizeof(File));
    return &fs->f[fs->n++];
}

static long fs_find(const Fs *fs, const char *name)
{
    char nm[13];
    size_t i;
    for (i = 0; i < fs->n; i++) {
        getname(fs->f[i].name, nm);
        if (strcmp(nm, name) == 0) return (long)i;
    }
    return -1;
}

static void fs_free(Fs *fs)
{
    size_t i;
    for (i = 0; i < fs->n; i++) free(fs->f[i].data);
    free(fs->f);
    free(fs);
}

/* --------------------------------------------------------------- disk I/O */

static uint8_t *read_whole(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    uint8_t *buf;
    if (!fp) die("cannot open '%s'", path);
    if (fseek(fp, 0, SEEK_END) != 0) die("seek failed on '%s'", path);
    sz = ftell(fp);
    if (sz < 0) die("tell failed on '%s'", path);
    rewind(fp);
    buf = xmalloc((size_t)sz);
    if (sz && fread(buf, 1, (size_t)sz, fp) != (size_t)sz)
        die("short read on '%s'", path);
    fclose(fp);
    *len = (size_t)sz;
    return buf;
}

static void write_whole(const char *path, const uint8_t *buf, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) die("cannot create '%s'", path);
    if (fwrite(buf, 1, len, fp) != len) die("write failed on '%s'", path);
    fclose(fp);
}

/* Parse an existing EOS image into a model. */
static Fs *load(const char *path, media_t media)
{
    size_t len;
    uint8_t *img = read_whole(path, &len);
    Fs *fs;
    uint8_t db[BLOCK], cur[BLOCK];
    uint32_t slots, i;
    long have = -1;

    if (len == 0 || len % BLOCK)
        die("'%s' size %lu is not a multiple of %u", path,
            (unsigned long)len, BLOCK);
    if (media == MEDIA_DSK && (len / BLOCK) % 4)
        die("DSK image must be a multiple of 4 blocks (has %lu)",
            (unsigned long)(len / BLOCK));

    fs = xcalloc(sizeof(Fs));
    fs->media = media;
    fs->nblk = (uint32_t)(len / BLOCK);

    bread(img, media, 0, fs->boot);
    bread(img, media, 1, db);

    if (!(db[O_START] == 0x55 && db[O_START + 1] == 0xAA &&
          db[O_START + 2] == 0x00 && db[O_START + 3] == 0xFF))
        die("'%s' is not an EOS volume (bad check code)", path);

    fs->dirblk = db[O_ATTR] & 0x7F;
    if (fs->dirblk < 1 || (size_t)fs->dirblk * BLOCK > len)
        die("bad directory size (%u blocks)", fs->dirblk);
    memcpy(fs->vol, db + O_NAME, 12);

    slots = fs->dirblk * EPB;
    for (i = 1; i < slots; i++) {
        uint32_t dbk = i / EPB, slot = i % EPB;
        uint8_t *e, attr;
        uint32_t start, sz, b;
        uint16_t used, last;
        File *nf;

        if ((long)dbk != have) {
            bread(img, media, 1 + dbk, cur);
            have = (long)dbk;
        }
        e = cur + slot * ENT;
        attr = e[O_ATTR];

        if (attr & A_NOTFILE) break;      /* BLOCKS LEFT -> end of directory */
        if (i == 1 || i == 2) continue;   /* BOOT, DIRECTORY (regenerated)   */
        if (attr & A_DELETED) continue;   /* skip deleted files              */

        start = g32(e + O_START);
        used  = g16(e + O_USED);
        last  = g16(e + O_LAST);
        sz    = used ? (uint32_t)(used - 1) * BLOCK + last : 0;
        if (used && (uint32_t)start + used > fs->nblk)
            die("corrupt entry: file runs past end of medium");

        nf = fs_add_slot(fs);
        memcpy(nf->name, e + O_NAME, 12);
        nf->attr = attr;
        nf->y = e[O_YEAR]; nf->mo = e[O_MON]; nf->d = e[O_DAY];
        nf->size = sz;
        nf->data = xmalloc(sz);
        for (b = 0; b < used; b++) {
            uint8_t tmp[BLOCK];
            uint32_t off = b * BLOCK, cpy;
            bread(img, media, start + b, tmp);
            cpy = (sz - off >= BLOCK) ? BLOCK : (sz - off);
            memcpy(nf->data + off, tmp, cpy);
        }
    }

    free(img);
    return fs;
}

/* Write the model out to a fresh, compact EOS image. */
static void store(Fs *fs, const char *path)
{
    size_t isz = (size_t)fs->nblk * BLOCK;
    uint8_t *img, *dir;
    uint32_t cur, free_start, free_cnt, entries;
    size_t i;
    uint8_t *e;

    /* Assign contiguous block ranges to files. */
    cur = fs->dirblk + 1;
    for (i = 0; i < fs->n; i++) {
        fs->f[i].start = cur;
        cur += file_blocks(fs->f[i].size);
    }
    if (cur > fs->nblk)
        die("out of space: need %u blocks, medium holds %u", cur, fs->nblk);
    free_start = cur;
    free_cnt = fs->nblk - cur;

    entries = 3 + (uint32_t)fs->n + 1;   /* VOLUME BOOT DIRECTORY + files + BLOCKS LEFT */
    if (entries > fs->dirblk * EPB)
        die("directory full: %u entries needed, %u slots in %u blocks",
            entries, fs->dirblk * EPB, fs->dirblk);

    img = xcalloc(isz);
    dir = xcalloc((size_t)fs->dirblk * BLOCK);
    bwrite(img, fs->media, 0, fs->boot);

    #define SLOT(idx) (dir + (size_t)((idx) / EPB) * BLOCK + ((idx) % EPB) * ENT)

    /* VOLUME */
    e = SLOT(0);
    memcpy(e + O_NAME, fs->vol, 12);
    e[O_ATTR] = (uint8_t)(ATTR_VOL_BASE | (fs->dirblk & 0x7F));
    e[O_START] = 0x55; e[O_START + 1] = 0xAA;
    e[O_START + 2] = 0x00; e[O_START + 3] = 0xFF;
    p16(e + O_ALLOC, (uint16_t)fs->nblk);   /* volume length in blocks */

    /* BOOT */
    e = SLOT(1);
    mkname(e + O_NAME, "BOOT");
    e[O_ATTR] = ATTR_BOOT;
    p32(e + O_START, 0);
    p16(e + O_ALLOC, 1);
    p16(e + O_USED, 1);

    /* DIRECTORY */
    e = SLOT(2);
    mkname(e + O_NAME, "DIRECTORY");
    e[O_ATTR] = ATTR_DIR;
    p32(e + O_START, 1);
    p16(e + O_ALLOC, (uint16_t)fs->dirblk);
    p16(e + O_USED, (uint16_t)fs->dirblk);
    p16(e + O_LAST, 1024);

    /* file entries */
    for (i = 0; i < fs->n; i++) {
        File *f = &fs->f[i];
        uint16_t used, last;
        uint8_t attr = f->attr;
        sizefields(f->size, &used, &last);
        if (attr == 0 || (attr & (A_NOTFILE | A_DELETED))) attr = A_USER;
        e = SLOT(3 + i);
        memcpy(e + O_NAME, f->name, 12);
        e[O_ATTR] = attr;
        p32(e + O_START, f->start);
        p16(e + O_ALLOC, used);
        p16(e + O_USED, used);
        p16(e + O_LAST, last);
        e[O_YEAR] = f->y; e[O_MON] = f->mo; e[O_DAY] = f->d;
    }

    /* BLOCKS LEFT sentinel */
    e = SLOT(3 + fs->n);
    mkname(e + O_NAME, "BLOCKS LEFT");
    e[O_ATTR] = A_NOTFILE;
    p32(e + O_START, free_start);
    p16(e + O_ALLOC, (uint16_t)free_cnt);
    e[O_YEAR] = 0x57; e[O_MON] = 0x07; e[O_DAY] = 0x11;  /* EOS template date */

    #undef SLOT

    /* emit directory blocks */
    for (i = 0; i < fs->dirblk; i++)
        bwrite(img, fs->media, 1 + (uint32_t)i, dir + i * BLOCK);

    /* emit file data */
    for (i = 0; i < fs->n; i++) {
        File *f = &fs->f[i];
        uint32_t nb = file_blocks(f->size), b;
        for (b = 0; b < nb; b++) {
            uint8_t tmp[BLOCK];
            uint32_t off = b * BLOCK, cpy;
            memset(tmp, 0, BLOCK);
            cpy = (f->size > off) ? (f->size - off) : 0;
            if (cpy > BLOCK) cpy = BLOCK;
            memcpy(tmp, f->data + off, cpy);
            bwrite(img, fs->media, f->start + b, tmp);
        }
    }

    write_whole(path, img, isz);
    free(dir);
    free(img);
}

/* ------------------------------------------------------------------- CLI */

static const char *media_name(media_t m) { return m == MEDIA_DDP ? "DDP" : "DSK"; }

static const char *basename_of(const char *p)
{
    const char *b = p, *s;
    for (s = p; *s; s++)
        if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}

/* Guess media from filename extension; -1 if unknown. */
static int guess_media(const char *path)
{
    const char *b = basename_of(path);
    const char *dot = strrchr(b, '.');
    if (!dot) return -1;
    if (strcmp(dot, ".ddp") == 0 || strcmp(dot, ".DDP") == 0) return MEDIA_DDP;
    if (strcmp(dot, ".dsk") == 0 || strcmp(dot, ".DSK") == 0) return MEDIA_DSK;
    return -1;
}

static media_t resolve_media(const char *path, const char *type_opt)
{
    if (type_opt) {
        if (strcmp(type_opt, "ddp") == 0) return MEDIA_DDP;
        if (strcmp(type_opt, "dsk") == 0) return MEDIA_DSK;
        die("unknown --type '%s' (expected ddp or dsk)", type_opt);
    }
    {
        int g = guess_media(path);
        if (g < 0)
            die("cannot tell DDP from DSK for '%s'; pass --type ddp|dsk", path);
        return (media_t)g;
    }
}

static void today(uint8_t *y, uint8_t *mo, uint8_t *d)
{
    time_t t = time(NULL);
    struct tm *lt = localtime(&t);
    *y = (uint8_t)(lt->tm_year);        /* tm_year is already years since 1900 */
    *mo = (uint8_t)(lt->tm_mon + 1);
    *d = (uint8_t)(lt->tm_mday);
}

static int parse_date(const char *s, uint8_t *y, uint8_t *mo, uint8_t *d)
{
    int yy, mm, dd;
    if (sscanf(s, "%d-%d-%d", &yy, &mm, &dd) != 3) return -1;
    if (yy >= 1900) yy -= 1900;
    *y = (uint8_t)yy; *mo = (uint8_t)mm; *d = (uint8_t)dd;
    return 0;
}

/* ------------------------------------------------------------- commands */

static void usage(void)
{
    fputs(
"eosfs - Coleco ADAM EOS filesystem tool for DDP/DSK images\n\n"
"Usage:\n"
"  eosfs create <image> <preset> [options]\n"
"      presets: ddp256   256K DDP (256 blocks)\n"
"               dsk160   160K DSK (160 blocks)\n"
"               ddp      custom DDP, needs --blocks N\n"
"               dsk      custom DSK, needs --blocks N (multiple of 4)\n"
"      options: -v, --volume NAME     volume name (<=11 chars)\n"
"               -d, --dir-blocks N    directory blocks (default 3)\n"
"               -b, --blocks N        block count for custom presets\n\n"
"  eosfs list   <image> [--type ddp|dsk]\n"
"  eosfs add    <image> <hostfile> [--name EOSNAME] [--date YYYY-MM-DD] [--type ...]\n"
"  eosfs replace<image> <hostfile> [--name EOSNAME] [--date YYYY-MM-DD] [--type ...]\n"
"  eosfs remove <image> <eosname> [--type ...]\n"
"  eosfs extract<image> <eosname> [-o OUTFILE] [--type ...]\n\n"
"  eosfs boot   <image> [mode] [--type ...]      set up block 0 (boot block)\n"
"      (no mode)              jump to SmartWriter (0xFCE7); the default\n"
"      --none                 same: jump to SmartWriter\n"
"      --block FILE           install FILE verbatim as the 1024-byte block 0\n"
"      --file EOSNAME         load an EOS file already in the image and run it\n"
"                             (BLOAD files are detected; raw files load at\n"
"                              0x0100 by default)\n"
"          --load ADDR        load address override (hex ok, e.g. 0x100)\n"
"          --entry ADDR       entry point (default = load address)\n\n"
"Media type is inferred from the .ddp/.dsk extension unless --type is given.\n",
        stderr);
    exit(2);
}

/* Small option scanner over argv[start..argc). Returns matched value or NULL,
 * and marks consumed slots by setting them to NULL. */
static const char *opt_val(int argc, char **argv, int start,
                           const char *lng, const char *shrt)
{
    int i;
    for (i = start; i < argc; i++) {
        if (!argv[i]) continue;
        if ((lng && strcmp(argv[i], lng) == 0) ||
            (shrt && strcmp(argv[i], shrt) == 0)) {
            const char *v;
            if (i + 1 >= argc) die("%s needs a value", argv[i]);
            v = argv[i + 1];
            argv[i] = NULL;
            argv[i + 1] = NULL;
            return v;
        }
    }
    return NULL;
}

/* First remaining positional at or after `start`. */
static const char *next_pos(int argc, char **argv, int *idx)
{
    while (*idx < argc && !argv[*idx]) (*idx)++;
    if (*idx >= argc) return NULL;
    return argv[(*idx)++];
}

static void cmd_create(int argc, char **argv)
{
    const char *image, *preset, *vol, *sblocks, *sdir;
    media_t media;
    uint32_t nblk = 0, dirblk;
    Fs *fs;
    int idx = 2;

    vol     = opt_val(argc, argv, 2, "--volume", "-v");
    sblocks = opt_val(argc, argv, 2, "--blocks", "-b");
    sdir    = opt_val(argc, argv, 2, "--dir-blocks", "-d");

    image  = next_pos(argc, argv, &idx);
    preset = next_pos(argc, argv, &idx);
    if (!image || !preset) usage();

    if (strcmp(preset, "ddp256") == 0)      { media = MEDIA_DDP; nblk = 256; }
    else if (strcmp(preset, "dsk160") == 0) { media = MEDIA_DSK; nblk = 160; }
    else if (strcmp(preset, "ddp") == 0)    { media = MEDIA_DDP; }
    else if (strcmp(preset, "dsk") == 0)    { media = MEDIA_DSK; }
    else die("unknown preset '%s'", preset);

    if (strcmp(preset, "ddp") == 0 || strcmp(preset, "dsk") == 0) {
        if (!sblocks) die("preset '%s' needs --blocks N", preset);
        nblk = (uint32_t)strtoul(sblocks, NULL, 0);
    } else if (sblocks) {
        die("--blocks is only valid with the custom ddp/dsk presets");
    }

    dirblk = sdir ? (uint32_t)strtoul(sdir, NULL, 0) : DEF_DIRBLK;

    if (dirblk < 1) die("need at least 1 directory block");
    if (nblk < dirblk + 1)
        die("medium too small: %u blocks, need at least %u", nblk, dirblk + 1);
    if (3u + 1u > dirblk * EPB)
        die("directory too small to hold system entries");
    if (media == MEDIA_DSK && nblk % 4)
        die("DSK block count must be a multiple of 4 (got %u)", nblk);
    if (nblk > 0xFFFF)
        die("block count %u exceeds 65535", nblk);

    fs = xcalloc(sizeof(Fs));
    fs->media = media;
    fs->nblk = nblk;
    fs->dirblk = dirblk;
    mkname(fs->vol, vol ? vol : "");
    make_boot_smartwriter(fs->boot);   /* default: boot -> SmartWriter */

    store(fs, image);
    printf("Created %s: %s, %u blocks (%uK), %u directory blocks, volume '%s'\n",
           image, media_name(media), nblk, nblk, dirblk, vol ? vol : "");
    fs_free(fs);
}

static void cmd_list(int argc, char **argv)
{
    const char *image, *type;
    Fs *fs;
    uint32_t cur, used_data;
    size_t i;
    int idx = 2;

    type  = opt_val(argc, argv, 2, "--type", NULL);
    image = next_pos(argc, argv, &idx);
    if (!image) usage();

    fs = load(image, resolve_media(image, type));

    {
        char vn[13];
        getname(fs->vol, vn);
        printf("Volume '%s'  %s  %u blocks (%uK)  %u directory blocks\n",
               vn, media_name(fs->media), fs->nblk, fs->nblk, fs->dirblk);
    }
    printf("  %-12s %5s %5s %8s %6s  %-10s\n",
           "Name", "start", "blks", "bytes", "attr", "date");

    cur = fs->dirblk + 1;
    for (i = 0; i < fs->n; i++) {
        File *f = &fs->f[i];
        char nm[13];
        uint32_t nb = file_blocks(f->size);
        getname(f->name, nm);
        printf("  %-12s %5u %5u %8u   0x%02X  %02u-%02u-%02u\n",
               nm, cur, nb, f->size, f->attr,
               (unsigned)((f->y + 1900) % 100), f->mo, f->d);
        cur += nb;
    }
    used_data = cur - (fs->dirblk + 1);
    printf("%lu file(s), %u data block(s) used, %u free block(s)\n",
           (unsigned long)fs->n, used_data, fs->nblk - cur);
    fs_free(fs);
}

static void put_file(Fs *fs, const char *hostfile, const char *name_opt,
                     const char *date_opt, int replace)
{
    uint8_t *data;
    size_t len;
    char name[64];
    long existing;
    File *f;
    uint8_t y, mo, d;

    data = read_whole(hostfile, &len);
    if (len > 0xFFFFFFFFul) die("file too large");

    if (name_opt) {
        strncpy(name, name_opt, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    } else {
        strncpy(name, basename_of(hostfile), sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
    }
    if (name[0] == 0) die("empty EOS filename");
    if (strlen(name) > 11)
        fprintf(stderr, "eosfs: warning: name '%s' truncated to 11 chars\n", name);

    if (date_opt) {
        if (parse_date(date_opt, &y, &mo, &d) != 0)
            die("bad --date '%s' (expected YYYY-MM-DD)", date_opt);
    } else {
        today(&y, &mo, &d);
    }

    existing = fs_find(fs, name);
    if (replace) {
        if (existing < 0) die("'%s' not found (use 'add' to create it)", name);
        f = &fs->f[existing];
        free(f->data);
        f->data = data;
        f->size = (uint32_t)len;
        f->y = y; f->mo = mo; f->d = d;
    } else {
        if (existing >= 0) die("'%s' already exists (use 'replace')", name);
        f = fs_add_slot(fs);
        mkname(f->name, name);
        f->attr = A_USER;
        f->data = data;
        f->size = (uint32_t)len;
        f->y = y; f->mo = mo; f->d = d;
    }
}

static void cmd_put(int argc, char **argv, int replace)
{
    const char *image, *hostfile, *type, *name, *date;
    Fs *fs;
    int idx = 2;

    type = opt_val(argc, argv, 2, "--type", NULL);
    name = opt_val(argc, argv, 2, "--name", "-n");
    date = opt_val(argc, argv, 2, "--date", NULL);

    image    = next_pos(argc, argv, &idx);
    hostfile = next_pos(argc, argv, &idx);
    if (!image || !hostfile) usage();

    fs = load(image, resolve_media(image, type));
    put_file(fs, hostfile, name, date, replace);
    store(fs, image);
    {
        char nm[64];
        strncpy(nm, name ? name : basename_of(hostfile), sizeof(nm) - 1);
        nm[sizeof(nm) - 1] = 0;
        printf("%s '%s' in %s\n", replace ? "Replaced" : "Added", nm, image);
    }
    fs_free(fs);
}

static void cmd_extract(int argc, char **argv)
{
    const char *image, *name, *type, *out;
    Fs *fs;
    long i;
    int idx = 2;

    type = opt_val(argc, argv, 2, "--type", NULL);
    out  = opt_val(argc, argv, 2, "--out", "-o");
    image = next_pos(argc, argv, &idx);
    name  = next_pos(argc, argv, &idx);
    if (!image || !name) usage();

    fs = load(image, resolve_media(image, type));
    i = fs_find(fs, name);
    if (i < 0) die("'%s' not found", name);

    write_whole(out ? out : name, fs->f[i].data, fs->f[i].size);
    printf("Extracted '%s' (%u bytes) to %s\n",
           name, fs->f[i].size, out ? out : name);
    fs_free(fs);
}

static void cmd_remove(int argc, char **argv)
{
    const char *image, *name, *type;
    Fs *fs;
    long i;
    int idx = 2;

    type = opt_val(argc, argv, 2, "--type", NULL);
    image = next_pos(argc, argv, &idx);
    name  = next_pos(argc, argv, &idx);
    if (!image || !name) usage();

    fs = load(image, resolve_media(image, type));
    i = fs_find(fs, name);
    if (i < 0) die("'%s' not found", name);

    free(fs->f[i].data);
    memmove(&fs->f[i], &fs->f[i + 1], (fs->n - (size_t)i - 1) * sizeof(File));
    fs->n--;

    store(fs, image);
    printf("Removed '%s' from %s\n", name, image);
    fs_free(fs);
}

static void cmd_boot(int argc, char **argv)
{
    const char *image, *type, *blockfile, *eosname, *sload, *sentry;
    Fs *fs;
    int idx = 2, i;

    type      = opt_val(argc, argv, 2, "--type", NULL);
    blockfile = opt_val(argc, argv, 2, "--block", NULL);
    eosname   = opt_val(argc, argv, 2, "--file", NULL);
    sload     = opt_val(argc, argv, 2, "--load", NULL);
    sentry    = opt_val(argc, argv, 2, "--entry", NULL);
    for (i = 2; i < argc; i++)                      /* --none is a bare flag */
        if (argv[i] && strcmp(argv[i], "--none") == 0) argv[i] = NULL;

    image = next_pos(argc, argv, &idx);
    if (!image) usage();
    if (blockfile && eosname) die("give only one of --block / --file");

    fs = load(image, resolve_media(image, type));

    if (blockfile) {
        size_t len;
        uint8_t *b = read_whole(blockfile, &len);
        if (len != BLOCK)
            die("boot block must be exactly %u bytes (%s is %lu)",
                BLOCK, blockfile, (unsigned long)len);
        memcpy(fs->boot, b, BLOCK);
        free(b);
        printf("Installed verbatim boot block from %s\n", blockfile);
    } else if (eosname) {
        long fi = fs_find(fs, eosname);
        File *f;
        int bload;
        uint16_t load, entry;
        uint32_t length;
        if (fi < 0) die("'%s' not found in image", eosname);
        f = &fs->f[fi];
        bload = is_bload(f->data, f->size);
        if (bload) {
            load = (uint16_t)(f->data[3] | (f->data[4] << 8));
            length = f->size - 5;
        } else {
            load = 0x0100;
            length = f->size;
        }
        if (sload)  load  = (uint16_t)strtoul(sload, NULL, 0);   /* override */
        entry = sentry ? (uint16_t)strtoul(sentry, NULL, 0) : load;
        if (length == 0) die("boot file '%s' is empty", eosname);
        if (length > 0xFFFFu || (uint32_t)load + length > 0x10000u)
            die("payload of %u bytes at %04X does not fit in the Z80 address space",
                length, load);
        if (load < 0xCC00u && (uint32_t)load + length > 0xC800u)
            fprintf(stderr, "eosfs: warning: load range %04X..%04X overlaps "
                    "the boot block at C800..CBFF\n",
                    load, (unsigned)(load + length - 1));
        make_boot_loader(fs->boot, f->name, load, entry, length, bload);
        printf("Boot loads '%s' (%s, %u bytes) at %04X, entry %04X\n",
               eosname, bload ? "BLOAD" : "raw", length, load, entry);
    } else {
        make_boot_smartwriter(fs->boot);
        printf("Boot set to jump to SmartWriter (0x%04X)\n", V_WP);
    }

    store(fs, image);
    fs_free(fs);
}

int main(int argc, char **argv)
{
    if (argc < 2) usage();
    if (strcmp(argv[1], "create") == 0)  cmd_create(argc, argv);
    else if (strcmp(argv[1], "list") == 0 ||
             strcmp(argv[1], "dir") == 0) cmd_list(argc, argv);
    else if (strcmp(argv[1], "add") == 0) cmd_put(argc, argv, 0);
    else if (strcmp(argv[1], "replace") == 0) cmd_put(argc, argv, 1);
    else if (strcmp(argv[1], "remove") == 0 ||
             strcmp(argv[1], "rm") == 0 ||
             strcmp(argv[1], "delete") == 0) cmd_remove(argc, argv);
    else if (strcmp(argv[1], "extract") == 0 ||
             strcmp(argv[1], "get") == 0) cmd_extract(argc, argv);
    else if (strcmp(argv[1], "boot") == 0) cmd_boot(argc, argv);
    else usage();
    return 0;
}
