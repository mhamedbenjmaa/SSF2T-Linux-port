// DOS drives for the game:
//   C: = the installed game folder (read-only). Files the game writes go to a
//        save folder, which is checked first on later reads.
//   D: = the CD image (.cue/.bin): ISO9660 data track + audio tracks.
#define _GNU_SOURCE
#include "platform.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char game_dir[1024], save_dir[1024];

// ------------------------------------------------------------- CD image --
#define MAX_TRACKS 99
typedef struct { uint32_t start, len; bool audio; } Track;
static Track tracks[MAX_TRACKS];
static int ntracks;
static int bin_fd = -1;
static uint32_t total_frames;

static bool cue_load(const char *cue) {
    FILE *f = fopen(cue, "r");
    if (!f) return false;
    char line[512], bin[1024] = "";
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (!strncasecmp(s, "FILE", 4)) {
            char *q1 = strchr(s, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (q1 && q2) {
                *q2 = 0;
                const char *slash = strrchr(cue, '/');
                snprintf(bin, sizeof bin, "%.*s%s", slash ? (int)(slash - cue + 1) : 0, cue, q1 + 1);
            }
        } else if (!strncasecmp(s, "TRACK", 5)) {
            int n; char type[32];
            if (sscanf(s + 5, "%d %31s", &n, type) == 2 && n >= 1 && n <= MAX_TRACKS) {
                cur = n - 1;
                tracks[cur].audio = !strcasecmp(type, "AUDIO");
                if (n > ntracks) ntracks = n;
            }
        } else if (!strncasecmp(s, "INDEX", 5) && cur >= 0) {
            int idx, mm, ss, ff;
            if (sscanf(s + 5, "%d %d:%d:%d", &idx, &mm, &ss, &ff) == 4 && idx == 1)
                tracks[cur].start = (mm * 60 + ss) * 75 + ff;
        }
    }
    fclose(f);
    if (!bin[0]) return false;
    bin_fd = open(bin, O_RDONLY);
    if (bin_fd < 0) return false;
    struct stat st;
    fstat(bin_fd, &st);
    total_frames = st.st_size / 2352;
    for (int i = 0; i < ntracks; i++)
        tracks[i].len = (i + 1 < ntracks ? tracks[i + 1].start : total_frames) - tracks[i].start;
    LOG("CD: %d tracks, %u frames\n", ntracks, total_frames);
    return true;
}

bool fs_cd_present(void) { return bin_fd >= 0; }
int cd_track_count(void) { return ntracks; }
uint32_t cd_leadout(void) { return total_frames; }
bool cd_track_info(int t, uint32_t *start, uint32_t *len, bool *audio) {
    if (t < 1 || t > ntracks) return false;
    *start = tracks[t - 1].start; *len = tracks[t - 1].len; *audio = tracks[t - 1].audio;
    return true;
}
bool cd_read_raw(uint32_t lba, uint8_t *out) {
    if (lba >= total_frames) return false;
    return pread(bin_fd, out, 2352, (off_t)lba * 2352) == 2352;
}
bool cd_read_cooked(uint32_t lba, uint8_t *out) {
    if (lba >= total_frames) return false;
    return pread(bin_fd, out, 2048, (off_t)lba * 2352 + 16) == 2048;
}

// ---- ISO9660 ----
typedef struct { uint32_t lba, size; bool dir; char name[64]; } IsoEnt;

static void iso_name(const uint8_t *rec, char *out) {
    int n = rec[32];
    if (n == 1 && (rec[33] == 0 || rec[33] == 1)) { strcpy(out, rec[33] ? ".." : "."); return; }
    int k = 0;
    for (int i = 0; i < n && k < 63; i++) {
        if (rec[33 + i] == ';') break;
        out[k++] = (char)toupper(rec[33 + i]);
    }
    if (k && out[k - 1] == '.') k--;
    out[k] = 0;
}

// Calls cb for each entry in the directory; stops when cb returns true.
static bool iso_scan(uint32_t lba, uint32_t size, bool (*cb)(IsoEnt *, void *), void *ud) {
    uint8_t sec[2048];
    for (uint32_t off = 0; off < size; off += 2048) {
        if (!cd_read_cooked(lba + off / 2048, sec)) return false;
        for (uint32_t p = 0; p < 2048 && sec[p];) {
            uint8_t *r = sec + p;
            IsoEnt e;
            e.lba = r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
            e.size = r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
            e.dir = (r[25] & 2) != 0;
            iso_name(r, e.name);
            if (cb(&e, ud)) return true;
            p += r[0];
        }
    }
    return false;
}

