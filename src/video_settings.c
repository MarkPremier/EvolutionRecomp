/**
 * Video Options: a page in the game's own Options menu, and the settings file
 * behind it.
 *
 * The page is game data, made at start-up from the player's own disc. Turok
 * mounts supertree0..5.tre and then looks for patch0.tre beside them, and a
 * file in that archive replaces the one in the supertrees. The menus are
 * WorldBuilder "levels" of widget actors, so the page is built the way the
 * console's other options pages are:
 *
 *   - Options.ati (the Options screen) gains a VIDEO OPTIONS entry, a copy of
 *     SOUND OPTIONS, in the gap the console version left between SOUND OPTIONS
 *     and SAVE OPTIONS (where the PC version kept a control-setup entry).
 *   - VideoOptions.ati is GameOptions.ati with its rows renamed: RESOLUTION,
 *     DISPLAY and VSYNC, each showing and adjusting one item of a new history
 *     file, Video.hst, registered in history.txt.
 *   - The strings go into Options.txm (keys 5 on): a text file the game has
 *     not heard of is never loaded.
 *
 * Options.hst itself is left alone: sub_000FF610 packs the Options and
 * ControlSetup history values into the save game's 256-byte options block in
 * that order, so a key added to Options.hst would shift every saved control
 * setting.
 *
 * A thread watches the Video.hst values in guest memory and applies a change
 * as the player makes it: the render scale (nv2a_gpu_set_render_scale), the
 * borderless window (Alt+Enter does the same, and the page follows it) and
 * VSync. turok_settings.ini beside the program keeps them between runs; an
 * environment variable set by hand (RECOMP_RENDER_SCALE, RECOMP_VSYNC) still
 * wins at start-up.
 */

#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "video_settings.h"

extern ptrdiff_t g_xbox_mem_offset;
extern void nv2a_gpu_set_render_scale(float scale);
extern void xbox_FramebufferSetFullscreen(int on);
extern int xbox_FramebufferFullscreen(void);
extern void xbox_SetPresentInterval(int interval);

/* ── Settings ─────────────────────────────────────────────────── */

#define RESOLUTION_STEPS 5      /* auto, then 1x .. 4x of 640x480 */

typedef struct VideoSettings {
    int resolution;             /* 0 auto (matched to the monitor), n: n x 640x480 */
    int fullscreen;             /* borderless, on the window's monitor */
    int vsync;
} VideoSettings;

static VideoSettings s_settings;
static char s_ini_path[MAX_PATH];

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static void settings_save(void)
{
    char value[16];
    if (s_settings.resolution) snprintf(value, sizeof value, "%d", s_settings.resolution);
    else strcpy(value, "auto");
    WritePrivateProfileStringA("video", "resolution", value, s_ini_path);
    WritePrivateProfileStringA("video", "fullscreen", s_settings.fullscreen ? "1" : "0", s_ini_path);
    WritePrivateProfileStringA("video", "vsync", s_settings.vsync ? "1" : "0", s_ini_path);
}

static int resolution_from_text(const char *text)
{
    if (!text || !*text || !_stricmp(text, "auto")) return 0;
    return clamp((int)(atof(text) + 0.5), 1, RESOLUTION_STEPS - 1);
}

void video_settings_load(void)
{
    char text[32], *slash;
    const char *env;

    GetModuleFileNameA(NULL, s_ini_path, sizeof s_ini_path);
    slash = strrchr(s_ini_path, '\\');
    snprintf(slash ? slash + 1 : s_ini_path, sizeof s_ini_path - (slash ? (size_t)(slash + 1 - s_ini_path) : 0),
             "turok_settings.ini");

    GetPrivateProfileStringA("video", "resolution", "auto", text, sizeof text, s_ini_path);
    s_settings.resolution = resolution_from_text(text);
    s_settings.fullscreen = GetPrivateProfileIntA("video", "fullscreen", 0, s_ini_path) != 0;
    s_settings.vsync = GetPrivateProfileIntA("video", "vsync", 0, s_ini_path) != 0;

    /* What the runtime reads at start-up, unless set by hand; set by hand, it
     * is also what the page shows. */
    env = getenv("RECOMP_RENDER_SCALE");
    if (env && *env) s_settings.resolution = resolution_from_text(env);
    else {
        if (s_settings.resolution) snprintf(text, sizeof text, "%d", s_settings.resolution);
        else strcpy(text, "auto");
        _putenv_s("RECOMP_RENDER_SCALE", text);
    }
    env = getenv("RECOMP_VSYNC");
    if (env && *env) s_settings.vsync = atoi(env) > 0;
    else _putenv_s("RECOMP_VSYNC", s_settings.vsync ? "1" : "0");

    fprintf(stderr, "[VIDEO] settings %s: resolution %d, fullscreen %d, vsync %d\n",
            s_ini_path, s_settings.resolution, s_settings.fullscreen, s_settings.vsync);
}

/* ── Archives ─────────────────────────────────────────────────── */

typedef struct Buffer {
    uint8_t *data;
    size_t size, capacity;
} Buffer;

