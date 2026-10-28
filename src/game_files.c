/**
 * The game's files: where the player's copy of Turok: Evolution is, and
 * getting it there from a disc image.
 *
 * The recompiled code is the USA release's default.xbe, so the game needs
 * that disc's files: default.xbe and the supertree*.tre archives beside it.
 * They are looked for, in order:
 *
 *   - RECOMP_GAME_DIR, if set;
 *   - the folder turok_settings.ini remembers ([paths] game_dir);
 *   - a "game" folder beside the program;
 *   - the folder the build was made with (main.c's YOUR_GAME_DIR);
 *   - a disc image (.iso or .xiso) beside the program, extracted once into
 *     "game";
 *   - and else the player is asked for the image or the extracted
 *     default.xbe.
 *
 * Every candidate's default.xbe must be the USA release's, by its SHA-1:
 * the code is tied to that executable's addresses. A folder found is
 * remembered, so the question is asked once.
 *
 * Disc images are XDVDFS: 2048-byte sectors, the volume descriptor at sector
 * 32 of the filesystem ("MICROSOFT*XBOX*MEDIA", then the root directory's
 * sector and size). A full image of the disc has the filesystem 0x18300000
 * bytes in (an Xbox game disc of Turok's time); an .xiso, trimmed, at 0. A
 * directory is a binary tree of entries: left and right child offsets in
 * 4-byte units (0 for none), the start sector, the size, attributes (0x10 a
 * directory) and the name.
 */

#include <windows.h>
#include <commdlg.h>
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_files.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "comdlg32.lib")

#define USA_XBE_SHA1 "25e1d77b216a2e557d7a5de6266cda64ae62627b"
#define SECTOR 2048u

static char s_ini_path[MAX_PATH], s_exe_dir[MAX_PATH];

static int file_exists(const char *path)
{
    DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* SHA-1 of a file, as lower-case hex; 0 if it cannot be read. */
static int file_sha1(const char *path, char hex[41])
{
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    unsigned char digest[20], buffer[65536];
    FILE *file = fopen(path, "rb");
    size_t got;
    int ok = 0, i;
    if (!file) return 0;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, NULL, 0) == 0 &&
            BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) == 0) {
        while ((got = fread(buffer, 1, sizeof buffer, file)) > 0) BCryptHashData(hash, buffer, (ULONG)got, 0);
        ok = BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    fclose(file);
    for (i = 0; ok && i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return ok;
}

/* A folder holding the USA disc's files. `say` reports a wrong default.xbe. */
static int usable_dir(const char *dir, int say)
{
    char path[MAX_PATH], hex[41];
    snprintf(path, sizeof path, "%s\\supertree0.tre", dir);
    if (!*dir || !file_exists(path)) return 0;
    snprintf(path, sizeof path, "%s\\default.xbe", dir);
    if (!file_exists(path)) return 0;
    if (file_sha1(path, hex) && !strcmp(hex, USA_XBE_SHA1)) return 1;
    fprintf(stderr, "[GAME] %s is not the USA release's default.xbe\n", path);
    if (say)
        MessageBoxA(NULL, "These files are not Turok: Evolution for the original Xbox, USA release.\n\n"
                    "This build runs that release's default.xbe only (the European and other "
                    "releases differ in their code).", "Turok: Evolution", MB_ICONWARNING);
    return 0;
}

/* ── Disc images ─────────────────────────────────────────────────── */

typedef struct Image {
    HANDLE file;
    uint64_t base;                  /* where the filesystem starts */
    uint64_t total, done;           /* bytes to write, and written */
    HWND progress;
    char status[160];
} Image;

static int image_read(Image *image, uint64_t offset, void *buffer, DWORD size)
{
    LARGE_INTEGER at;
    DWORD got = 0;
    at.QuadPart = (LONGLONG)(image->base + offset);
    return SetFilePointerEx(image->file, at, NULL, FILE_BEGIN) && ReadFile(image->file, buffer, size, &got, NULL) &&
           got == size;
}

static int image_open(Image *image, const char *path)
{
    static const uint64_t bases[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};
    char magic[20];
    size_t b;
    memset(image, 0, sizeof *image);
    image->file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (image->file == INVALID_HANDLE_VALUE) return 0;
    for (b = 0; b < sizeof bases / sizeof *bases; b++) {
        image->base = bases[b];
        if (image_read(image, 32ull * SECTOR, magic, sizeof magic) && !memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20))
            return 1;
    }
    CloseHandle(image->file);
    image->file = INVALID_HANDLE_VALUE;
    return 0;
}