typedef struct { const char *want; IsoEnt found; bool ok; } IsoFind;
static bool iso_match_cb(IsoEnt *e, void *ud) {
    IsoFind *f = ud;
    if (!strcasecmp(e->name, f->want)) { f->found = *e; f->ok = true; return true; }
    return false;
}

static bool iso_root(IsoEnt *root) {
    uint8_t pvd[2048];
    if (!cd_read_cooked(16, pvd) || memcmp(pvd + 1, "CD001", 5)) return false;
    uint8_t *r = pvd + 156;
    root->lba = r[2] | r[3] << 8 | r[4] << 16 | (uint32_t)r[5] << 24;
    root->size = r[10] | r[11] << 8 | r[12] << 16 | (uint32_t)r[13] << 24;
    root->dir = true;
    strcpy(root->name, "");
    return true;
}

// path: components separated by '\' with no drive letter
static bool iso_lookup(const char *path, IsoEnt *out) {
    IsoEnt cur;
    if (!iso_root(&cur)) return false;
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *tok = strtok(buf, "\\/"); tok; tok = strtok(NULL, "\\/")) {
        if (!strcmp(tok, ".")) continue;
        if (!cur.dir) return false;
        IsoFind f = { tok, {0}, false };
        iso_scan(cur.lba, cur.size, iso_match_cb, &f);
        if (!f.ok) return false;
        cur = f.found;
    }
    *out = cur;
    return true;
}

// ------------------------------------------------------------ paths ----
// Split a DOS path into drive letter ('C' default) and a relative path.
static char split_path(const char *dos, char *rel, size_t n) {
    char drive = 'C';
    if (dos[0] && dos[1] == ':') { drive = (char)toupper((unsigned char)dos[0]); dos += 2; }
    while (*dos == '\\' || *dos == '/') dos++;
    snprintf(rel, n, "%s", dos);
    for (char *p = rel; *p; p++) if (*p == '/') *p = '\\';
    return drive;
}

// Find a host path under root matching rel case-insensitively.
// If the last component is missing and allow_new, build it uppercase.
static bool host_resolve(const char *root, const char *rel, char *out, size_t n, bool allow_new) {
    char buf[512], cur[1024];
    snprintf(buf, sizeof buf, "%s", rel);
    snprintf(cur, sizeof cur, "%s", root);
    char *save = NULL;
    for (char *tok = strtok_r(buf, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(tok, ".") || !*tok) continue;
        DIR *d = opendir(cur);
        bool found = false;
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)))
                if (!strcasecmp(de->d_name, tok)) {
                    size_t l = strlen(cur);
                    snprintf(cur + l, sizeof cur - l, "/%s", de->d_name);
                    found = true;
                    break;
                }
            closedir(d);
        }
        if (!found) {
            bool last = save == NULL || *save == 0;
            if (!allow_new || !last) return false;
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof cur - l, "/%s", tok);
            for (char *p = cur + l + 1; *p; p++) *p = (char)toupper((unsigned char)*p);
        }
    }
    snprintf(out, n, "%s", cur);
    return true;
}

void mkdirs_for(const char *file) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", file);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
}

// ---------------------------------------------------------- handles ----
typedef struct { int kind; int fd; uint32_t lba, size, pos; } Handle;   // kind 0=free 1=host 2=iso
#define MAX_HANDLES 64
static Handle handles[MAX_HANDLES];

static int new_handle(void) {
    for (int i = 0; i < MAX_HANDLES; i++) if (!handles[i].kind) return i;
    return -1;
}

