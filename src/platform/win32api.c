// The kernel32/user32 subset the game imports (as provided by Borland's
// 32rtm). Each api_X is reached by "call [IAT]" with the return address on
// the stack; stdcall: it pops the return address and its arguments.
#include "platform.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t g_tib;
static uint32_t last_error, str_cmdline, str_env, str_modname;

#define HBASE 0x100
#define FINDBASE 0x200

static uint32_t A(int i) { return rd32(R.esp + 4 + 4 * i); }
static void ret(int nargs, uint32_t v) { R.eax = v; R.esp += 4 + 4 * nargs; }
static const char *S(uint32_t p) { return p ? (const char *)G(p) : ""; }
static int to_fs(uint32_t h) { return h >= HBASE && h < HBASE + 64 ? (int)(h - HBASE) : -1; }
static void err_errno(void) { last_error = errno == ENOENT ? 2 : errno == EACCES ? 5 : errno == EEXIST ? 80 : 31; }

#define API(name, n) void api_##name(void)

API(GetModuleHandleA, 1) {
    uint32_t r = A(0) == 0 ? 0x100000 : !strncmp(S(A(0)), "kernel32", 8) || !strncmp(S(A(0)), "KERNEL32", 8) ? 0x7FF00000 : 0;
    ret(1, r);
}
API(GetProcAddress, 2) {
    // only "Borland32" is looked up: its presence means "running under 32rtm"
    ret(2, A(1) >= 0x10000 && !strcmp(S(A(1)), "Borland32") ? 0xFFFF00FFu : 0);
}
API(GetModuleFileNameA, 3) {
    const char *s = S(str_modname);
    uint32_t n = (uint32_t)strlen(s);
    if (A(2) == 0) { ret(3, 0); return; }
    if (n >= A(2)) n = A(2) - 1;
    memcpy(G(A(1)), s, n);
    wr8(A(1) + n, 0);
    ret(3, n);
}
API(ExitProcess, 1) { fflush(stdout); exit((int)A(0)); }
API(GetCommandLineA, 0) { ret(0, str_cmdline); }
API(GetEnvironmentStrings, 0) { ret(0, str_env); }
API(GetVersion, 0) { ret(0, 0x80000003u); }
API(GetCurrentThreadId, 0) { ret(0, 1); }
API(GetLastError, 0) { ret(0, last_error); }
API(SetHandleCount, 1) { ret(1, A(0)); }
API(GetStartupInfoA, 1) { memset(G(A(0)), 0, 68); wr32(A(0), 68); ret(1, 0); }
API(GetStdHandle, 1) {
    int32_t n = (int32_t)A(0);
    ret(1, n == -10 ? 0x10 : n == -11 ? 0x11 : n == -12 ? 0x12 : 0xFFFFFFFFu);
}
API(GetFileType, 1) { ret(1, A(0) >= 0x10 && A(0) <= 0x12 ? 2 : to_fs(A(0)) >= 0 ? 1 : 0); }
API(GetLocalTime, 1) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    uint32_t p = A(0);
    wr16(p, (uint16_t)(tm->tm_year + 1900)); wr16(p + 2, (uint16_t)(tm->tm_mon + 1)); wr16(p + 4, (uint16_t)tm->tm_wday);
    wr16(p + 6, (uint16_t)tm->tm_mday); wr16(p + 8, (uint16_t)tm->tm_hour); wr16(p + 10, (uint16_t)tm->tm_min);
    wr16(p + 12, (uint16_t)tm->tm_sec); wr16(p + 14, 0);
    ret(1, 0);
}
API(FileTimeToLocalFileTime, 2) { wr32(A(1), rd32(A(0))); wr32(A(1) + 4, rd32(A(0) + 4)); ret(2, 1); }
API(FileTimeToDosDateTime, 3) {
    uint64_t ft = rd32(A(0)) | (uint64_t)rd32(A(0) + 4) << 32;
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm *tm = localtime(&t);
    wr16(A(1), (uint16_t)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday));
    wr16(A(2), (uint16_t)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2)));
    ret(3, 1);
}
API(CreateFileA, 7) {
    int h = fs_open(S(A(0)), (int)A(1), (int)A(4));
    LOG("CreateFileA(%s) = %d\n", S(A(0)), h);
    if (h < 0) { err_errno(); ret(7, 0xFFFFFFFFu); return; }
    last_error = 0;
    ret(7, HBASE + h);
}
API(CloseHandle, 1) { int h = to_fs(A(0)); if (h >= 0) fs_close(h); ret(1, 1); }
API(ReadFile, 5) {
    int h = to_fs(A(0)), n = h >= 0 ? fs_read(h, G(A(1)), A(2)) : 0;
    if (A(3)) wr32(A(3), n < 0 ? 0 : (uint32_t)n);
    if (n < 0) { last_error = 6; ret(5, 0); return; }
    ret(5, 1);
}
API(WriteFile, 5) {
    int n;
    if (A(0) == 0x11 || A(0) == 0x12) { n = (int)fwrite(G(A(1)), 1, A(2), A(0) == 0x11 ? stdout : stderr); fflush(stdout); }
    else { int h = to_fs(A(0)); n = h >= 0 ? fs_write(h, G(A(1)), A(2)) : -1; }
    if (A(3)) wr32(A(3), n < 0 ? 0 : (uint32_t)n);
    if (n < 0) { last_error = 6; ret(5, 0); return; }
    ret(5, 1);
}
API(SetFilePointer, 4) {
    int h = to_fs(A(0));
    int64_t off = (int32_t)A(1);
    if (A(2)) off = (int64_t)((uint64_t)rd32(A(2)) << 32 | A(1));
    int64_t r = h >= 0 ? fs_seek(h, off, (int)A(3)) : -1;
    if (r < 0) { last_error = 131; ret(4, 0xFFFFFFFFu); return; }
    if (A(2)) wr32(A(2), (uint32_t)(r >> 32));
    ret(4, (uint32_t)r);
}
API(DeleteFileA, 1) { if (fs_delete(S(A(0)))) ret(1, 1); else { last_error = 2; ret(1, 0); } }
API(GetFileAttributesA, 1) {
    int r = fs_attributes(S(A(0)));
    if (r < 0) { last_error = 2; ret(1, 0xFFFFFFFFu); } else ret(1, (uint32_t)r);
}
static void fill_find(uint32_t p, FsFind *f) {
    memset(G(p), 0, 318);
    wr32(p, (uint32_t)f->attr);
    uint64_t ft = ((uint64_t)f->mtime + 11644473600ull) * 10000000ull;
    for (int i = 0; i < 3; i++) { wr32(p + 4 + i * 8, (uint32_t)ft); wr32(p + 8 + i * 8, (uint32_t)(ft >> 32)); }
    wr32(p + 32, f->size);
    strncpy(G(p + 44), f->name, 259);
    strncpy(G(p + 304), f->name, 13);
}
API(FindFirstFileA, 2) {
    FsFind f;
    int sh = fs_find_first(S(A(0)), &f);
    if (sh < 0) { last_error = 2; ret(2, 0xFFFFFFFFu); return; }
    fill_find(A(1), &f);
    ret(2, FINDBASE + sh);
}
API(FindNextFileA, 2) {
    FsFind f;
    if (!fs_find_next((int)A(0) - FINDBASE, &f)) { last_error = 18; ret(2, 0); return; }
    fill_find(A(1), &f);
    ret(2, 1);
}
API(FindClose, 1) { fs_find_close((int)A(0) - FINDBASE); ret(1, 1); }
API(VirtualAlloc, 4) {
    uint32_t r = A(0) ? A(0) : mem_alloc(A(1), 0x10000);
    if (!r) last_error = 8;
    ret(4, r);
}
API(VirtualFree, 3) { if (A(2) & 0x8000) mem_free(A(0)); ret(3, 1); }
API(RaiseException, 4) { die("game raised exception %08x", A(0)); }
API(RtlUnwind, 4) { if (A(0)) wr32(g_tib, A(0)); ret(4, A(3)); }
API(UnhandledExceptionFilter, 1) { ret(1, 1); }
API(GetConsoleMode, 2) { if (A(1)) wr32(A(1), 3); ret(2, 1); }
API(GetConsoleScreenBufferInfo, 2) {
    uint32_t p = A(1);
    memset(G(p), 0, 22);
    wr16(p, 80); wr16(p + 2, 25); wr16(p + 8, 7); wr16(p + 14, 79); wr16(p + 16, 24); wr16(p + 18, 80); wr16(p + 20, 25);
    ret(2, 1);
}
API(GetLargestConsoleWindowSize, 1) { ret(1, 0x00190050); }
API(GetNumberOfConsoleInputEvents, 2) { if (A(1)) wr32(A(1), 0); ret(2, 1); }
API(PeekConsoleInputA, 4) { if (A(3)) wr32(A(3), 0); ret(4, 1); }
API(ReadConsoleInputA, 4) { if (A(3)) wr32(A(3), 0); ret(4, 1); }
API(FillConsoleOutputAttribute, 5) { if (A(4)) wr32(A(4), A(2)); ret(5, 1); }
API(FillConsoleOutputCharacterA, 5) { if (A(4)) wr32(A(4), A(2)); ret(5, 1); }
API(SetConsoleCtrlHandler, 2) { ret(2, 1); }
API(SetConsoleCursorPosition, 2) { ret(2, 1); }
API(SetConsoleMode, 2) { ret(2, 1); }
API(SetConsoleScreenBufferSize, 2) { ret(2, 1); }
API(SetConsoleWindowInfo, 3) { ret(3, 1); }
API(MessageBoxA, 4) { fprintf(stderr, "[%s] %s\n", S(A(2)), S(A(1))); ret(4, 1); }
API(EnumThreadWindows, 3) { ret(3, 1); }
static void api_Borland32(void) { ret(0, 0); }