static void buffer_put(Buffer *buffer, const void *data, size_t size)
{
    if (buffer->size + size > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
        while (capacity < buffer->size + size) capacity *= 2;
        buffer->data = (uint8_t *)realloc(buffer->data, capacity);
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static void buffer_text(Buffer *buffer, const char *text)
{
    buffer_put(buffer, text, strlen(text));
}

static uint32_t crc32_of(const void *data, size_t size)
{
    static uint32_t table[256];
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (!table[1])
        for (i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i;
            int bit;
            for (bit = 0; bit < 8; bit++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    for (i = 0; i < size; i++) crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* A file's key in a .tre: the CRC-32 of its path as "d:\data\...", lower case. */
static uint32_t tree_key(const char *path)
{
    char normal[MAX_PATH];
    size_t n = 0;
    normal[n++] = 'd'; normal[n++] = ':';
    if (*path != '\\' && *path != '/') normal[n++] = '\\';
    for (; *path && n + 1 < sizeof normal; path++)
        normal[n++] = *path == '/' ? '\\' : (char)(*path >= 'A' && *path <= 'Z' ? *path + 32 : *path);
    return crc32_of(normal, n);
}

/* A .tre: u32 count, then count x {offset, size, key, crc32 of the data},
 * sorted by key, then the data. */
typedef struct TreeEntry {
    uint32_t offset, size, key, crc;
    int tree;
} TreeEntry;

typedef struct Trees {
    FILE *files[16];
    TreeEntry *entries;
    size_t count;
} Trees;

static int trees_open(Trees *trees, const char *game_dir)
{
    int tree;
    memset(trees, 0, sizeof *trees);
    for (tree = 0; tree < 16; tree++) {
        char path[MAX_PATH];
        uint32_t count, index;
        FILE *file;
        snprintf(path, sizeof path, "%s\\supertree%d.tre", game_dir, tree);
        file = fopen(path, "rb");
        if (!file) break;
        trees->files[tree] = file;
        if (fread(&count, 4, 1, file) != 1 || count > 1000000) return 0;
        trees->entries = (TreeEntry *)realloc(trees->entries, (trees->count + count) * sizeof *trees->entries);
        for (index = 0; index < count; index++) {
            TreeEntry *entry = &trees->entries[trees->count + index];
            if (fread(entry, 16, 1, file) != 1) return 0;
            entry->tree = tree;
        }
        trees->count += count;
    }
    return trees->count != 0;
}

static void trees_close(Trees *trees)
{
    int tree;
    for (tree = 0; tree < 16; tree++)
        if (trees->files[tree]) fclose(trees->files[tree]);
    free(trees->entries);
}

/* The first supertree's copy of `path`, which is the one the game reads. */
static int trees_read(Trees *trees, const char *path, Buffer *out)
{
    uint32_t key = tree_key(path);
    size_t index;
    for (index = 0; index < trees->count; index++) {
        TreeEntry *entry = &trees->entries[index];
        FILE *file = trees->files[entry->tree];
        if (entry->key != key) continue;
        out->size = 0;
        buffer_put(out, "", 0);
        if (out->capacity < entry->size) {
            out->data = (uint8_t *)realloc(out->data, entry->size);
            out->capacity = entry->size;
        }
        if (_fseeki64(file, entry->offset, SEEK_SET) || fread(out->data, 1, entry->size, file) != entry->size)
            return 0;
        out->size = entry->size;
        return 1;
    }
    fprintf(stderr, "[VIDEO] %s is not in the game's archives\n", path);
    return 0;
}

typedef struct PatchFile {
    uint32_t key;
    Buffer data;
} PatchFile;

static int patch_file_order(const void *a, const void *b)
{
    uint32_t x = ((const PatchFile *)a)->key, y = ((const PatchFile *)b)->key;
    return x < y ? -1 : x > y;
}

static void tree_build(PatchFile *files, size_t count, Buffer *out)
{
    uint32_t offset = (uint32_t)(4 + 16 * count), n = (uint32_t)count;
    size_t index;
    qsort(files, count, sizeof *files, patch_file_order);
    buffer_put(out, &n, 4);
    for (index = 0; index < count; index++) {
        uint32_t record[4];
        record[0] = offset;
        record[1] = (uint32_t)files[index].data.size;
        record[2] = files[index].key;
        record[3] = crc32_of(files[index].data.data, files[index].data.size);
        buffer_put(out, record, sizeof record);
        offset += record[1];
    }
    for (index = 0; index < count; index++)
        buffer_put(out, files[index].data.data, files[index].data.size);
}

/* ── Screen files ─────────────────────────────────────────────────
 *
 * An .ati or .atr is a 4-byte header and a run of tagged elements:
 *   type     low 5 bits the kind (1 a list, 2 a string with children, others
 *            a plain value); 0x40 a 1-byte size, 0x80 a 2-byte size; 0x20 a
 *            u16 child count follows the size
 *   level    nesting depth
 *   size     payload bytes, after the name
 *   name     length byte, then the name and a NUL
 *   payload  children; a length byte, text, NUL and children; or the value */

typedef struct Node Node;
struct Node {
    uint8_t kind, flags, level;
    char name[24];
    uint8_t *value;             /* a value's bytes, or a kind-2 node's text (no NUL) */
    uint32_t value_size;
    uint16_t stored_count;      /* a plain value's child count, kept as read */
    Node **children;
    uint32_t count, capacity;
};

static Node *node_new(void)
{
    return (Node *)calloc(1, sizeof(Node));
}

static void node_free(Node *node)
{
    uint32_t i;
    if (!node) return;
    for (i = 0; i < node->count; i++) node_free(node->children[i]);
    free(node->children);
    free(node->value);
    free(node);
}

static void node_insert(Node *parent, uint32_t at, Node *child)
{
    if (parent->count == parent->capacity) {
        parent->capacity = parent->capacity ? parent->capacity * 2 : 8;
        parent->children = (Node **)realloc(parent->children, parent->capacity * sizeof(Node *));
    }
    memmove(parent->children + at + 1, parent->children + at, (parent->count - at) * sizeof(Node *));
    parent->children[at] = child;
    parent->count++;
}

static void node_remove(Node *parent, uint32_t at)
{
    node_free(parent->children[at]);
    memmove(parent->children + at, parent->children + at + 1, (parent->count - at - 1) * sizeof(Node *));
    parent->count--;
}

static void node_set(Node *node, const void *value, uint32_t size)
{
    free(node->value);
    node->value = (uint8_t *)malloc(size ? size : 1);
    memcpy(node->value, value, size);
    node->value_size = size;
}

static Node *node_clone(const Node *node)
{
    Node *copy = node_new();
    uint32_t i;
    *copy = *node;
    copy->value = NULL;
    copy->children = NULL;
    copy->count = copy->capacity = 0;
    node_set(copy, node->value, node->value_size);
    for (i = 0; i < node->count; i++) node_insert(copy, copy->count, node_clone(node->children[i]));
    return copy;
}

static int parse_nodes(const uint8_t *data, size_t pos, size_t end, Node *parent)
{
    while (pos < end) {
        Node *node;
        uint8_t type, name_size;
        size_t size;
        if (end - pos < 4) return 0;
        type = data[pos];
        node = node_new();
        node->kind = type & 0x1F;
        node->flags = type & 0xE0;
        node->level = data[pos + 1];
        pos += 2;
        if (type & 0x80) { size = (size_t)data[pos] | (size_t)data[pos + 1] << 8; pos += 2; }
        else if (type & 0x40) size = data[pos++];
        else { node_free(node); return 0; }
        if (type & 0x20) { node->stored_count = (uint16_t)(data[pos] | data[pos + 1] << 8); pos += 2; }
        name_size = data[pos++];
        if (name_size >= sizeof node->name || pos + name_size + 1 + size > end) { node_free(node); return 0; }
        memcpy(node->name, data + pos, name_size);
        pos += name_size + 1;
        node_insert(parent, parent->count, node);
        if (node->kind == 1) {
            if (!parse_nodes(data, pos, pos + size, node)) return 0;
        } else if (node->kind == 2) {
            uint8_t text = data[pos];
            if ((size_t)text + 2 > size) return 0;
            node_set(node, data + pos + 1, text);
            if (!parse_nodes(data, pos + 2 + text, pos + size, node)) return 0;
        } else {
            node_set(node, data + pos, (uint32_t)size);
        }
        pos += size;
    }
    return pos == end;
}

static void encode_node(const Node *node, Buffer *out)
{
    Buffer payload = {0};
    uint8_t head[8], type;
    size_t n = 0, name_size = strlen(node->name);
    uint32_t i;
    if (node->kind == 2) {
        uint8_t text = (uint8_t)node->value_size;
        buffer_put(&payload, &text, 1);
        buffer_put(&payload, node->value, node->value_size);
        buffer_put(&payload, "", 1);
    }
    if (node->kind == 1 || node->kind == 2)
        for (i = 0; i < node->count; i++) encode_node(node->children[i], &payload);
    else
        buffer_put(&payload, node->value, node->value_size);
    type = (uint8_t)(node->kind | (node->flags & 0x20));
    type |= payload.size < 256 && !(node->flags & 0x80) ? 0x40 : 0x80;
    head[n++] = type;
    head[n++] = node->level;
    head[n++] = (uint8_t)payload.size;
    if (type & 0x80) head[n++] = (uint8_t)(payload.size >> 8);
    if (type & 0x20) {
        uint16_t count = node->kind == 1 || node->kind == 2 ? (uint16_t)node->count : node->stored_count;
        head[n++] = (uint8_t)count;
        head[n++] = (uint8_t)(count >> 8);
    }
    head[n++] = (uint8_t)name_size;
    buffer_put(out, head, n);
    buffer_put(out, node->name, name_size + 1);
    buffer_put(out, payload.data, payload.size);
    free(payload.data);
}

static Node *screen_load(const Buffer *file)
{
    Node *root = node_new();
    if (file->size < 4 || !parse_nodes(file->data, 4, file->size, root)) {
        node_free(root);
        return NULL;
    }
    node_set(root, file->data, 4);     /* the header */
    return root;
}

static void screen_save(const Node *root, Buffer *out)
{
    uint32_t i;
    out->size = 0;
    buffer_put(out, root->value, 4);
    for (i = 0; i < root->count; i++) encode_node(root->children[i], out);
}

static Node *child(const Node *node, const char *name)
{
    uint32_t i;
    for (i = 0; node && i < node->count; i++)
        if (!strcmp(node->children[i]->name, name)) return node->children[i];
    return NULL;
}

/* A text value matches `text` (they end in a NUL). */
static int is_text(const Node *node, const char *text)
{
    return node && node->value_size == strlen(text) + 1 && !memcmp(node->value, text, node->value_size);
}

static void set_text(Node *node, const char *text)
{
    if (node) node_set(node, text, (uint32_t)strlen(text) + (node->kind == 2 ? 0 : 1));
}

static void set_byte(Node *node, int value)
{
    uint8_t byte = (uint8_t)value;
    if (node) node_set(node, &byte, 1);
}

static int get_vector(const Node *node, float vector[3])
{
    if (!node || node->value_size != 12) return 0;
    memcpy(vector, node->value, 12);
    return 1;
}

static void set_vector(Node *node, const float vector[3])
{
    if (node) node_set(node, vector, 12);
}

static Node *actor_named(const Node *root, const char *name, uint32_t *at)
{
    uint32_t i;
    for (i = 0; i < root->count; i++)
        if (is_text(child(root->children[i], "NAME"), name)) {
            if (at) *at = i;
            return root->children[i];
        }
    return NULL;
}

static const char *target_of(const Node *event, char *name, size_t size)
{
    const Node *target = child(event, "TARGETNAME");
    if (!target || !target->value_size) return NULL;
    snprintf(name, size, "%.*s", (int)target->value_size - 1, (const char *)target->value);
    return name;
}

#define OPTIONS_TXM   "$\\Data\\Text\\Options.txm"
#define FRONTEND_TXM  "$\\Data\\Text\\FrontEnd.txm"
#define OPTIONS_HST   "$\\Data\\History\\Options.hst"
#define VIDEO_HST     "$\\Data\\History\\Video.hst"
#define SCREENS       "data\\levels\\u_jcowlishaw\\screens\\"

/* Where the page goes: the front end's Options and the pause menu's, each
 * with its own Game Options page to build from (the pause menu's has the
 * darker panel and returns to the game). */
static const struct VideoMenu {
    const char *menu, *game_options, *page;
} k_menus[] = {
    {"options", "gameoptions", "VideoOptions"},
    {"ingameoptionsmenu", "ingameoptions", "InGameVideoOptions"},
};
#define MENU_COUNT (sizeof k_menus / sizeof *k_menus)

/* Options.txm keys from 5 on: the entry, the page's title, its rows and
 * their values. */
enum {
    TEXT_ENTRY = 5, TEXT_TITLE, TEXT_RESOLUTION, TEXT_DISPLAY, TEXT_VSYNC,
    TEXT_WINDOWED, TEXT_FULLSCREEN, TEXT_SCALES,
};
/* FrontEnd.txm's "off" ("on" follows it). */
#define TEXT_OFF 3

/* An Options screen: a VIDEO OPTIONS entry in the empty slot below SOUND
 * OPTIONS, opening the page `page`. */
static int add_video_entry(Node *screen, const char *page)
{
    char level[MAX_PATH];
    uint32_t at, i, j;
    Node *sound = actor_named(screen, "soundvideo", &at), *control = actor_named(screen, "control", NULL);
    Node *deity = actor_named(screen, "DeityNoFade", NULL), *video, *link;
    float position[3], above[3];
    int id = 0;
    if (!sound || !control || !deity || !get_vector(child(sound, "POS"), position) ||
            !get_vector(child(control, "POS"), above)) return 0;
    for (i = 0; i < screen->count; i++) {
        Node *number = child(screen->children[i], "ID");
        if (number && number->value_size == 1 && number->value[0] > id) id = number->value[0];
    }
    id++;
    video = node_clone(sound);
    set_byte(child(video, "ID"), id);
    set_text(child(video, "NAME"), "videooptions");
    /* One step below SOUND OPTIONS, as SOUND OPTIONS is below CONTROL SETUP. */
    position[1] -= above[1] - position[1];
    set_vector(child(video, "POS"), position);
    set_byte(child(child(video, "ACTOR_VARIABLES"), "W_TMN"), TEXT_ENTRY);
    link = child(video, "LINK");
    if (!link || !child(link, "EVENTS")) return 0;
    set_text(link, "videooptions selected");
    snprintf(level, sizeof level, "$/Data/Levels/U_JCowlishaw\\Screens\\%s\\%s.atr", page, page);
    for (i = 0; i < child(link, "EVENTS")->count; i++) {
        Node *event = child(link, "EVENTS")->children[i];
        if (event->value_size == 5 && !memcmp(event->value, "Level", 5)) set_text(child(event, "LN"), level);
    }
    node_insert(screen, at + 1, video);

    /* The screen's setup and fade events name every entry. */
    for (i = 0; i < deity->count; i++) {
        Node *events = child(deity->children[i], "EVENTS");
        for (j = 0; events && j < events->count; j++) {
            char name[64];
            const char *target = target_of(events->children[j], name, sizeof name);
            if (target && !strcmp(target, "soundvideo")) {
                Node *copy = node_clone(events->children[j]);
                set_text(child(copy, "TARGETNAME"), "videooptions");
                set_byte(child(copy, "TARGETID"), id);
                node_insert(events, ++j, copy);
            }
        }
    }
    return 1;
}

/* Game Options' rows, reused: the row, its value box, their new names, the
 * Video.hst item, its last value, and the text file, first value text and
 * label. */
static const struct VideoRow {
    const char *row, *value, *new_row, *new_value;
    int item, last;
    const char *text;
    int first, label;
} k_rows[] = {
    {"aim", "autoaimValueWidget", "resolution", "resolutionValueWidget", 1, RESOLUTION_STEPS - 1,
     OPTIONS_TXM, TEXT_SCALES, TEXT_RESOLUTION},
    {"hud", "hudValueWidget", "display", "displayValueWidget", 2, 1, OPTIONS_TXM, TEXT_WINDOWED, TEXT_DISPLAY},
    {"crosshair", "crosshairValueWidget", "vsync", "vsyncValueWidget", 3, 1, FRONTEND_TXM, TEXT_OFF, TEXT_VSYNC},
};
static const char *const k_dropped[] = {"violence", "language", "violenceValueWidget", "languageValueWidget"};

/* The rows are wider and their text smaller than Game Options': a value such
 * as 1920x1440 does not fit beside its label at the console's size. */
#define ROW_WIDTH  31.0f
#define TEXT_SCALE 0.75f

static int dropped(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_dropped / sizeof *k_dropped; i++)
        if (!strcmp(name, k_dropped[i])) return 1;
    return 0;
}

static const char *renamed(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof k_rows / sizeof *k_rows; i++) {
        if (!strcmp(name, k_rows[i].row)) return k_rows[i].new_row;
        if (!strcmp(name, k_rows[i].value)) return k_rows[i].new_value;
    }
    return NULL;
}

static int make_video_screen(Node *screen)
{
    Node *title = actor_named(screen, "title", NULL), *bbutton = actor_named(screen, "BButton", NULL), *deity;
    uint32_t i, j, k;
    size_t r;
    char name[64];

    for (r = 0; r < sizeof k_rows / sizeof *k_rows; r++) {
        const struct VideoRow *row = &k_rows[r];
        Node *label = actor_named(screen, row->row, NULL), *value = actor_named(screen, row->value, NULL);
        Node *label_vars = child(label, "ACTOR_VARIABLES"), *value_vars = child(value, "ACTOR_VARIABLES");
        float label_scale[3], value_position[3], value_scale[3];
        if (!label_vars || !value_vars || !get_vector(child(label, "SCALE"), label_scale) ||
                !get_vector(child(value, "POS"), value_position) || !get_vector(child(value, "SCALE"), value_scale))
            return 0;
        value_position[0] += (ROW_WIDTH - label_scale[0]) / 2;
        label_scale[0] = ROW_WIDTH;
        label_scale[1] *= TEXT_SCALE;
        value_scale[1] *= TEXT_SCALE;
        set_vector(child(label, "SCALE"), label_scale);
        set_vector(child(value, "POS"), value_position);
        set_vector(child(value, "SCALE"), value_scale);
        set_text(child(label_vars, "W_TMF"), OPTIONS_TXM);
        set_byte(child(label_vars, "W_TMN"), row->label);
        set_text(child(value_vars, "W_TMF"), row->text);
        set_byte(child(value_vars, "W_TMN"), row->first);
        set_text(child(value_vars, "W_THF"), VIDEO_HST);
        set_byte(child(value_vars, "W_THI"), row->item);
        set_text(child(value_vars, "A_HIF"), VIDEO_HST);
        set_byte(child(value_vars, "A_HII"), row->item);
    }
    if (!title || !bbutton) return 0;
    set_text(child(child(title, "ACTOR_VARIABLES"), "W_TMF"), OPTIONS_TXM);
    set_byte(child(child(title, "ACTOR_VARIABLES"), "W_TMN"), TEXT_TITLE);

    /* Violence and language go; so do the back button's moves for languages
     * with longer words. */
    for (i = screen->count; i-- > 0;) {
        Node *actor_name = child(screen->children[i], "NAME");
        if (actor_name && actor_name->value_size &&
                dropped((const char *)actor_name->value)) node_remove(screen, i);
    }
    for (i = bbutton->count; i-- > 0;)
        if (!strcmp(bbutton->children[i]->name, "LINK")) node_remove(bbutton, i);

    deity = actor_named(screen, "DeityNoFade", NULL);
    if (!deity) return 0;
    for (i = deity->count; i-- > 0;) {
        Node *link = deity->children[i], *cause = child(link, "CAUSE"), *events = child(link, "EVENTS");
        if (strcmp(link->name, "LINK")) continue;
        /* "aim > 1" and the like keep a row's value in range. */
        if (cause && cause->value_size == 7 && !memcmp(cause->value, "History", 7) &&
                is_text(child(cause, "HSTF"), OPTIONS_HST)) {
            Node *item = child(cause, "ITM");
            const struct VideoRow *row = NULL;
            char text[48];
            for (r = 0; item && item->value_size == 1 && r < sizeof k_rows / sizeof *k_rows; r++)
                if (k_rows[r].item == item->value[0]) row = &k_rows[r];
            if (!row) { node_remove(deity, i); continue; }
            snprintf(text, sizeof text, "%s > %d", row->new_row, row->last);
            set_text(link, text);
            set_text(child(cause, "HSTF"), VIDEO_HST);
            set_byte(child(cause, "ITM"), row->item);
            set_byte(child(cause, "VAL"), row->last);
            for (j = 0; events && j < events->count; j++)
                if (events->children[j]->value_size == 4 && !memcmp(events->children[j]->value, "Hist", 4)) {
                    set_text(child(events->children[j], "HSTF"), VIDEO_HST);
                    set_byte(child(events->children[j], "ITM"), row->item);
                    set_byte(child(events->children[j], "VAL"), row->last);
                }
            continue;
        }
        /* The language versions' links hide violence or language. */
        snprintf(name, sizeof name, "%.*s", (int)link->value_size, (const char *)link->value);
        if (strstr(name, "version") || strstr(name, "Violence")) { node_remove(deity, i); continue; }
        for (j = events ? events->count : 0; j-- > 0;) {
            const char *target = target_of(events->children[j], name, sizeof name);
            if (target && dropped(target)) node_remove(events, j);
        }
    }

    for (i = 0; i < screen->count; i++) {
        Node *actor = screen->children[i], *actor_name = child(actor, "NAME");
        for (j = 0; j < actor->count; j++) {
            Node *events = child(actor->children[j], "EVENTS");
            if (strcmp(actor->children[j]->name, "LINK")) continue;
            for (k = 0; events && k < events->count; k++) {
                const char *target = target_of(events->children[k], name, sizeof name);
                if (target && renamed(target)) set_text(child(events->children[k], "TARGETNAME"), renamed(target));
            }
        }
        if (actor_name && actor_name->value_size && renamed((const char *)actor_name->value))
            set_text(actor_name, renamed((const char *)actor_name->value));
    }
    return 1;
}

/* The page's .atr: the template's, naming the page's own actors (its mesh,
 * the stone panel, stays the template's). */
static int make_video_descriptor(Node *descriptor, const char *page)
{
    Node *actor = descriptor->count ? descriptor->children[0] : NULL;
    char text[MAX_PATH], upper[64];
    size_t i;
    if (!actor || !child(actor, "ACTORINSTANCES")) return 0;
    snprintf(text, sizeof text, "y:\\data\\levels\\u_jcowlishaw\\screens\\%s\\%s.atr", page, page);
    for (i = 0; text[i]; i++) text[i] = (char)tolower((unsigned char)text[i]);
    set_text(actor, text);
    for (i = 0; page[i] && i + 1 < sizeof upper; i++) upper[i] = (char)toupper((unsigned char)page[i]);
    upper[i] = 0;
    snprintf(text, sizeof text, "$\\DATA\\LEVELS\\U_JCOWLISHAW\\SCREENS\\%s\\%s.ati", upper, page);
    set_text(child(actor, "ACTORINSTANCES"), text);
    return 1;
}

static void add_text(Buffer *text, int key, const char *value)
{
    char line[96];
    snprintf(line, sizeof line, "*key = %d\r\n*value = \"%s\"\r\n", key, value);
    buffer_text(text, line);
}

/* Video.hst, each item starting at the saved setting. */
static void make_history(Buffer *out)
{
    char line[96];
    buffer_text(out, ";------------------------------------------------------------------------------\r\n"
                     ";  Video options (recompilation)\r\n"
                     ";------------------------------------------------------------------------------\r\n\r\n"
                     "*Key   = 0\r\n*Value = \"All\"\r\n\r\n");
    snprintf(line, sizeof line, "*Key   = 1\r\n*Value = \"Resolution <%d>\"\r\n\r\n", s_settings.resolution);
    buffer_text(out, line);
    snprintf(line, sizeof line, "*Key   = 2\r\n*Value = \"Display Mode <%d>\"\r\n\r\n", s_settings.fullscreen);
    buffer_text(out, line);
    snprintf(line, sizeof line, "*Key   = 3\r\n*Value = \"VSync <%d>\"\r\n\r\n", s_settings.vsync);
    buffer_text(out, line);
}

#define PATCH_MARKER "data\\recomp\\videooptions.txt"

/* Whether patch0.tre is absent or one this file wrote: someone else's is left
 * alone. */
static int patch_is_ours(const char *path)
{
    FILE *file = fopen(path, "rb");
    uint32_t count, index, record[4], marker = tree_key(PATCH_MARKER);
    int ours = 0;
    if (!file) return 1;
    if (fread(&count, 4, 1, file) == 1 && count < 4096)
        for (index = 0; index < count && fread(record, 16, 1, file) == 1; index++)
            if (record[2] == marker) ours = 1;
    fclose(file);
    return ours;
}

/* One screen file from the archives, changed by `edit`, as patch file `out`. */
static int patch_screen(Trees *trees, const char *from, const char *to, PatchFile *out,
                        int (*edit)(Node *, const char *), const char *argument)
{
    Node *screen;
    out->key = tree_key(to);
    if (!trees_read(trees, from, &out->data)) return 0;
    screen = screen_load(&out->data);
    if (!screen || !edit(screen, argument)) {
        fprintf(stderr, "[VIDEO] %s is not as expected; Video Options left out\n", from);
        node_free(screen);
        return 0;
    }
    screen_save(screen, &out->data);
    node_free(screen);
    return 1;
}

static int edit_video_screen(Node *screen, const char *unused)
{
    (void)unused;
    return make_video_screen(screen);
}

static int write_patch(const char *game_dir)
{
    /* Per menu: its Options screen, the page and the page's .atr; then the
     * strings, the history file, history.txt and the marker. */
    enum { OPTIONS_TEXT = 3 * MENU_COUNT, VIDEO_HISTORY, HISTORY_LIST, MARKER, FILE_COUNT };
    static const char *const scales[RESOLUTION_STEPS] = {"auto", "640x480", "1280x960", "1920x1440", "2560x1920"};
    PatchFile files[FILE_COUNT];
    Trees trees;
    Buffer tree = {0}, existing = {0};
    char path[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    int ok = 0, i;
    size_t m;
    FILE *file;

    memset(files, 0, sizeof files);
    snprintf(path, sizeof path, "%s\\patch0.tre", game_dir);
    if (!patch_is_ours(path)) {
        fprintf(stderr, "[VIDEO] %s is not ours; Video Options left out\n", path);
        return 0;
    }
    /* RECOMP_VIDEO_OPTIONS=0: the menus as the disc has them. */
    if (getenv("RECOMP_VIDEO_OPTIONS") && !strcmp(getenv("RECOMP_VIDEO_OPTIONS"), "0")) {
        DeleteFileA(path);
        return 0;
    }
    if (!trees_open(&trees, game_dir)) {
        fprintf(stderr, "[VIDEO] could not read the game's archives in %s\n", game_dir);
        trees_close(&trees);
        return 0;
    }

    for (m = 0; m < MENU_COUNT; m++) {
        const struct VideoMenu *menu = &k_menus[m];
        PatchFile *out = &files[3 * m];
        snprintf(from, sizeof from, SCREENS "%s\\%s.ati", menu->menu, menu->menu);
        if (!patch_screen(&trees, from, from, &out[0], add_video_entry, menu->page)) goto done;
        snprintf(from, sizeof from, SCREENS "%s\\%s.ati", menu->game_options, menu->game_options);
        snprintf(to, sizeof to, SCREENS "%s\\%s.ati", menu->page, menu->page);
        if (!patch_screen(&trees, from, to, &out[1], edit_video_screen, NULL)) goto done;
        snprintf(from, sizeof from, SCREENS "%s\\%s.atr", menu->game_options, menu->game_options);
        snprintf(to, sizeof to, SCREENS "%s\\%s.atr", menu->page, menu->page);
        if (!patch_screen(&trees, from, to, &out[2], make_video_descriptor, menu->page)) goto done;
    }

    files[OPTIONS_TEXT].key = tree_key("data\\text\\options.txm");
    files[VIDEO_HISTORY].key = tree_key("data\\history\\video.hst");
    files[HISTORY_LIST].key = tree_key("data\\history\\history.txt");
    files[MARKER].key = tree_key(PATCH_MARKER);
    if (!trees_read(&trees, "data\\text\\options.txm", &files[OPTIONS_TEXT].data)) goto done;
    add_text(&files[OPTIONS_TEXT].data, TEXT_ENTRY, "VIDEO OPTIONS");
    add_text(&files[OPTIONS_TEXT].data, TEXT_TITLE, "Video Options");
    add_text(&files[OPTIONS_TEXT].data, TEXT_RESOLUTION, "resolution");
    add_text(&files[OPTIONS_TEXT].data, TEXT_DISPLAY, "display");
    add_text(&files[OPTIONS_TEXT].data, TEXT_VSYNC, "vsync");
    add_text(&files[OPTIONS_TEXT].data, TEXT_WINDOWED, "windowed");
    add_text(&files[OPTIONS_TEXT].data, TEXT_FULLSCREEN, "fullscreen");
    for (i = 0; i < RESOLUTION_STEPS; i++) add_text(&files[OPTIONS_TEXT].data, TEXT_SCALES + i, scales[i]);

    if (!trees_read(&trees, "data\\history\\history.txt", &files[HISTORY_LIST].data)) goto done;
    while (files[HISTORY_LIST].data.size &&
           (files[HISTORY_LIST].data.data[files[HISTORY_LIST].data.size - 1] == '\r' ||
            files[HISTORY_LIST].data.data[files[HISTORY_LIST].data.size - 1] == '\n'))
        files[HISTORY_LIST].data.size--;
    buffer_text(&files[HISTORY_LIST].data, "\r\n" VIDEO_HST "\r\n");

    make_history(&files[VIDEO_HISTORY].data);
    buffer_text(&files[MARKER].data, "Video Options for the Turok: Evolution recompilation\r\n");

    tree_build(files, FILE_COUNT, &tree);
    /* Rewritten only when it changed (a setting, or this code). */
    file = fopen(path, "rb");
    if (file) {
        existing.data = (uint8_t *)malloc(tree.size + 1);
        existing.size = fread(existing.data, 1, tree.size + 1, file);
        fclose(file);
    }
    if (existing.size == tree.size && !memcmp(existing.data, tree.data, tree.size)) ok = 1;
    else if ((file = fopen(path, "wb")) != NULL) {
        ok = fwrite(tree.data, 1, tree.size, file) == tree.size;
        ok = !fclose(file) && ok;
        fprintf(stderr, "[VIDEO] %s %s (%zu bytes)\n", ok ? "wrote" : "could not write", path, tree.size);
    }
done:
    for (i = 0; i < FILE_COUNT; i++) free(files[i].data.data);
    free(tree.data);
    free(existing.data);
    trees_close(&trees);
    return ok;
}

/* ── The page, live ───────────────────────────────────────────── */

/* Guest memory, read and written from this thread while the game runs. A
 * pointer the game has not set up yet faults rather than reading garbage. */
static int guest_read(uint32_t va, uint32_t *value)
{
    if (va < 0x10000u) return 0;
    __try {
        *value = *(volatile uint32_t *)((uintptr_t)va + g_xbox_mem_offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 1;
}

static int guest_name_is(uint32_t va, const char *suffix)
{
    char name[64];
    size_t n = 0, s = strlen(suffix);
    if (va < 0x10000u) return 0;
    __try {
        const char *text = (const char *)((uintptr_t)va + g_xbox_mem_offset);
        while (n + 1 < sizeof name && text[n]) { name[n] = text[n]; n++; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    name[n] = 0;
    return n >= s && !_stricmp(name + n - s, suffix);
}

/* The Video.hst items' values (8 bytes an item, the value first, from key 1:
 * key 0, "All", is not one), found through the history manager
 * [[0x250A84] + 0x80]: a vector of groups, each with its items at +4..+8 and
 * its file name at +0x14 (sub_00060680). */
static uint32_t video_items(void)
{
    uint32_t game, manager, first, last, at;
    if (!guest_read(0x250A84u, &game) || !guest_read(game + 0x80u, &manager) ||
            !guest_read(manager, &first) || !guest_read(manager + 4u, &last) ||
            last < first || last - first > 1024u) return 0;
    for (at = first; at < last; at += 4) {
        uint32_t group, name, begin, end;
        if (!guest_read(at, &group) || !guest_read(group + 0x14u, &name) || !guest_name_is(name, "video.hst"))
            continue;
        if (guest_read(group + 4u, &begin) && guest_read(group + 8u, &end) && end >= begin + 3u * 8u)
            return begin;
    }
    return 0;
}

static void item_write(uint32_t items, int item, int value)
{
    __try {
        *(volatile int32_t *)((uintptr_t)(items + 8u * (uint32_t)(item - 1)) + g_xbox_mem_offset) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void apply_resolution(void)
{
    nv2a_gpu_set_render_scale((float)s_settings.resolution);
}

static DWORD WINAPI video_watch(LPVOID unused)
{
    uint32_t seen = 0;
    int window_fullscreen = 0;
    (void)unused;
    for (;;) {
        uint32_t items, values[4];
        int item, changed = 0, now_fullscreen;
        Sleep(100);

        /* Alt+Enter: the page follows the window. */
        now_fullscreen = xbox_FramebufferFullscreen();
        if (now_fullscreen != window_fullscreen) {
            window_fullscreen = now_fullscreen;
            if (now_fullscreen != s_settings.fullscreen) {
                s_settings.fullscreen = now_fullscreen;
                changed = 1;
                if (seen) item_write(seen, 2, now_fullscreen);
            }
        }

        items = video_items();
        if (items != seen && items) {
            fprintf(stderr, "[VIDEO] options page values at 0x%08X\n", items);
            /* The history starts at the saved settings; one changed since
             * (Alt+Enter) is put back on the page. */
            item_write(items, 1, s_settings.resolution);
            item_write(items, 2, s_settings.fullscreen);
            item_write(items, 3, s_settings.vsync);
        }
        seen = items;
        if (items) {
            for (item = 1; item <= 3; item++)
                if (!guest_read(items + 8u * (uint32_t)(item - 1), &values[item])) break;
            if (item > 3) {
                int resolution = clamp((int)values[1], 0, RESOLUTION_STEPS - 1);
                int fullscreen = values[2] != 0, vsync = values[3] != 0;
                if (resolution != s_settings.resolution) {
                    s_settings.resolution = resolution;
                    apply_resolution();
                    changed = 1;
                }
                if (fullscreen != s_settings.fullscreen) {
                    s_settings.fullscreen = fullscreen;
                    xbox_FramebufferSetFullscreen(fullscreen);
                    changed = 1;
                }
                if (vsync != s_settings.vsync) {
                    s_settings.vsync = vsync;
                    xbox_SetPresentInterval(vsync);
                    changed = 1;
                }
                if ((int)values[1] != resolution) item_write(items, 1, resolution);
            }
        }
        if (changed) {
            fprintf(stderr, "[VIDEO] resolution %d, fullscreen %d, vsync %d\n",
                    s_settings.resolution, s_settings.fullscreen, s_settings.vsync);
            settings_save();
        }
    }
    return 0;
}

void video_settings_start(const char *game_dir)
{
    HANDLE thread;
    write_patch(game_dir);
    if (s_settings.fullscreen) xbox_FramebufferSetFullscreen(1);
    thread = CreateThread(NULL, 0, video_watch, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}