int fs_open(const char *dospath, int access, int disp) {
    char rel[512];
    char drive = split_path(dospath, rel, sizeof rel);
    bool wantw = (access & 0x40000000) != 0;
    int h = new_handle();
    if (h < 0) return -1;
    if (drive == 'D') {
        if (wantw) { errno = EACCES; return -1; }
        IsoEnt e;
        if (!iso_lookup(rel, &e) || e.dir) { errno = ENOENT; return -1; }
        handles[h] = (Handle){ 2, -1, e.lba, e.size, 0 };
        LOG("open D:\\%s -> h%d (%u bytes)\n", rel, h, e.size);
        return h;
    }
    if (drive != 'C') { errno = ENOENT; return -1; }
    char spath[1024], gpath[1024];
    bool in_save = host_resolve(save_dir, rel, spath, sizeof spath, false);
    bool in_game = host_resolve(game_dir, rel, gpath, sizeof gpath, false);
    int fd;
    if (!wantw) {
        if (disp == 1 || disp == 2) wantw = true;  // create implies write
    }
    if (!wantw) {
        if (!in_save && !in_game) { errno = ENOENT; return -1; }
        fd = open(in_save ? spath : gpath, O_RDONLY);
    } else {
        // writes always go to the save folder (copy-on-write)
        if (!in_save) {
            host_resolve(save_dir, rel, spath, sizeof spath, true);
            mkdirs_for(spath);
        }
        bool exists = in_save || in_game;
        if (disp == 1 && exists) { errno = EEXIST; return -1; }
        if ((disp == 3 || disp == 5) && !exists) { errno = ENOENT; return -1; }
        if (!in_save && in_game && (disp == 3 || disp == 4)) {
            // copy original content first
            int src = open(gpath, O_RDONLY), dst = open(spath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            char buf[65536]; ssize_t r;
            while (src >= 0 && dst >= 0 && (r = read(src, buf, sizeof buf)) > 0) write(dst, buf, r);
            if (src >= 0) close(src);
            if (dst >= 0) close(dst);
        }
        int fl = O_RDWR | O_CREAT;
        if (disp == 2 || disp == 5) fl |= O_TRUNC;
        fd = open(spath, fl, 0644);
    }
    if (fd < 0) return -1;
    handles[h] = (Handle){ 1, fd, 0, 0, 0 };
    LOG("open C:\\%s (%s) -> h%d\n", rel, wantw ? "w" : "r", h);
    return h;
}

int fs_read(int h, void *buf, uint32_t n) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].kind) return -1;
    Handle *H = &handles[h];
    if (H->kind == 1) return (int)read(H->fd, buf, n);
    if (H->pos >= H->size) return 0;
    if (n > H->size - H->pos) n = H->size - H->pos;
    uint8_t sec[2048];
    uint32_t done = 0;
    while (done < n) {
        uint32_t p = H->pos + done;
        if (!cd_read_cooked(H->lba + p / 2048, sec)) break;
        uint32_t off = p % 2048, k = 2048 - off;
        if (k > n - done) k = n - done;
        memcpy((uint8_t *)buf + done, sec + off, k);
        done += k;
    }
    H->pos += done;
    return (int)done;
}

int fs_write(int h, const void *buf, uint32_t n) {
    if (h < 0 || h >= MAX_HANDLES || handles[h].kind != 1) return -1;
    return (int)write(handles[h].fd, buf, n);
}

int64_t fs_seek(int h, int64_t off, int whence) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].kind) return -1;
    Handle *H = &handles[h];
    if (H->kind == 1) return lseek(H->fd, off, whence);
    int64_t np = whence == 0 ? off : whence == 1 ? H->pos + off : H->size + off;
    if (np < 0) return -1;
    H->pos = (uint32_t)np;
    return np;
}

void fs_close(int h) {
    if (h < 0 || h >= MAX_HANDLES || !handles[h].kind) return;
    if (handles[h].kind == 1) close(handles[h].fd);
    handles[h].kind = 0;
}

bool fs_delete(const char *dospath) {
    char rel[512], p[1024];
    if (split_path(dospath, rel, sizeof rel) != 'C') return false;
    // only files we created can be deleted; the original game stays intact
    if (host_resolve(save_dir, rel, p, sizeof p, false)) return unlink(p) == 0;
    return false;
}

int fs_attributes(const char *dospath) {
    char rel[512], p[1024];
    char drive = split_path(dospath, rel, sizeof rel);
    if (drive == 'D') {
        IsoEnt e;
        if (!iso_lookup(rel, &e)) return -1;
        return e.dir ? 0x11 : 0x01;
    }
    if (drive != 'C') return -1;
    if (!rel[0]) return 0x10;
    if (!host_resolve(save_dir, rel, p, sizeof p, false) && !host_resolve(game_dir, rel, p, sizeof p, false))
        return -1;
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return S_ISDIR(st.st_mode) ? 0x10 : 0x20;
}