// IAT slots hold 0xFFFF0000 | index; calls through thunks land here.
typedef struct { uint32_t slot; void (*fn)(void); } IatEnt;
#define X(slot, name) { slot, api_##name },
static const IatEnt iat_table[] = { IAT_ENTRIES };
#undef X
#define NIAT (int)(sizeof iat_table / sizeof *iat_table)

void api_dispatch(uint32_t v) {
    int i = (int)(v & 0xFFFF);
    if (i == 0xFF) { api_Borland32(); return; }
    if (i < NIAT) { iat_table[i].fn(); return; }
    die("call to unknown API slot %08x", v);
}

static uint32_t put_str(uint32_t *at, const char *s, size_t n) {
    uint32_t p = *at;
    memcpy(G(p), s, n);
    *at += (uint32_t)((n + 3) & ~3u);
    return p;
}

void win32_init(const char *cmdline) {
    for (int i = 0; i < NIAT; i++) wr32(iat_table[i].slot, 0xFFFF0000u | (uint32_t)i);
    uint32_t at = THUNK_BASE + 0x1000;
    char buf[512];
    snprintf(buf, sizeof buf, "C:\\SF2TURBO.EXE%s%s", cmdline[0] ? " " : "", cmdline);
    str_cmdline = put_str(&at, buf, strlen(buf) + 1);
    str_modname = put_str(&at, "C:\\SF2TURBO.EXE", 16);
    static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0\0";
    str_env = put_str(&at, env, sizeof env);
    g_tib = THUNK_BASE + 0x800;
}
