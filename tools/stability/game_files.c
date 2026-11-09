#include <assert.h>
#include "../../src/game_files.c"

int main(void)
{
    Image image = {0};
    uint8_t table[32] = {0};
    uint32_t sector = 100, bytes = 2048;
    char temp[MAX_PATH], file[MAX_PATH], directory[MAX_PATH], child[MAX_PATH];
    GetTempPathA(sizeof temp, temp);
    assert(GetTempFileNameA(temp, "trk", 0, file));
    image.file = CreateFileA(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    assert(image.file != INVALID_HANDLE_VALUE);
    memcpy(table + 4, &sector, 4); memcpy(table + 8, &bytes, 4);
    table[12] = 0x10; table[13] = 5; memcpy(table + 14, "empty", 5);
    assert(!walk(&image, table, sizeof table, 0, 0, NULL));
    snprintf(directory, sizeof directory, "%s.dir", file);
    assert(CreateDirectoryA(directory, NULL));
    assert(!walk(&image, table, sizeof table, 0, 0, directory));
    snprintf(child, sizeof child, "%s\\empty", directory);
    assert(GetFileAttributesA(child) == INVALID_FILE_ATTRIBUTES);
    bytes = 0; memcpy(table + 8, &bytes, 4);
    assert(walk(&image, table, sizeof table, 0, 0, NULL));
    assert(walk(&image, table, sizeof table, 0, 0, directory));
    assert(GetFileAttributesA(child) & FILE_ATTRIBUTE_DIRECTORY);
    CloseHandle(image.file);
    assert(RemoveDirectoryA(child)); assert(RemoveDirectoryA(directory)); assert(DeleteFileA(file));
    puts("PASS: unreadable directories fail both extraction passes; empty directories succeed");
    return 0;
}