// ------------------------------------------------------------- find ----
#define MAX_SEARCH 8
typedef struct { bool used; FsFind *items; int n, pos; } Search;
static Search searches[MAX_SEARCH];

static bool wild_match(const char *pat, const char *s) {
    // DOS-style: '*' any run, '?' one char; "*.*" matches names without dot
    if (!strcmp(pat, "*.*")) return true;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return true;
            for (; *s; s++) if (wild_match(pat, s)) return true;
            return wild_match(pat, s);
        }
        if (!*s) return false;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)) return false;
        pat++; s++;
    }
    return *s == 0;
}

typedef struct { Search *s; const char *pat; } IsoListCtx;
static bool iso_list_cb(IsoEnt *e, void *ud) {
    IsoListCtx *x = ud;
    if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) return false;
    if (!wild_match(x->pat, e->name)) return false;
    x->s->items = realloc(x->s->items, (x->s->n + 1) * sizeof(FsFind));
    FsFind *f = &x->s->items[x->s->n++];
    snprintf(f->name, sizeof f->name, "%s", e->name);
    f->size = e->size; f->attr = e->dir ? 0x11 : 0x01; f->mtime = 0;
    return false;
}

static void host_list(Search *s, const char *dir, const char *pat) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!wild_match(pat, de->d_name)) continue;
        bool dup = false;
        for (int i = 0; i < s->n; i++) if (!strcasecmp(s->items[i].name, de->d_name)) dup = true;
        if (dup) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        s->items = realloc(s->items, (s->n + 1) * sizeof(FsFind));
        FsFind *f = &s->items[s->n++];
        snprintf(f->name, sizeof f->name, "%s", de->d_name);
        for (char *q = f->name; *q; q++) *q = (char)toupper((unsigned char)*q);
        f->size = (uint32_t)st.st_size;
        f->attr = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        f->mtime = (uint32_t)st.st_mtime;
    }
    closedir(d);
}

int fs_find_first(const char *pattern, FsFind *out) {
    int sh = -1;
    for (int i = 0; i < MAX_SEARCH; i++) if (!searches[i].used) { sh = i; break; }
    if (sh < 0) return -1;
    Search *s = &searches[sh];
    *s = (Search){ true, NULL, 0, 0 };
    char rel[512];
    char drive = split_path(pattern, rel, sizeof rel);
    char *slash = strrchr(rel, '\\');
    const char *pat = slash ? slash + 1 : rel;
    char dirrel[512];
    snprintf(dirrel, sizeof dirrel, "%.*s", slash ? (int)(slash - rel) : 0, rel);
    if (drive == 'D') {
        IsoEnt e;
        if (iso_lookup(dirrel, &e) && e.dir) {
            IsoListCtx x = { s, pat };
            iso_scan(e.lba, e.size, iso_list_cb, &x);
        }
    } else if (drive == 'C') {
        char p[1024];
        if (host_resolve(save_dir, dirrel, p, sizeof p, false)) host_list(s, p, pat);
        if (host_resolve(game_dir, dirrel, p, sizeof p, false)) host_list(s, p, pat);
    }
    if (s->n == 0) { free(s->items); s->used = false; return -1; }
    *out = s->items[s->pos++];
    return sh;
}

bool fs_find_next(int sh, FsFind *out) {
    if (sh < 0 || sh >= MAX_SEARCH || !searches[sh].used) return false;
    Search *s = &searches[sh];
    if (s->pos >= s->n) return false;
    *out = s->items[s->pos++];
    return true;
}

void fs_find_close(int sh) {
    if (sh < 0 || sh >= MAX_SEARCH || !searches[sh].used) return;
    free(searches[sh].items);
    searches[sh].used = false;
}

void fs_init(const char *gdir, const char *cue, const char *sdir) {
    snprintf(game_dir, sizeof game_dir, "%s", gdir);
    snprintf(save_dir, sizeof save_dir, "%s", sdir);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s/x", save_dir);
    mkdirs_for(tmp);
    if (*cue && !cue_load(cue)) fprintf(stderr, "warning: CD image %s not found, the game will ask for its CD\n", cue);
}
