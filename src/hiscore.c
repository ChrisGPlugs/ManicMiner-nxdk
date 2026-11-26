/* Persistent high scores in the Xbox save area (see hiscore.h).
 * SPDX-License-Identifier: MIT */
#include "hiscore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NXDK
#include <windows.h>
#include <nxdk/mount.h>
#define SEP "\\"
static const char *save_root(void) { return "E:\\UDATA"; }
static void make_dir(const char *p) { CreateDirectoryA(p, NULL); }
static void delete_file(const char *p) { DeleteFileA(p); }
static int  rename_file(const char *from, const char *to) { return MoveFileA(from, to) ? 0 : -1; }
#else
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define SEP "\\"
static void make_dir(const char *p) { _mkdir(p); }
#else
#define SEP "/"
static void make_dir(const char *p) { mkdir(p, 0755); }
#endif
static const char *save_root(void)
{
    const char *d = getenv("MM_SAVE_DIR");
    return (d && *d) ? d : "UDATA";
}
static void delete_file(const char *p) { remove(p); }
static int  rename_file(const char *from, const char *to) { return rename(from, to); }
#endif

static int mounted;

void hs_init(void)
{
#ifdef NXDK
    if (!mounted)
        mounted = nxIsDriveMounted('E') ||
                  nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
#else
    mounted = 1;
#endif
}

/* --- save layout ---------------------------------------------------------- */

static const char *save_dir_name(int v128) { return v128 ? "000000000128" : "000000000048"; }
static const char *save_name(int v128)
{
    return v128 ? "Manic Miner 128 high score" : "Manic Miner high score";
}

static void title_dir(char *out, size_t n)
{
    snprintf(out, n, "%s" SEP HS_TITLE_ID, save_root());
}

static void game_path(char *out, size_t n, int v128, const char *file)
{
    snprintf(out, n, "%s" SEP HS_TITLE_ID SEP "%s%s%s", save_root(), save_dir_name(v128),
             file ? SEP : "", file ? file : "");
}

/* HISCORE.DAT: "MMHS", version, game, six ASCII digits, 2 spare, 16-bit sum. */
#define REC_SIZE 16

static uint16_t rec_sum(const uint8_t *r)
{
    uint16_t s = 0x4D4D;
    for (int i = 0; i < REC_SIZE - 2; i++) s = (uint16_t)(s * 31 + r[i]);
    return s;
}

static int read_rec(const char *path, int v128, uint8_t digits[6])
{
    uint8_t r[REC_SIZE];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t got = fread(r, 1, sizeof r, f);
    fclose(f);
    if (got != sizeof r || memcmp(r, "MMHS", 4) || r[4] != 1 || r[5] != (v128 ? 128 : 48))
        return 0;
    if ((uint16_t)(r[14] | r[15] << 8) != rec_sum(r)) return 0;
    for (int i = 0; i < 6; i++)
        if (r[6 + i] < '0' || r[6 + i] > '9') return 0;
    memcpy(digits, r + 6, 6);
    return 1;
}

static int write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* The dashboard's metadata files: UTF-16LE with a BOM, "Key=Value\r\n". */
static int write_meta(const char *path, const char *key, const char *value)
{
    uint8_t buf[256];
    char line[120];
    int n = snprintf(line, sizeof line, "%s=%s\r\n", key, value);
    size_t len = 0;
    buf[len++] = 0xFF; buf[len++] = 0xFE;
    for (int i = 0; i < n && len + 2 <= sizeof buf; i++) {
        buf[len++] = (uint8_t)line[i];
        buf[len++] = 0;
    }
    return write_file(path, buf, len);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* --- public --------------------------------------------------------------- */

int hs_load(int v128, uint8_t digits[6])
{
    char p[160];
    if (!mounted) return 0;
    game_path(p, sizeof p, v128, "HISCORE.DAT");
    if (read_rec(p, v128, digits)) return 1;
    game_path(p, sizeof p, v128, "HISCORE.TMP");      /* an interrupted save */
    return read_rec(p, v128, digits);
}

int hs_save(int v128, const uint8_t digits[6])
{
    char p[160], tmp[160], dat[160];
    uint8_t r[REC_SIZE] = { 'M', 'M', 'H', 'S', 1 };
    if (!mounted) return 0;

    make_dir(save_root());
    title_dir(p, sizeof p);
    make_dir(p);
    snprintf(p + strlen(p), sizeof p - strlen(p), SEP "TitleMeta.xbx");
    if (!file_exists(p)) write_meta(p, "TitleName", "Manic Miner");

    game_path(p, sizeof p, v128, NULL);
    make_dir(p);
    game_path(p, sizeof p, v128, "SaveMeta.xbx");
    if (!file_exists(p)) write_meta(p, "Name", save_name(v128));

    r[5] = v128 ? 128 : 48;
    memcpy(r + 6, digits, 6);
    uint16_t s = rec_sum(r);
    r[14] = (uint8_t)s; r[15] = (uint8_t)(s >> 8);

    game_path(tmp, sizeof tmp, v128, "HISCORE.TMP");
    game_path(dat, sizeof dat, v128, "HISCORE.DAT");
    if (!write_file(tmp, r, sizeof r)) return 0;
    delete_file(dat);
    return rename_file(tmp, dat) == 0;
}