static LRESULT CALLBACK progress_proc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        RECT area;
        HDC dc = BeginPaint(window, &paint);
        Image *image = (Image *)GetWindowLongPtrA(window, GWLP_USERDATA);
        GetClientRect(window, &area);
        FillRect(dc, &area, (HBRUSH)(COLOR_WINDOW + 1));
        if (image) {
            RECT bar = {16, area.bottom - 34, area.right - 16, area.bottom - 18}, filled = bar;
            InflateRect(&area, -16, -12);
            DrawTextA(dc, image->status, -1, &area, DT_LEFT | DT_TOP | DT_WORDBREAK);
            FrameRect(dc, &bar, (HBRUSH)GetStockObject(GRAY_BRUSH));
            if (image->total)
                filled.right = bar.left + (LONG)((bar.right - bar.left) * (double)image->done / (double)image->total);
            FillRect(dc, &filled, (HBRUSH)(COLOR_HIGHLIGHT + 1));
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_CLOSE) return 0;      /* runs to the end */
    return DefWindowProcA(window, message, w, l);
}

static void progress_show(Image *image, const char *text)
{
    MSG message;
    if (text) snprintf(image->status, sizeof image->status, "%s", text);
    if (!image->progress) return;
    InvalidateRect(image->progress, NULL, FALSE);
    while (PeekMessageA(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

static void progress_open(Image *image)
{
    WNDCLASSA kind;
    memset(&kind, 0, sizeof kind);
    kind.lpfnWndProc = progress_proc;
    kind.hInstance = GetModuleHandleA(NULL);
    kind.hCursor = LoadCursorA(NULL, IDC_ARROW);
    kind.lpszClassName = "TurokGameFiles";
    RegisterClassA(&kind);
    image->progress = CreateWindowA("TurokGameFiles", "Turok: Evolution", WS_CAPTION | WS_VISIBLE,
                                    (GetSystemMetrics(SM_CXSCREEN) - 460) / 2, (GetSystemMetrics(SM_CYSCREEN) - 150) / 2,
                                    460, 150, NULL, NULL, kind.hInstance, NULL);
    if (image->progress) SetWindowLongPtrA(image->progress, GWLP_USERDATA, (LONG_PTR)image);
}

/* One directory's table, read whole. */
static uint8_t *read_table(Image *image, uint32_t sector, uint32_t size)
{
    uint32_t bytes = (size + SECTOR - 1) / SECTOR * SECTOR;
    uint8_t *table;
    if (!size || size > 64u * 1024 * 1024) return NULL;
    table = (uint8_t *)malloc(bytes);
    if (table && !image_read(image, (uint64_t)sector * SECTOR, table, bytes)) { free(table); table = NULL; }
    return table;
}

/* Each entry of a directory's tree, in order; `out` NULL only sums sizes. */
static int walk(Image *image, const uint8_t *table, uint32_t size, uint32_t offset, int depth, const char *out);

static int copy_file(Image *image, uint32_t sector, uint32_t size, const char *path)
{
    static uint8_t buffer[1u << 20];
    uint64_t left = size, at = (uint64_t)sector * SECTOR;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    while (left) {
        DWORD chunk = (DWORD)(left < sizeof buffer ? left : sizeof buffer), wrote = 0;
        if (!image_read(image, at, buffer, chunk) || !WriteFile(file, buffer, chunk, &wrote, NULL) || wrote != chunk) {
            CloseHandle(file);
            return 0;
        }
        left -= chunk; at += chunk; image->done += chunk;
        progress_show(image, NULL);
    }
    CloseHandle(file);
    return 1;
}

static int walk(Image *image, const uint8_t *table, uint32_t size, uint32_t offset, int depth, const char *out)
{
    uint16_t left, right;
    uint32_t sector, bytes;
    uint8_t attributes, length;
    char name[256], path[MAX_PATH];
    size_t i;
    if (depth > 64 || offset + 14 > size) return 0;
    memcpy(&left, table + offset, 2);
    memcpy(&right, table + offset + 2, 2);
    if (left == 0xFFFF) return 1;                   /* padding: no entry */
    memcpy(&sector, table + offset + 4, 4);
    memcpy(&bytes, table + offset + 8, 4);
    attributes = table[offset + 12];
    length = table[offset + 13];
    if (offset + 14 + length > size || !length) return 0;
    memcpy(name, table + offset + 14, length);
    name[length] = 0;
    /* A name is one path part, nothing that climbs out of the folder. */
    for (i = 0; i < length; i++)
        if (name[i] == '\\' || name[i] == '/' || name[i] == ':' || (unsigned char)name[i] < 32) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    if (left && !walk(image, table, size, left * 4u, depth + 1, out)) return 0;
    if (out) snprintf(path, sizeof path, "%s\\%s", out, name);
    if (attributes & 0x10) {
        uint8_t *sub = read_table(image, sector, bytes);
        int ok = 1;
        if (out && !CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
        if (ok && sub) ok = walk(image, sub, bytes, 0, depth + 1, out ? path : NULL);
        free(sub);
        if (!ok) return 0;
    } else if (out) {
        if (!copy_file(image, sector, bytes, path)) return 0;
    } else {
        image->total += bytes;
    }
    if (right && !walk(image, table, size, right * 4u, depth + 1, out)) return 0;
    return 1;
}

static int delete_tree(const char *dir)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            snprintf(path, sizeof path, "%s\\%s", dir, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(path);
            else DeleteFileA(path);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    return RemoveDirectoryA(dir);
}

/* The image's files into `dir` (made here; through "<dir>.partial", so an
 * extraction cut short is never taken for a finished one). */
static int extract_image(const char *image_path, const char *dir)
{
    Image image;
    char partial[MAX_PATH], text[300];
    uint32_t root[2];
    uint8_t *table;
    ULARGE_INTEGER free_bytes;
    int ok;
    if (!image_open(&image, image_path)) {
        snprintf(text, sizeof text, "%s is not an Xbox disc image.", image_path);
        MessageBoxA(NULL, text, "Turok: Evolution", MB_ICONWARNING);
        return 0;
    }
    if (!image_read(&image, 32ull * SECTOR + 20, root, sizeof root) || !(table = read_table(&image, root[0], root[1]))) {
        CloseHandle(image.file);
        return 0;
    }
    ok = walk(&image, table, root[1], 0, 0, NULL);
    snprintf(partial, sizeof partial, "%s.partial", dir);
    if (ok && GetDiskFreeSpaceExA(s_exe_dir, &free_bytes, NULL, NULL) && free_bytes.QuadPart < image.total + (64ull << 20)) {
        snprintf(text, sizeof text, "Extracting the disc needs %.1f GB free beside the program; there is %.1f GB.",
                 image.total / 1e9, free_bytes.QuadPart / 1e9);
        MessageBoxA(NULL, text, "Turok: Evolution", MB_ICONWARNING);
        ok = 0;
    }
    if (ok) {
        delete_tree(partial);
        ok = CreateDirectoryA(partial, NULL) != 0;
    }
    if (ok) {
        progress_open(&image);
        snprintf(text, sizeof text, "Extracting the game's files from your disc image, once (%.1f GB)...\n%s",
                 image.total / 1e9, image_path);
        progress_show(&image, text);
        ok = walk(&image, table, root[1], 0, 0, partial);
        if (image.progress) DestroyWindow(image.progress);
    }
    free(table);
    CloseHandle(image.file);
    if (ok) ok = MoveFileA(partial, dir) != 0;
    if (!ok) {
        delete_tree(partial);
        snprintf(text, sizeof text, "The game's files could not be extracted from\n%s", image_path);
        MessageBoxA(NULL, text, "Turok: Evolution", MB_ICONERROR);
    }
    fprintf(stderr, "[GAME] %s %s into %s\n", ok ? "extracted" : "could not extract", image_path, dir);
    return ok;
}

/* A disc image beside the program, if there is one. */
static int image_beside(char *path, size_t size)
{
    static const char *const patterns[] = {"*.iso", "*.xiso"};
    WIN32_FIND_DATAA found;
    char pattern[MAX_PATH];
    size_t p;
    for (p = 0; p < 2; p++) {
        HANDLE search;
        snprintf(pattern, sizeof pattern, "%s\\%s", s_exe_dir, patterns[p]);
        search = FindFirstFileA(pattern, &found);
        if (search == INVALID_HANDLE_VALUE) continue;
        FindClose(search);
        snprintf(path, size, "%s\\%s", s_exe_dir, found.cFileName);
        return 1;
    }
    return 0;
}

/* The player's choice: a disc image or an extracted default.xbe. */
static int ask(char *path, size_t size)
{
    OPENFILENAMEA dialog;
    if (MessageBoxA(NULL, "Turok: Evolution needs your copy of the game (original Xbox, USA).\n\n"
                    "Choose its disc image (.iso or .xiso), which is extracted once beside this program, "
                    "or the default.xbe in a folder of its extracted files.",
                    "Turok: Evolution", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
        return 0;
    memset(&dialog, 0, sizeof dialog);
    path[0] = 0;
    dialog.lStructSize = sizeof dialog;
    dialog.lpstrFilter = "Turok: Evolution (disc image or default.xbe)\0*.iso;*.xiso;default.xbe\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = (DWORD)size;
    dialog.lpstrTitle = "Your copy of Turok: Evolution";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&dialog) != 0;
}

int game_files_locate(const char *build_dir, char *dir, size_t size)
{
    char candidate[MAX_PATH], chosen[MAX_PATH], *slash;
    const char *env = getenv("RECOMP_GAME_DIR");

    GetModuleFileNameA(NULL, s_exe_dir, sizeof s_exe_dir);
    if ((slash = strrchr(s_exe_dir, '\\')) != NULL) *slash = 0;
    snprintf(s_ini_path, sizeof s_ini_path, "%s\\turok_settings.ini", s_exe_dir);

    /* RECOMP_GAME_SETUP=1: as a new install, only what is beside the program. */
    int fresh = getenv("RECOMP_GAME_SETUP") && atoi(getenv("RECOMP_GAME_SETUP")) != 0;

    if (env && *env && usable_dir(env, 1)) { snprintf(dir, size, "%s", env); return 1; }
    GetPrivateProfileStringA("paths", "game_dir", "", candidate, sizeof candidate, s_ini_path);
    if (!fresh && *candidate && usable_dir(candidate, 0)) { snprintf(dir, size, "%s", candidate); return 1; }
    snprintf(candidate, sizeof candidate, "%s\\game", s_exe_dir);
    if (usable_dir(candidate, 1)) { snprintf(dir, size, "%s", candidate); return 1; }
    if (!fresh && build_dir && usable_dir(build_dir, 0)) { snprintf(dir, size, "%s", build_dir); return 1; }

    if (image_beside(chosen, sizeof chosen) || ask(chosen, sizeof chosen)) {
        for (;;) {
            size_t n = strlen(chosen);
            if (n > 12 && !_stricmp(chosen + n - 12, "\\default.xbe")) {
                chosen[n - 12] = 0;                 /* its folder */
                if (usable_dir(chosen, 1)) break;
            } else if (extract_image(chosen, candidate)) {
                if (usable_dir(candidate, 1)) {
                    snprintf(chosen, sizeof chosen, "%s", candidate);
                    break;
                }
                delete_tree(candidate);             /* another release's files */
            }
            if (!ask(chosen, sizeof chosen)) return 0;
        }
        WritePrivateProfileStringA("paths", "game_dir", chosen, s_ini_path);
        snprintf(dir, size, "%s", chosen);
        return 1;
    }
    return 0;
}
