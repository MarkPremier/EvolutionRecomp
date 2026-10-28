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
#include <math.h>
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
extern void nv2a_gpu_set_wide_factor(float factor);
extern void nv2a_gpu_set_projection_registers(const int *registers, int count);
extern void xbox_Nv2aSetVblankRate(double hz);
extern void xbox_SetFlipUnlimited(int on);
extern void xbox_FramebufferSetWindowAspect(float aspect);
extern void xbox_KbmSetRawMouse(int on);
extern int xbox_FramebufferMouseDelta(int *dx, int *dy);
extern void nv2a_gpu_replace_texture(uint64_t hash, const uint32_t *bgra, uint32_t width, uint32_t height);
extern void nv2a_gpu_set_texture_replacements(int on);
extern void nv2a_gpu_set_anisotropy(int level);
extern void nv2a_gpu_set_msaa(int samples);

/* ── Settings ─────────────────────────────────────────────────── */

/* The page's rows, in order; row n is Video.hst item n + 1. Each steps
 * through its values (shown, and written in turok_settings.ini, as their
 * text). */
enum { SET_RESOLUTION, SET_ASPECT, SET_DISPLAY, SET_VSYNC, SET_FRAMES, SET_ANISOTROPY, SET_MSAA, SET_DRAW_DISTANCE,
       SET_ICONS,
       SET_MOUSE_LOOK, SET_MOUSE_SPEED, SETTING_COUNT };
#define VIDEO_ROWS SETTING_COUNT        /* the page's rows: every setting */

typedef struct VideoSetting {
    const char *key, *label;
    const char *const *values;
    int count, fallback;
} VideoSetting;

static const char *const k_resolutions[] = {"auto", "480p", "960p", "1440p", "1920p", "2160p"};
/* Each as the render scale: lines over the console's 480 (0 automatic). */
static const float k_resolution_scales[] = {0, 1, 2, 3, 4, 4.5f};
static const char *const k_aspects[] = {"4:3", "16:10", "16:9", "21:9", "auto"};
static const char *const k_displays[] = {"windowed", "fullscreen"};
static const char *const k_switches[] = {"off", "on"};
static const char *const k_frames[] = {"30", "60", "120", "144", "240", "display", "off"};
static const char *const k_anisotropy[] = {"off", "2x", "4x", "8x", "16x"};
static const char *const k_msaa[] = {"off", "2x", "4x", "8x"};
static const char *const k_distances[] = {"normal", "2x", "4x", "8x"};
static const char *const k_icons[] = {"xbox", "pc"};
static const char *const k_mouse_looks[] = {"stick", "raw"};
static const char *const k_speeds[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
#define COUNT(array) ((int)(sizeof array / sizeof *array))

static const VideoSetting k_settings[SETTING_COUNT] = {
    {"resolution", "resolution", k_resolutions, COUNT(k_resolutions), 0},
    {"aspect", "aspect ratio", k_aspects, COUNT(k_aspects), 0},
    {"display", "display", k_displays, COUNT(k_displays), 0},
    {"vsync", "vsync", k_switches, COUNT(k_switches), 0},
    {"frame_limit", "frame limit", k_frames, COUNT(k_frames), 1},
    {"anisotropy", "anisotropy", k_anisotropy, COUNT(k_anisotropy), 4},
    {"msaa", "msaa", k_msaa, COUNT(k_msaa), 0},
    {"draw_distance", "draw distance", k_distances, COUNT(k_distances), 1},
    {"button_icons", "button icons", k_icons, COUNT(k_icons), 0},
    {"mouse_look", "mouse look", k_mouse_looks, COUNT(k_mouse_looks), 1},
    {"mouse_speed", "mouse speed", k_speeds, COUNT(k_speeds), 4},
};

static int s_values[SETTING_COUNT];
static char s_ini_path[MAX_PATH];
static volatile int s_front_end = 1;     /* the title is in its 4:3 front end */
static volatile int s_pause_menu;        /* on the pause menu's first screen */
static volatile int s_cinema;            /* an in-engine cutscene is playing */
static volatile int s_in_menu = 1;       /* FrontEnd.hst's FE Flag: not in play */

static int clamp(int value, int low, int high)
{
    return value < low ? low : value > high ? high : value;
}

static void settings_save(void)
{
    int s;
    for (s = 0; s < SETTING_COUNT; s++)
        WritePrivateProfileStringA("video", k_settings[s].key, k_settings[s].values[s_values[s]], s_ini_path);
}

/* A value's index from its text; -1 if it is none of them. */
static int value_index(int setting, const char *text)
{
    int v;
    for (v = 0; text && v < k_settings[setting].count; v++)
        if (!_stricmp(text, k_settings[setting].values[v])) return v;
    return -1;
}

/* RECOMP_RENDER_SCALE's value as the resolution row's (a factor; 1 is 480p):
 * the nearest. */
static int resolution_from_scale(const char *text)
{
    int r, nearest = 1;
    double scale;
    if (!text || !*text || !_stricmp(text, "auto")) return 0;
    scale = atof(text);
    for (r = 2; r < COUNT(k_resolution_scales); r++)
        if (fabs(k_resolution_scales[r] - scale) < fabs(k_resolution_scales[nearest] - scale)) nearest = r;
    return nearest;
}

void video_settings_load(void)
{
    char text[32], *slash;
    const char *env;
    int s;

    GetModuleFileNameA(NULL, s_ini_path, sizeof s_ini_path);
    slash = strrchr(s_ini_path, '\\');
    snprintf(slash ? slash + 1 : s_ini_path, sizeof s_ini_path - (slash ? (size_t)(slash + 1 - s_ini_path) : 0),
             "turok_settings.ini");

    for (s = 0; s < SETTING_COUNT; s++) {
        GetPrivateProfileStringA("video", k_settings[s].key, "", text, sizeof text, s_ini_path);
        s_values[s] = value_index(s, text);
        /* Files from before the values were words. */
        if (s_values[s] < 0 && *text && s == SET_RESOLUTION) s_values[s] = resolution_from_scale(text);
        if (s_values[s] < 0 && *text && (s == SET_DISPLAY || s == SET_VSYNC)) s_values[s] = atoi(text) != 0;
        if (s_values[s] < 0) s_values[s] = k_settings[s].fallback;
    }

    /* What the runtime reads at start-up, unless set by hand; set by hand, it
     * is also what the page shows. */
    env = getenv("RECOMP_RENDER_SCALE");
    if (env && *env) s_values[SET_RESOLUTION] = resolution_from_scale(env);
    else {
        if (s_values[SET_RESOLUTION]) snprintf(text, sizeof text, "%g", k_resolution_scales[s_values[SET_RESOLUTION]]);
        else strcpy(text, "auto");
        _putenv_s("RECOMP_RENDER_SCALE", text);
    }
    env = getenv("RECOMP_VSYNC");
    if (env && *env) s_values[SET_VSYNC] = atoi(env) > 0;
    else _putenv_s("RECOMP_VSYNC", s_values[SET_VSYNC] ? "1" : "0");

    fprintf(stderr, "[VIDEO] settings %s:", s_ini_path);
    for (s = 0; s < SETTING_COUNT; s++)
        fprintf(stderr, " %s %s", k_settings[s].key, k_settings[s].values[s_values[s]]);
    fprintf(stderr, "\n");
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
 *            a plain value); 0x40 a 1-byte size, 0x80 a 2-byte size, both a
 *            4-byte size (an actor of over 64 KB); 0x20 a u16 child count
 *            follows the size
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
        if ((type & 0xC0) == 0xC0) {
            size = (size_t)data[pos] | (size_t)data[pos + 1] << 8 | (size_t)data[pos + 2] << 16 |
                   (size_t)data[pos + 3] << 24;
            pos += 4;
        } else if (type & 0x80) { size = (size_t)data[pos] | (size_t)data[pos + 1] << 8; pos += 2; }
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
    uint8_t head[10], type;
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
    if (payload.size < 256 && !(node->flags & 0x80)) type |= 0x40;
    else if (payload.size > 0xFFFF || (node->flags & 0xC0) == 0xC0) type |= 0xC0;
    else type |= 0x80;
    head[n++] = type;
    head[n++] = node->level;
    head[n++] = (uint8_t)payload.size;
    if (type & 0x80) head[n++] = (uint8_t)(payload.size >> 8);
    if ((type & 0xC0) == 0xC0) {
        head[n++] = (uint8_t)(payload.size >> 16);
        head[n++] = (uint8_t)(payload.size >> 24);
    }
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

/* Options.txm keys from 5 on: the entry, the page's title, then each row's
 * label followed by its values. */
#define TEXT_ENTRY 5
#define TEXT_TITLE 6

static int text_label(int setting)
{
    int key = TEXT_TITLE + 1, s;
    for (s = 0; s < setting; s++) key += 1 + k_settings[s].count;
    return key;
}

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

/* Game Options' rows: the first is the pattern for every row of the page. */
static const char *const k_template_rows[] = {"aim", "hud", "crosshair", "violence", "language"};
static const char *const k_template_values[] = {"autoaimValueWidget", "hudValueWidget", "crosshairValueWidget",
                                                "violenceValueWidget", "languageValueWidget"};

/* The rows are wider, closer and their text smaller than Game Options': a
 * value such as fullscreen does not fit beside its label at the console's
 * size, and the page has eleven rows where Game Options has five. */
#define ROW_WIDTH  31.0f
#define TEXT_SCALE 0.75f
#define ROW_STEP   2.55f
#define ROW_RAISE  3.5f      /* the first row, above Game Options' */

static int is_template(const char *name)
{
    int t;
    for (t = 0; t < COUNT(k_template_rows); t++)
        if (!strcmp(name, k_template_rows[t]) || !strcmp(name, k_template_values[t])) return 1;
    return 0;
}

static int actor_id(const Node *actor)
{
    const Node *id = child(actor, "ID");
    return id && id->value_size == 1 ? id->value[0] : -1;
}

static void retarget(Node *event, const char *name, int id)
{
    set_text(child(event, "TARGETNAME"), name);
    set_byte(child(event, "TARGETID"), id);
}

static int make_video_screen(Node *screen)
{
    Node *row = actor_named(screen, k_template_rows[0], NULL), *value = actor_named(screen, k_template_values[0], NULL);
    Node *title = actor_named(screen, "title", NULL), *bbutton = actor_named(screen, "BButton", NULL);
    Node *deity = actor_named(screen, "DeityNoFade", NULL), *limit = NULL;
    float row_position[3], row_scale[3], value_position[3], value_scale[3];
    char name[64], row_names[VIDEO_ROWS][32], value_names[VIDEO_ROWS][48];
    int row_ids[VIDEO_ROWS], value_ids[VIDEO_ROWS], id = 0, s;
    uint32_t i, j, at = 0;

    if (!row || !value || !title || !bbutton || !deity || !get_vector(child(row, "POS"), row_position) ||
            !get_vector(child(row, "SCALE"), row_scale) || !get_vector(child(value, "POS"), value_position) ||
            !get_vector(child(value, "SCALE"), value_scale) || !child(row, "ACTOR_VARIABLES") ||
            !child(value, "ACTOR_VARIABLES")) return 0;
    for (i = 0; i < screen->count; i++)
        if (actor_id(screen->children[i]) > id) id = actor_id(screen->children[i]);
    row = node_clone(row);
    value = node_clone(value);

    /* The template's rows make way for the page's, where they were. */
    for (i = screen->count; i-- > 0;) {
        Node *actor_name = child(screen->children[i], "NAME");
        if (actor_name && actor_name->value_size && is_template((const char *)actor_name->value)) {
            node_remove(screen, i);
            at = i;
        }
    }
    for (s = 0; s < VIDEO_ROWS; s++) {
        Node *label = node_clone(row), *box = node_clone(value);
        Node *label_vars = child(label, "ACTOR_VARIABLES"), *box_vars = child(box, "ACTOR_VARIABLES");
        float position[3], scale[3];
        snprintf(row_names[s], sizeof row_names[s], "%s", k_settings[s].key);
        snprintf(value_names[s], sizeof value_names[s], "%sValueWidget", k_settings[s].key);
        row_ids[s] = ++id;
        value_ids[s] = ++id;
        set_byte(child(label, "ID"), row_ids[s]);
        set_text(child(label, "NAME"), row_names[s]);
        set_byte(child(box, "ID"), value_ids[s]);
        set_text(child(box, "NAME"), value_names[s]);

        memcpy(position, row_position, sizeof position);
        position[1] += ROW_RAISE - s * ROW_STEP;
        set_vector(child(label, "POS"), position);
        memcpy(scale, row_scale, sizeof scale);
        scale[0] = ROW_WIDTH;
        scale[1] *= TEXT_SCALE;
        set_vector(child(label, "SCALE"), scale);
        memcpy(position, value_position, sizeof position);
        position[0] += (ROW_WIDTH - row_scale[0]) / 2;
        position[1] += ROW_RAISE - s * ROW_STEP;
        set_vector(child(box, "POS"), position);
        memcpy(scale, value_scale, sizeof scale);
        scale[1] *= TEXT_SCALE;
        set_vector(child(box, "SCALE"), scale);

        set_text(child(label_vars, "W_TMF"), OPTIONS_TXM);
        set_byte(child(label_vars, "W_TMN"), text_label(s));
        set_text(child(box_vars, "W_TMF"), OPTIONS_TXM);
        set_byte(child(box_vars, "W_TMN"), text_label(s) + 1);
        set_text(child(box_vars, "W_THF"), VIDEO_HST);
        set_byte(child(box_vars, "W_THI"), s + 1);
        set_text(child(box_vars, "A_HIF"), VIDEO_HST);
        set_byte(child(box_vars, "A_HII"), s + 1);
        /* The row's own setup points its value box at it. */
        for (i = 0; i < label->count; i++) {
            Node *events = child(label->children[i], "EVENTS");
            for (j = 0; events && j < events->count; j++) retarget(events->children[j], value_names[s], value_ids[s]);
        }
        node_insert(screen, at++, label);
        node_insert(screen, at++, box);
    }
    node_free(row);
    node_free(value);

    set_text(child(child(title, "ACTOR_VARIABLES"), "W_TMF"), OPTIONS_TXM);
    set_byte(child(child(title, "ACTOR_VARIABLES"), "W_TMN"), TEXT_TITLE);
    /* The back button's moves for languages with longer words. */
    for (i = bbutton->count; i-- > 0;)
        if (!strcmp(bbutton->children[i]->name, "LINK")) node_remove(bbutton, i);

    for (i = deity->count; i-- > 0;) {
        Node *link = deity->children[i], *cause = child(link, "CAUSE"), *events = child(link, "EVENTS");
        if (strcmp(link->name, "LINK")) continue;
        /* "aim > 1" and the like keep a row's value in range: the first is the
         * pattern for the page's. */
        if (cause && cause->value_size == 7 && !memcmp(cause->value, "History", 7) &&
                is_text(child(cause, "HSTF"), OPTIONS_HST)) {
            if (!limit) limit = node_clone(link);
            node_remove(deity, i);
            continue;
        }
        /* The language versions' links hide violence or language. */
        snprintf(name, sizeof name, "%.*s", (int)link->value_size, (const char *)link->value);
        if (strstr(name, "version") || strstr(name, "Violence")) { node_remove(deity, i); continue; }
        /* Setup and fade name each row ("Name."); setup also puts the focus on
         * the first ("Focus.", "Controller"). */
        for (j = events ? events->count : 0; j-- > 0;) {
            Node *event = events->children[j];
            const char *target = target_of(event, name, sizeof name);
            if (!target || !is_template(target)) continue;
            if (strcmp(target, k_template_rows[0])) { node_remove(events, j); continue; }
            if (event->value_size == 5 && !memcmp(event->value, "Name.", 5)) {
                for (s = VIDEO_ROWS; s-- > 1;) {
                    Node *copy = node_clone(event);
                    retarget(copy, row_names[s], row_ids[s]);
                    node_insert(events, j + 1, copy);
                }
            }
            retarget(event, row_names[0], row_ids[0]);
        }
    }
    if (!limit) return 0;
    for (s = 0; s < VIDEO_ROWS; s++) {
        Node *link = node_clone(limit), *cause = child(link, "CAUSE"), *events = child(link, "EVENTS");
        int last = k_settings[s].count - 1;
        snprintf(name, sizeof name, "%s > %d", row_names[s], last);
        set_text(link, name);
        set_text(child(cause, "HSTF"), VIDEO_HST);
        set_byte(child(cause, "ITM"), s + 1);
        set_byte(child(cause, "VAL"), last);
        for (j = 0; events && j < events->count; j++)
            if (events->children[j]->value_size == 4 && !memcmp(events->children[j]->value, "Hist", 4)) {
                set_text(child(events->children[j], "HSTF"), VIDEO_HST);
                set_byte(child(events->children[j], "ITM"), s + 1);
                set_byte(child(events->children[j], "VAL"), last);
            }
        node_insert(deity, deity->count, link);
    }
    node_free(limit);
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
    int s;
    buffer_text(out, ";------------------------------------------------------------------------------\r\n"
                     ";  Video options (recompilation)\r\n"
                     ";------------------------------------------------------------------------------\r\n\r\n"
                     "*Key   = 0\r\n*Value = \"All\"\r\n\r\n");
    for (s = 0; s < SETTING_COUNT; s++) {
        snprintf(line, sizeof line, "*Key   = %d\r\n*Value = \"%s <%d>\"\r\n\r\n", s + 1, k_settings[s].key, s_values[s]);
        buffer_text(out, line);
    }
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

/* The title's "press start" screen: any button. Its "Start Released" link
 * takes the button mask of the screen's own "reset timer" link, which any
 * button fires. */
static int any_button_starts(Node *screen, const char *unused)
{
    Node *widget = actor_named(screen, "PressStartWidget", NULL), *start = NULL, *any = NULL;
    uint32_t i;
    (void)unused;
    for (i = 0; widget && i < widget->count; i++) {
        Node *link = widget->children[i];
        if (strcmp(link->name, "LINK")) continue;
        if (link->value_size == 14 && !memcmp(link->value, "Start Released", 14)) start = child(link, "CAUSE");
        if (link->value_size == 11 && !memcmp(link->value, "reset timer", 11)) any = child(link, "CAUSE");
    }
    if (!start || !any || !child(start, "ANY") || !child(any, "ANY") || !child(start, "APS") || !child(any, "APS"))
        return 0;
    for (i = 0; i < start->count; i++)
        if (!strcmp(start->children[i]->name, "ANY") || !strcmp(start->children[i]->name, "APS")) {
            Node *pattern = child(any, start->children[i]->name);
            node_free(start->children[i]);
            start->children[i] = node_clone(pattern);
        }
    return 1;
}

/* The PC release's main menu ends in QUIT, which asks "are you sure?" and
 * leaves the game (XBOX:DASH, the console's return to the dashboard, ends the
 * process here). The pieces are the pause menu's own: its QUIT entry, the
 * question's title, yes and no, and its "confirm" and "no" links. The menu's
 * entries are widget group 10 and the question group 20; "Group." switches a
 * group on (A 1) or off (A 2), and "Focus." makes an entry the focus (A 0),
 * focusable (A 6) or not (A 7). */
static const Node *s_pause_screen;      /* ingamepause.ati, while the patch is made */

static const char *const k_main_entries[] = {"newgame", "load game", "multiplayer", "options", "cheats", "quit"};
#define MAIN_TOP   2.574f     /* NEW GAME, where the console has it */
#define MAIN_STEP  3.4f       /* closer than the console's 4: QUIT is where the PC has it */
#define MAIN_GROUP 10
#define ASK_GROUP  20

static int is_event(const Node *event, const char *type)
{
    size_t n = strlen(type);
    return event->value_size == n && !memcmp(event->value, type, n);
}

static int main_entry(const char *name)
{
    int e;
    for (e = 0; e < COUNT(k_main_entries); e++)
        if (!strcmp(name, k_main_entries[e])) return e;
    return -1;
}

static int byte_of(const Node *node)
{
    return node && node->value_size == 1 ? node->value[0] : -1;
}

/* The widget's group (A_GN), added from a pause-menu entry's if it has none. */
static void set_group(Node *actor, int group)
{
    Node *vars = child(actor, "ACTOR_VARIABLES"), *gn = child(vars, "A_GN");
    if (!vars) return;
    if (!gn) {
        const Node *pattern = child(child(actor_named(s_pause_screen, "resume", NULL), "ACTOR_VARIABLES"), "A_GN");
        if (!pattern) return;
        gn = node_clone(pattern);
        node_insert(vars, vars->count, gn);
    }
    set_byte(gn, group);
}

/* A copy of `event` aimed at `name`, inserted after it. */
static Node *event_copy_after(Node *events, uint32_t at, const char *name, int id)
{
    Node *copy = node_clone(events->children[at]);
    retarget(copy, name, id);
    node_insert(events, at + 1, copy);
    return copy;
}

/* For each event of `actor`'s links naming `from`, the same naming each of
 * `names`. */
static void name_too(Node *actor, const char *from, const char *const *names, const int *ids, int count)
{
    uint32_t i, j;
    char target[64];
    int n;
    for (i = 0; actor && i < actor->count; i++) {
        Node *events = child(actor->children[i], "EVENTS");
        for (j = 0; events && j < events->count; j++) {
            const char *name = target_of(events->children[j], target, sizeof target);
            if (!name || strcmp(name, from) || !is_event(events->children[j], "Name.")) continue;
            for (n = count; n-- > 0;) event_copy_after(events, j, names[n], ids[n]);
            j += (uint32_t)count;
        }
    }
}

/* A pause-menu link of the question, made the main menu's: the question's
 * widgets by their new ids, the pause menu's entries as the main menu's, and
 * what the main menu does not have (the B prompt, the controller message) left
 * out. */
static void adopt_events(Node *events, int deity_id, const int ask_ids[3], const int entry_ids[])
{
    static const char *const asked[] = {"title", "yes", "no"};
    char target[64];
    int seen_on = 0, seen_off = 0, e, a;
    uint32_t i = 0;
    while (i < events->count) {
        Node *event = events->children[i];
        const char *name = target_of(event, target, sizeof target);
        int ask = -1;
        if (!name) { i++; continue; }
        for (e = 0; e < 3; e++) if (!strcmp(name, asked[e])) ask = e;
        if (!strcmp(name, "DeityNoFade")) {
            retarget(event, "DeityNoFade", deity_id);
            /* The pause menu's "optavail" puts its OPTIONS back; the main
             * menu's "plug" puts MULTIPLAYER back if there are two pads. */
            if (is_event(event, "Name.") && is_text(child(event, "NAME"), "optavail")) set_text(child(event, "NAME"), "plug");
            if (is_event(event, "Name.") && is_text(child(event, "NAME"), "quit")) set_text(child(event, "NAME"), "quitgame");
        } else if (ask >= 0) {
            retarget(event, asked[ask], ask_ids[ask]);
        } else if (is_event(event, "Focus.") &&
                   (!strcmp(name, "resume") || !strcmp(name, "options") || !strcmp(name, "quit"))) {
            /* The entries: the focus back on QUIT, or every entry switched on
             * (but MULTIPLAYER, which "plug" decides) or off. */
            a = byte_of(child(event, "A"));
            if (a == 0) {
                retarget(event, "quit", entry_ids[COUNT(k_main_entries) - 1]);
            } else if ((a == 6 && !seen_on++) || (a == 7 && !seen_off++)) {
                retarget(event, k_main_entries[0], entry_ids[0]);
                for (e = COUNT(k_main_entries); e-- > 1;)
                    if (a == 7 || strcmp(k_main_entries[e], "multiplayer")) {
                        event_copy_after(events, i, k_main_entries[e], entry_ids[e]);
                        i++;
                    }
            } else {
                node_remove(events, i);
                continue;
            }
        } else {
            node_remove(events, i);
            continue;
        }
        i++;
    }
}

static int add_quit_entry(Node *screen, const char *unused)
{
    static const char *const asked[] = {"title", "yes", "no"};
    static const float asked_y[] = {4.406f, -5.387f, -1.305f};     /* the PC's */
    static const char *const pause_links[] = {"confirm", "no"};
    Node *cheats = actor_named(screen, "cheats", NULL), *deity = actor_named(screen, "DeityNoFade", NULL);
    Node *stone = actor_named(screen, "stoneWidget", NULL), *quit, *link, *events;
    const Node *pause_deity = actor_named(s_pause_screen, "DeityNoFade", NULL), *dash_pattern = NULL;
    const Node *group_pattern = NULL;
    int entry_ids[COUNT(k_main_entries)], ask_ids[3], id = 0, deity_id = actor_id(deity), e;
    const char *new_names[4] = {"quit", "title", "yes", "no"};
    int new_ids[4];
    uint32_t i, j, at;
    float position[3];
    (void)unused;

    if (!cheats || !deity || !stone || !pause_deity || deity_id < 0 || !child(cheats, "LINK")) return 0;
    for (i = 0; i < screen->count; i++)
        if (actor_id(screen->children[i]) > id) id = actor_id(screen->children[i]);
    /* The pause menu's patterns: switching a group, and a plain event. */
    for (i = 0; i < pause_deity->count; i++) {
        const Node *pause_events = child(pause_deity->children[i], "EVENTS");
        for (j = 0; pause_events && j < pause_events->count; j++) {
            if (!group_pattern && is_event(pause_events->children[j], "Group.")) group_pattern = pause_events->children[j];
            if (!dash_pattern && is_event(pause_events->children[j], "Turn visible")) dash_pattern = pause_events->children[j];
        }
    }
    if (!group_pattern || !dash_pattern) return 0;

    /* QUIT: CHEATS, saying the pause menu's "Quit" and asking. */
    quit = node_clone(cheats);
    new_ids[0] = ++id;
    set_byte(child(quit, "ID"), id);
    set_text(child(quit, "NAME"), "quit");
    set_text(child(child(quit, "ACTOR_VARIABLES"), "W_TMF"), "$\\Data\\Text\\InGamePause.txm");
    set_byte(child(child(quit, "ACTOR_VARIABLES"), "W_TMN"), 3);
    link = child(quit, "LINK");
    events = child(link, "EVENTS");
    if (!events || !events->count || !is_event(events->children[0], "Name.")) { node_free(quit); return 0; }
    set_text(link, "quit selected");
    set_text(child(events->children[0], "NAME"), "confirm");
    while (events->count > 1) node_remove(events, 1);
    actor_named(screen, "cheats", &at);
    node_insert(screen, at + 1, quit);

    /* The entries, evenly down to QUIT, in group 10. */
    for (e = 0; e < COUNT(k_main_entries); e++) {
        Node *entry = actor_named(screen, k_main_entries[e], NULL);
        if (!entry || !get_vector(child(entry, "POS"), position)) return 0;
        position[1] = MAIN_TOP - e * MAIN_STEP;
        set_vector(child(entry, "POS"), position);
        set_group(entry, MAIN_GROUP);
        entry_ids[e] = actor_id(entry);
    }

    /* The question: the pause menu's title, yes and no, where the PC has them. */
    for (e = 0; e < 3; e++) {
        const Node *pattern = actor_named(s_pause_screen, asked[e], NULL);
        Node *widget;
        if (!pattern) return 0;
        widget = node_clone(pattern);
        ask_ids[e] = new_ids[e + 1] = ++id;
        set_byte(child(widget, "ID"), id);
        if (get_vector(child(widget, "POS"), position)) {
            position[1] = asked_y[e];
            set_vector(child(widget, "POS"), position);
        }
        for (i = 0; i < widget->count; i++)
            if (child(widget->children[i], "EVENTS"))
                adopt_events(child(widget->children[i], "EVENTS"), deity_id, ask_ids, entry_ids);
        actor_named(screen, "DeityNoFade", &at);
        node_insert(screen, at, widget);
    }

    /* Asking and not quitting: the pause menu's links. */
    for (e = 0; e < COUNT(pause_links); e++) {
        Node *copy = NULL;
        for (i = 0; i < pause_deity->count && !copy; i++) {
            const Node *pattern = pause_deity->children[i];
            if (!strcmp(pattern->name, "LINK") && pattern->value_size == strlen(pause_links[e]) &&
                    !memcmp(pattern->value, pause_links[e], pattern->value_size))
                copy = node_clone(pattern);
        }
        if (!copy || !child(copy, "EVENTS")) { node_free(copy); return 0; }
        adopt_events(child(copy, "EVENTS"), deity_id, ask_ids, entry_ids);
        node_insert(deity, deity->count, copy);
        /* Quitting: the dashboard. */
        if (e == 0) {
            Node *dash = node_clone(copy), *dash_events = child(dash, "EVENTS"), *event;
            set_text(dash, "quitgame");
            set_text(child(child(dash, "CAUSE"), "NAME"), "quitgame");
            while (dash_events->count) node_remove(dash_events, 0);
            event = node_clone(dash_pattern);
            set_text(event, "XBOX:DASH");
            retarget(event, "DeityNoFade", deity_id);
            node_insert(dash_events, 0, event);
            node_insert(deity, deity->count, dash);
        }
    }

    /* The question is off until asked. */
    for (i = 0; i < deity->count; i++) {
        Node *setup = deity->children[i], *cause = child(setup, "CAUSE"), *group;
        if (strcmp(setup->name, "LINK") || !cause || !is_event(cause, "Setup") || !child(setup, "EVENTS")) continue;
        group = node_clone(group_pattern);
        retarget(group, "DeityNoFade", deity_id);
        set_byte(child(group, "A"), 2);
        set_byte(child(group, "G"), ASK_GROUP);
        node_insert(child(setup, "EVENTS"), child(setup, "EVENTS")->count, group);
    }
    /* QUIT appears with CHEATS; it and the question fade with it. */
    name_too(stone, "cheats", new_names, new_ids, 1);
    name_too(deity, "cheats", new_names, new_ids, 4);
    return 1;
}

/* ControlSetup.hst: Invert Look (Reverse, keys 4, 14, 24 and 34 for each pad's
 * setup and 44 for the one being set) off as a new player finds it; the disc
 * has it on. It turns the mouse as it does the pad. */
static void look_not_inverted(Buffer *history)
{
    static const char from[] = "<1>\"";
    char *text = (char *)history->data, *at;
    size_t n = history->size;
    for (at = text; at && (size_t)(at - text) + 8 < n; ) {
        char *reverse = (char *)memchr(at, 'R', n - (size_t)(at - text));
        if (!reverse) break;
        if ((size_t)(reverse - text) + 12 < n && !memcmp(reverse, "Reverse", 7)) {
            char *mark = reverse + 7;
            while ((size_t)(mark - text) + 4 < n && *mark != '<' && *mark != '\n') mark++;
            if (!memcmp(mark, from, sizeof from - 1)) mark[1] = '0';
        }
        at = reverse + 1;
    }
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
    enum { OPTIONS_TEXT = 3 * MENU_COUNT, VIDEO_HISTORY, HISTORY_LIST, MARKER, PRESS_START, MAIN_MENU,
           CONTROL_HISTORY, FILE_COUNT };
    PatchFile files[FILE_COUNT];
    Trees trees;
    Buffer tree = {0}, existing = {0}, pause = {0};
    Node *pause_screen = NULL;
    char path[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    int ok = 0, i, s;
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
    snprintf(from, sizeof from, SCREENS "firstinteractivescreen\\firstinteractivescreen.ati");
    if (!patch_screen(&trees, from, from, &files[PRESS_START], any_button_starts, NULL)) goto done;
    if (!trees_read(&trees, SCREENS "ingamepause\\ingamepause.ati", &pause) || !(pause_screen = screen_load(&pause)))
        goto done;
    s_pause_screen = pause_screen;
    snprintf(from, sizeof from, SCREENS "mainmenu\\mainmenu.ati");
    if (!patch_screen(&trees, from, from, &files[MAIN_MENU], add_quit_entry, NULL)) goto done;
    files[CONTROL_HISTORY].key = tree_key("data\\history\\controlsetup.hst");
    if (!trees_read(&trees, "data\\history\\controlsetup.hst", &files[CONTROL_HISTORY].data)) goto done;
    look_not_inverted(&files[CONTROL_HISTORY].data);

    files[OPTIONS_TEXT].key = tree_key("data\\text\\options.txm");
    files[VIDEO_HISTORY].key = tree_key("data\\history\\video.hst");
    files[HISTORY_LIST].key = tree_key("data\\history\\history.txt");
    files[MARKER].key = tree_key(PATCH_MARKER);
    if (!trees_read(&trees, "data\\text\\options.txm", &files[OPTIONS_TEXT].data)) goto done;
    add_text(&files[OPTIONS_TEXT].data, TEXT_ENTRY, "VIDEO OPTIONS");
    add_text(&files[OPTIONS_TEXT].data, TEXT_TITLE, "Video Options");
    for (s = 0; s < SETTING_COUNT; s++) {
        add_text(&files[OPTIONS_TEXT].data, text_label(s), k_settings[s].label);
        for (i = 0; i < k_settings[s].count; i++)
            add_text(&files[OPTIONS_TEXT].data, text_label(s) + 1 + i, k_settings[s].values[i]);
    }

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
    s_pause_screen = NULL;
    node_free(pause_screen);
    free(pause.data);
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

/* A history file's item values (8 bytes an item, the value first, from key 1:
 * key 0, "All", is not one), found through the history manager
 * [[0x250A84] + 0x80]: a vector of groups, each with its items at +4..+8 and
 * its file name at +0x14 (sub_00060680). */
static uint32_t history_items(const char *file, uint32_t count)
{
    uint32_t game, manager, first, last, at;
    if (!guest_read(0x250A84u, &game) || !guest_read(game + 0x80u, &manager) ||
            !guest_read(manager, &first) || !guest_read(manager + 4u, &last) ||
            last < first || last - first > 1024u) return 0;
    for (at = first; at < last; at += 4) {
        uint32_t group, name, begin, end;
        if (!guest_read(at, &group) || !guest_read(group + 0x14u, &name) || !guest_name_is(name, file))
            continue;
        if (guest_read(group + 4u, &begin) && guest_read(group + 8u, &end) && end >= begin + count * 8u)
            return begin;
    }
    return 0;
}

/* FrontEnd.hst's "FE Flag" (key 6): 0 in play, otherwise a menu (1 the front
 * end, 2 the intro, 3 the pause menu). The keyboard's Escape is Back there
 * and Start in play (RECOMP_KBM_MENU_MAP) -- and on the pause menu's first
 * screen, where Start resumes and Back does nothing. */
static void follow_menus(void)
{
    extern void xbox_KbmSetMenuMode(int on);
    uint32_t items = history_items("frontend.hst", 6), flag;
    if (!items || !guest_read(items + 5u * 8u, &flag)) return;
    s_in_menu = flag != 0;
    xbox_KbmSetMenuMode(flag != 0 && !s_pause_menu);
}

/* ── PC button icons ──────────────────────────────────────────────
 *
 * The PC release draws the menus' A (select) and B (back) prompts as the
 * keyboard's Enter and Escape. Its icons (data\Textures\OnScreenGraphics\
 * FrontEnd\Controls\OSG_bf_a.dds and OSG_bf_bbutton.dds, read from a PC
 * install named by pc_game_dir in turok_settings.ini's [paths], or
 * RECOMP_PC_GAME_DIR) replace the Xbox ones where the renderer meets them:
 * by the hash of the Xbox textures' bytes, DXT3 32x32 with two mips. */
static const struct {
    uint64_t hash;
    const char *file;
} k_pc_icons[] = {
    {0x89FA67573E0F3F67ull, "OSG_bf_a.dds"},
    {0x256B2E3DFDD6EA16ull, "OSG_bf_bbutton.dds"},
};

static uint32_t rgb565(uint16_t c)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (r * 255 / 31) << 16 | (g * 255 / 63) << 8 | (b * 255 / 31);
}

static uint32_t mix(uint32_t a, uint32_t b, int wa, int wb)
{
    uint32_t out = 0, shift;
    for (shift = 0; shift < 24; shift += 8)
        out |= ((((a >> shift) & 255) * wa + ((b >> shift) & 255) * wb) / (wa + wb)) << shift;
    return out;
}

/* A DDS's first image as B8G8R8A8 (DXT1, DXT3 or DXT5); NULL if not one. */
static uint32_t *dds_decode(const uint8_t *data, size_t size, uint32_t *width, uint32_t *height)
{
    uint32_t w, h, fourcc, block, bx, by, i;
    uint32_t *pixels;
    size_t pos = 128;
    if (size < 128 || memcmp(data, "DDS ", 4)) return NULL;
    memcpy(&h, data + 12, 4); memcpy(&w, data + 16, 4); memcpy(&fourcc, data + 84, 4);
    if (fourcc != 0x31545844 && fourcc != 0x33545844 && fourcc != 0x35545844) return NULL;
    block = fourcc == 0x31545844 ? 8 : 16;
    if (!w || !h || w > 1024 || h > 1024 || size < pos + (size_t)((w + 3) / 4) * ((h + 3) / 4) * block) return NULL;
    pixels = (uint32_t *)calloc((size_t)w * h, 4);
    for (by = 0; by < h; by += 4)
        for (bx = 0; bx < w; bx += 4, pos += block) {
            const uint8_t *b = data + pos;
            uint32_t alpha[16], colors[4], indices;
            uint16_t c0, c1;
            for (i = 0; i < 16; i++) alpha[i] = 255;
            if (fourcc == 0x33545844) {
                for (i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
                b += 8;
            } else if (fourcc == 0x35545844) {
                uint32_t a[8], k;
                uint64_t bits = 0;
                a[0] = b[0]; a[1] = b[1];
                for (k = 2; k < 8; k++)
                    a[k] = a[0] > a[1] ? (a[0] * (8 - k) + a[1] * (k - 1)) / 7
                         : k < 6 ? (a[0] * (6 - k) + a[1] * (k - 1)) / 5 : (k == 6 ? 0 : 255);
                for (k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
                for (i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
                b += 8;
            }
            c0 = (uint16_t)(b[0] | b[1] << 8); c1 = (uint16_t)(b[2] | b[3] << 8);
            memcpy(&indices, b + 4, 4);
            colors[0] = rgb565(c0); colors[1] = rgb565(c1);
            if (c0 > c1 || block == 16) {
                colors[2] = mix(colors[0], colors[1], 2, 1);
                colors[3] = mix(colors[0], colors[1], 1, 2);
            } else {
                colors[2] = mix(colors[0], colors[1], 1, 1);
                colors[3] = 0;
            }
            for (i = 0; i < 16; i++) {
                uint32_t x = bx + i % 4, y = by + i / 4, k = (indices >> (2 * i)) & 3;
                uint32_t a = block == 8 && c0 <= c1 && k == 3 ? 0 : alpha[i];
                if (x < w && y < h) pixels[(size_t)y * w + x] = colors[k] | a << 24;
            }
        }
    *width = w; *height = h;
    return pixels;
}

/* The PC install named by pc_game_dir (or RECOMP_PC_GAME_DIR); 0 if none. */
static int pc_game_dir(char *directory, size_t size)
{
    const char *env = getenv("RECOMP_PC_GAME_DIR");
    if (env && *env) snprintf(directory, size, "%s", env);
    else GetPrivateProfileStringA("paths", "pc_game_dir", "", directory, (DWORD)size, s_ini_path);
    return *directory != 0;
}

/* A DDS of the PC install, `path` under its data\Textures, decoded. */
static uint32_t *pc_texture(const char *directory, const char *path, uint32_t *width, uint32_t *height)
{
    char full[MAX_PATH];
    uint32_t *pixels = NULL;
    uint8_t *data;
    long size;
    FILE *file;
    snprintf(full, sizeof full, "%s\\data\\Textures\\%s", directory, path);
    if (!(file = fopen(full, "rb"))) return NULL;
    fseek(file, 0, SEEK_END); size = ftell(file); fseek(file, 0, SEEK_SET);
    data = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    if (size > 0 && fread(data, 1, (size_t)size, file) == (size_t)size)
        pixels = dds_decode(data, (size_t)size, width, height);
    fclose(file);
    free(data);
    return pixels;
}

static int load_pc_icons(void)
{
    char directory[MAX_PATH], path[MAX_PATH];
    size_t i;
    int loaded = 0;
    if (!pc_game_dir(directory, sizeof directory)) return 0;
    for (i = 0; i < sizeof k_pc_icons / sizeof *k_pc_icons; i++) {
        uint32_t width, height, *pixels;
        snprintf(path, sizeof path, "OnScreenGraphics\\FrontEnd\\Controls\\%s", k_pc_icons[i].file);
        pixels = pc_texture(directory, path, &width, &height);
        if (!pixels) continue;
        nv2a_gpu_replace_texture(k_pc_icons[i].hash, pixels, width, height);
        free(pixels);
        loaded++;
    }
    fprintf(stderr, "[VIDEO] PC button icons: %d of %d from %s\n", loaded,
            (int)(sizeof k_pc_icons / sizeof *k_pc_icons), directory);
    return loaded;
}

static void item_write(uint32_t items, int item, int value)
{
    __try {
        *(volatile int32_t *)((uintptr_t)(items + 8u * (uint32_t)(item - 1)) + g_xbox_mem_offset) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static DEVMODEA monitor_mode(void)
{
    DEVMODEA mode = {0};
    mode.dmSize = sizeof mode;
    if (!EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &mode)) mode.dmPelsWidth = mode.dmPelsHeight = 0;
    return mode;
}

/* Widescreen: the screen's width over 4:3's. The front end stays 4:3 (its
 * stage is a 3D set built to end just past the 4:3 edges), and so do the
 * in-engine cutscenes, framed for 4:3 with their own bars. */
static float aspect_ratio(void)
{
    static const float ratios[] = {4.0f / 3, 16.0f / 10, 16.0f / 9, 64.0f / 27, 0};
    float ratio = ratios[s_values[SET_ASPECT]];
    if (!ratio) {
        DEVMODEA mode = monitor_mode();
        ratio = mode.dmPelsHeight ? (float)mode.dmPelsWidth / (float)mode.dmPelsHeight : 4.0f / 3;
    }
    return ratio < 4.0f / 3 ? 4.0f / 3 : ratio;
}

static void apply_aspect(void)
{
    nv2a_gpu_set_wide_factor(s_front_end || s_cinema ? 1.0f : aspect_ratio() * 3 / 4);
}

/* The frame limit is the emulated vertical blank's rate (each flip waits for
 * one), or no wait at all. Turok steps its game by a measured clock
 * (sub_00028B40), so it plays at the same speed at any rate. The front end
 * keeps the console's 59.94: its movies are paced by the blank. */
static void apply_frames(void)
{
    static const double rates[] = {30, 59.94, 120, 144, 240, 0, 0};
    int value = s_front_end ? 1 : s_values[SET_FRAMES];
    double rate = rates[value];
    if (value == COUNT(k_frames) - 2) {
        DEVMODEA mode = monitor_mode();
        rate = mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 59.94;
    }
    xbox_SetFlipUnlimited(value == COUNT(k_frames) - 1);
    xbox_Nv2aSetVblankRate(rate ? rate : 59.94);
}

static void apply_setting(int setting)
{
    switch (setting) {
    case SET_RESOLUTION: nv2a_gpu_set_render_scale(k_resolution_scales[s_values[SET_RESOLUTION]]); break;
    case SET_ASPECT: apply_aspect(); xbox_FramebufferSetWindowAspect(aspect_ratio()); break;
    case SET_DISPLAY: xbox_FramebufferSetFullscreen(s_values[SET_DISPLAY]); break;
    case SET_VSYNC: xbox_SetPresentInterval(s_values[SET_VSYNC]); break;
    case SET_FRAMES: apply_frames(); break;
    case SET_ANISOTROPY: nv2a_gpu_set_anisotropy(1 << s_values[SET_ANISOTROPY]); break;
    case SET_MSAA: nv2a_gpu_set_msaa(1 << s_values[SET_MSAA]); break;
    case SET_ICONS: nv2a_gpu_set_texture_replacements(s_values[SET_ICONS]); break;
    case SET_MOUSE_LOOK: xbox_KbmSetRawMouse(s_values[SET_MOUSE_LOOK]); break;
    case SET_DRAW_DISTANCE:           /* read each frame */
    case SET_MOUSE_SPEED: break;
    }
}

/* Raw mouse look, after the input mapper has filled the players' axes
 * (sub_0002F580): the mouse's counts this frame, over the frame's time, are
 * added to player 1's look axes (0x250BD0 x, 0x250BD4 y, at stick scale).
 * The game turns by axis x turn rate x frame time and does not clamp the
 * axes, so a count turns the camera by the same angle at any frame rate and
 * any speed of hand -- not at a stick's rate, as the mouse did through the
 * right stick. Mouse speed steps by 1.4x around 5.
 *
 * Further on, the look axes pass a deadzone: under 0.3 is nothing, and above
 * it (|a| - 0.3) / 0.7 (the remap sub_001420D0 also does). A small mouse
 * movement fell inside it and was lost, so the sum -- the pad's effective
 * value plus the mouse's -- is written as the axis that comes out of the
 * deadzone as that sum.
 *
 * The game's Invert Look (Control Setup > Advanced) flips the look y after
 * this, so it turns the mouse as it does the pad: one setting for both, off
 * as the patch's ControlSetup.hst has it for a new player. */

static void menu_pointer(void);

static float through_deadzone(float axis)
{
    float size = axis < 0 ? -axis : axis;
    return size <= 0.3f ? 0.0f : (axis < 0 ? -1.0f : 1.0f) * (size - 0.3f) / 0.7f;
}

static float into_deadzone(float effective)
{
    if (effective == 0.0f) return 0.0f;
    return effective < 0 ? -(0.3f + 0.7f * -effective) : 0.3f + 0.7f * effective;
}

void video_settings_mouse_look(void)
{
    static LARGE_INTEGER frequency, last;
    static float test = -1;
    LARGE_INTEGER now;
    double seconds, gain;
    float turn[2];
    int dx = 0, dy = 0, step, axis;
    float *axes = (float *)((uintptr_t)0x250BD0u + g_xbox_mem_offset);
    menu_pointer();
    /* DIAG RECOMP_LOOK_TEST=<x>: a constant effective look x. */
    if (test < 0) test = getenv("RECOMP_LOOK_TEST") ? (float)atof(getenv("RECOMP_LOOK_TEST")) : 0;
    if (s_values[SET_MOUSE_LOOK] != 1 && test <= 0) return;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    seconds = last.QuadPart ? (double)(now.QuadPart - last.QuadPart) / (double)frequency.QuadPart : 1.0 / 60;
    last = now;
    if (seconds < 0.001) seconds = 0.001;
    if (seconds > 0.1) seconds = 0.1;
    if (s_values[SET_MOUSE_LOOK] == 1) xbox_FramebufferMouseDelta(&dx, &dy);
    if (s_in_menu || (!dx && !dy && test <= 0)) return;
    for (gain = 0.0005, step = 4; step < s_values[SET_MOUSE_SPEED]; step++) gain *= 1.4;
    for (step = 4; step > s_values[SET_MOUSE_SPEED]; step--) gain /= 1.4;
    turn[0] = (float)(dx * gain / seconds) + test;
    turn[1] = (float)(-dy * gain / seconds);
    for (axis = 0; axis < 2; axis++)
        if (turn[axis] != 0.0f) axes[axis] = into_deadzone(through_deadzone(axes[axis]) + turn[axis]);
}

/* Draw distance: how much further than the game's own distances (A_NDD
 * where an actor starts to fade, A_MDD where it is gone: grass and plants
 * have 15 to 25) actors with one are drawn. */
float video_settings_draw_distance(void)
{
    return (float)(1 << s_values[SET_DRAW_DISTANCE]);
}

void video_settings_screen(int front_end, int pause_menu)
{
    s_pause_menu = pause_menu;
    if (front_end == s_front_end && !s_cinema) return;
    s_front_end = front_end;
    s_cinema = 0;      /* a level or menu left mid-cutscene */
    apply_aspect();
    apply_frames();
}

void video_settings_cinema(int on)
{
    if (on == s_cinema) return;
    s_cinema = on;
    fprintf(stderr, "[VIDEO] cutscene %s\n", on ? "begins: 4:3" : "ends");
    apply_aspect();
}

/* ── Menu pointer ─────────────────────────────────────────────────
 *
 * The PC release's menus take the mouse: an arrow over the window, the entry
 * under it focused, a click choosing it. Here, in any menu (FE Flag not 0):
 *
 *   - the toolkit lets the mouse go and shows the PC's cursor.dds
 *     (xbox_FramebufferSetMenuPointer, xbox_FramebufferSetCursorImage) once
 *     the mouse moves; when the keys or a pad move the focus it hides again;
 *   - moving over an entry focuses it as the menu's up and down do: the
 *     controller handed from the old entry to the new, then the screen's own
 *     SetFocus (set_focus);
 *   - a left click chooses it (A); on an option row (a SelectionMenuWidget,
 *     changed with left and right) it steps the value on (right), a right
 *     click back (left). A right click elsewhere is Back (B, or Start on the
 *     pause menu's first screen, as Escape is), and the wheel steps an option
 *     row's value under the pointer or else moves up and down.
 *
 * The screen is the loader's this (sub_00054A60). Its actors are a tree from
 * +0x9C (next_actor); the focus is at +0x198. An entry is a widget (its
 * class, the type its vtable's first function returns -- mov eax, type; ret
 * -- is 0x242778 or derives from it: a type's first word is its parent) that
 * can take the focus (+0x244 bit 0, which the menu's up and down also ask),
 * and its rectangle on the 640x480 screen is at +0x2C4 (x, y, width,
 * height). Option rows are told by the screen's .ati: a SelectionMenuWidget
 * with no A button link. */
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern void sub_000544D0(void);
extern void xbox_FramebufferSetMenuPointer(int on);
extern int xbox_FramebufferPointer(int *x, int *y, int *width, int *height);
extern void xbox_FramebufferSetCursorImage(const uint32_t *bgra, int width, int height, int hot_x, int hot_y);
extern float nv2a_gpu_presented_aspect(void);
extern int xbox_FramebufferMouseClicks(int which);
extern void xbox_FramebufferHidePointer(void);
extern void xbox_KbmPulse(const char *target, int ms);
extern int xbox_KbmMenuWheel(void);

#define WIDGET_TYPE 0x242778u
#define PRESS_MS    70        /* a pad button held this long, then as long up */

static char s_game_dir[MAX_PATH];
static volatile uint32_t s_menu_screen;     /* the screen's object; 0 in a level */
static volatile uint32_t s_menu_screens;    /* screens entered: the object is reused */
/* Its option rows: each shows and steps a history item (its value widget's
 * A_HIF/A_HII), kept to 0..max by a "> max" link (COMP 4); max 0 if none. */
typedef struct OptionRow {
    char name[32], file[40];
    int item, max;
} OptionRow;
static OptionRow s_option_rows[64];
static int s_option_row_count;

/* A file of the game's data, patch0.tre's copy first. */
static int archive_read(const char *path, Buffer *out)
{
    static Trees trees;
    static int opened;
    char patch[MAX_PATH];
    uint32_t count, index, record[4], key = tree_key(path);
    FILE *file;
    snprintf(patch, sizeof patch, "%s\\patch0.tre", s_game_dir);
    if ((file = fopen(patch, "rb")) != NULL) {
        if (fread(&count, 4, 1, file) == 1 && count < 4096)
            for (index = 0; index < count && fread(record, 16, 1, file) == 1; index++) {
                if (record[2] != key) continue;
                out->size = 0;
                buffer_put(out, "", 0);
                if (out->capacity < record[1]) {
                    out->data = (uint8_t *)realloc(out->data, record[1]);
                    out->capacity = record[1];
                }
                if (!fseek(file, (long)record[0], SEEK_SET) && fread(out->data, 1, record[1], file) == record[1]) {
                    out->size = record[1];
                    fclose(file);
                    return 1;
                }
                break;
            }
        fclose(file);
    }
    if (!opened) opened = trees_open(&trees, s_game_dir) ? 1 : -1;
    return opened > 0 && trees_read(&trees, path, out);
}

static int has_a_button_link(const Node *actor)
{
    uint32_t i;
    for (i = 0; i < actor->count; i++) {
        const Node *cause = child(actor->children[i], "CAUSE"), *any = child(cause, "ANY");
        if (!strcmp(actor->children[i]->name, "LINK") && cause && is_event(cause, "CoBu") &&
                any && any->value_size && (any->value[0] & 0x40))
            return 1;
    }
    return 0;
}

/* Which history item a row steps and how far: its links name its value
 * widget (the setup's Finger), whose A_HIF/A_HII are the item; a "> max"
 * link on the screen keeps it in range. */
static void option_row_value(const Node *screen, const Node *row, OptionRow *out)
{
    char target[64];
    const Node *value = NULL, *vars, *file;
    uint32_t i, j;
    out->file[0] = 0;
    out->item = out->max = 0;
    for (i = 0; i < row->count && !value; i++) {
        const Node *events = child(row->children[i], "EVENTS");
        for (j = 0; events && j < events->count && !value; j++)
            if (target_of(events->children[j], target, sizeof target)) value = actor_named(screen, target, NULL);
    }
    vars = value ? child(value, "ACTOR_VARIABLES") : NULL;
    file = child(vars, "A_HIF");
    if (!file || file->value_size < 2 || byte_of(child(vars, "A_HII")) <= 0) return;
    out->item = byte_of(child(vars, "A_HII"));
    {
        const char *text = (const char *)file->value, *slash = strrchr(text, '\\');
        snprintf(out->file, sizeof out->file, "%s", slash ? slash + 1 : text);
    }
    for (i = 0; i < screen->count; i++) {
        const Node *actor = screen->children[i];
        for (j = 0; j < actor->count; j++) {
            const Node *cause = child(actor->children[j], "CAUSE");
            if (strcmp(actor->children[j]->name, "LINK") || !cause || !is_event(cause, "History") ||
                    byte_of(child(cause, "COMP")) != 4 || byte_of(child(cause, "ITM")) != out->item ||
                    !child(cause, "HSTF") || child(cause, "HSTF")->value_size != file->value_size ||
                    _strnicmp((const char *)child(cause, "HSTF")->value, (const char *)file->value, file->value_size))
                continue;
            out->max = byte_of(child(cause, "VAL"));
        }
    }
}

/* The screen's option rows, from its .ati (`name` is its .atr). */
static void find_option_rows(const char *name)
{
    static const char kind[] = "selectionmenuwidget.atr";
    char path[MAX_PATH], *dot;
    Buffer file = {0};
    Node *screen;
    uint32_t i;
    s_option_row_count = 0;
    if (!_strnicmp(name, "d:\\", 3)) name += 3;
    snprintf(path, sizeof path, "%s", name);
    if (!(dot = strrchr(path, '.')) || _stricmp(dot, ".atr")) return;
    dot[3] = 'i';
    if (!archive_read(path, &file) || !(screen = screen_load(&file))) {
        free(file.data);
        return;
    }
    for (i = 0; i < screen->count && s_option_row_count < COUNT(s_option_rows); i++) {
        const Node *actor = screen->children[i], *actor_name = child(actor, "NAME");
        size_t n = actor->value_size, k = sizeof kind - 1;
        if (!actor_name || !actor_name->value_size || n < k || _strnicmp((const char *)actor->value + n - k, kind, k) ||
                has_a_button_link(actor))
            continue;
        snprintf(s_option_rows[s_option_row_count].name, sizeof s_option_rows[0].name, "%.*s",
                 (int)actor_name->value_size - 1, (const char *)actor_name->value);
        option_row_value(screen, actor, &s_option_rows[s_option_row_count++]);
    }
    node_free(screen);
    free(file.data);
}

void video_settings_menu_screen(uint32_t screen, const char *name)
{
    char lower[160];
    size_t n;
    for (n = 0; n + 1 < sizeof lower && name[n]; n++) lower[n] = (char)tolower((unsigned char)name[n]);
    lower[n] = 0;
    if (!strstr(lower, "\\screens\\")) {
        s_menu_screen = 0;
        return;
    }
    find_option_rows(name);
    s_menu_screen = screen;
    s_menu_screens++;
    fprintf(stderr, "[MENU] screen %08X, %d option rows\n", screen, s_option_row_count);
}

static int guest_float(uint32_t va, float *value)
{
    uint32_t bits;
    if (!guest_read(va, &bits)) return 0;
    memcpy(value, &bits, 4);
    return 1;
}

static int is_widget(uint32_t actor)
{
    static uint32_t known[32], verdict[32];
    uint32_t vtable, function, word, type, depth;
    int slot;
    if (!guest_read(actor, &vtable)) return 0;
    for (slot = 0; slot < 32 && known[slot]; slot++)
        if (known[slot] == vtable) return (int)verdict[slot];
    /* The class: its first virtual is mov eax, type (B8 imm32); ret (C3). */
    if (!guest_read(vtable, &function) || !guest_read(function, &word) || (word & 0xFF) != 0xB8 ||
            !guest_read(function + 4, &type) || ((type >> 8) & 0xFF) != 0xC3) return 0;
    guest_read(function + 1, &type);
    for (depth = 0; depth < 16 && type && type != WIDGET_TYPE; depth++)
        if (!guest_read(type, &type)) type = 0;
    if (slot < 32) { known[slot] = vtable; verdict[slot] = type == WIDGET_TYPE; }
    return type == WIDGET_TYPE;
}

static const OptionRow *option_row(uint32_t actor)
{
    char name[32];
    uint32_t text;
    int i;
    if (!actor || !guest_read(actor + 0x5C, &text)) return NULL;
    __try {
        snprintf(name, sizeof name, "%s", (const char *)((uintptr_t)text + g_xbox_mem_offset));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NULL;
    }
    for (i = 0; i < s_option_row_count; i++)
        if (!strcmp(name, s_option_rows[i].name)) return &s_option_rows[i];
    return NULL;
}

static int is_option_row(uint32_t actor)
{
    return option_row(actor) != NULL;
}

/* A click on an option row at the end of its values goes round to the other
 * end (the game's own left and right stop there); 0 if it is not at an end. */
static int wrap_option(uint32_t actor, int forward)
{
    const OptionRow *row = option_row(actor);
    uint32_t items, value;
    if (!row || row->max <= 0 || !(items = history_items(row->file, (uint32_t)row->item)) ||
            !guest_read(items + 8u * (uint32_t)(row->item - 1), &value))
        return 0;
    if (forward && (int)value >= row->max) item_write(items, row->item, 0);
    else if (!forward && (int)value <= 0) item_write(items, row->item, row->max);
    else return 0;
    return 1;
}

/* The screen's actors are a tree, walked depth first (sub_0009C1C0): the
 * first child at +0x6C (while +0x74 says there are children), the next
 * sibling at +0x14, the parent at +0x64 (or +0x68). */
static uint32_t next_actor(uint32_t actor)
{
    uint32_t next = 0, up = 0;
    if (guest_read(actor + 0x74, &next) && next && guest_read(actor + 0x6C, &next) && next) return next;
    if (guest_read(actor + 0x14, &next) && next) return next;
    if (!guest_read(actor + 0x64, &up) || !up) guest_read(actor + 0x68, &up);
    while (up) {
        if (guest_read(up + 0x14, &next) && next) return next;
        actor = up;
        if (!guest_read(actor + 0x64, &up) || !up) if (!guest_read(actor + 0x68, &up)) up = 0;
    }
    return 0;
}

/* The entry of `screen` at (x, y) on the 640x480 screen, or 0. */
static uint32_t entry_at(uint32_t screen, float x, float y)
{
    uint32_t actor, owner, flags;
    int n;
    float box[4];
    if (!guest_read(screen + 0x9C, &actor)) return 0;
    for (n = 0; actor && n < 2048; n++, actor = next_actor(actor)) {
        if (is_widget(actor) && guest_read(actor + 0x48, &owner) && owner == screen &&
                guest_read(actor + 0x244, &flags) && (flags & 1) &&
                guest_float(actor + 0x2C4, &box[0]) && guest_float(actor + 0x2C8, &box[1]) &&
                guest_float(actor + 0x2CC, &box[2]) && guest_float(actor + 0x2D0, &box[3]) &&
                box[2] > 0 && box[3] > 0 && box[2] < 2000 && box[3] < 2000 &&
                x >= box[0] && x < box[0] + box[2] && y >= box[1] && y < box[1] + box[3])
            return actor;
    }
    return 0;
}

/* A this-call into the game, on its thread between its own calls; its eax. */
static uint32_t guest_call(void (*function)(void), uint32_t self, int count, const uint32_t *arguments)
{
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, esp = g_esp, result;
    int i;
    for (i = count; i-- > 0;) {
        g_esp -= 4;
        *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset) = arguments[i];
    }
    g_esp -= 4;
    *(volatile uint32_t *)((uintptr_t)g_esp + g_xbox_mem_offset) = 0;      /* return address */
    g_ecx = self;
    function();
    result = g_eax;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_esp = esp;
    return result;
}

/* Focus `actor` as the menu's up and down do (sub_000A86A0): the controller
 * goes with the focus -- a button press is the widget's that holds it -- so
 * the old entry gives it up (sub_0003A1F0) and the new one takes the same
 * (pad n: sub_00046D60(n); any pad: sub_00045390 or sub_00045450, as the old
 * one had it), then the screen's SetFocus tells both widgets. */
extern void sub_00038C50(void), sub_0003AA60(void), sub_0003AA80(void), sub_0003A1F0(void);
extern void sub_00046D60(void), sub_00045390(void), sub_00045450(void);

static void set_focus(uint32_t screen, uint32_t old, uint32_t actor)
{
    uint32_t pad = 0, owner, arguments[2];
    int handed = 0;
    if (old && guest_read(old + 0x48, &owner) && owner == screen && is_widget(old)) {
        uint32_t control = 0;
        if (guest_read(old + 0x210, &control) && control && guest_call(sub_00038C50, old, 0, NULL)) {
            if (guest_read(control + 4, &pad)) handed = (int)pad + 1;
        } else if (guest_call(sub_0003AA60, old, 0, NULL)) {
            handed = -1;
        } else if (guest_call(sub_0003AA80, old, 0, NULL)) {
            handed = -2;
        }
        if (handed) guest_call(sub_0003A1F0, old, 0, NULL);
    }
    if (handed > 0) {
        arguments[0] = (uint32_t)handed;
        guest_call(sub_00046D60, actor, 1, arguments);
    } else if (handed == -1) {
        guest_call(sub_00045390, actor, 0, NULL);
    } else if (handed == -2) {
        guest_call(sub_00045450, actor, 0, NULL);
    }
    arguments[0] = actor;
    arguments[1] = 1;
    guest_call(sub_000544D0, screen, 2, arguments);
}

/* The pointer on the 640x480 menu: the frame is letterboxed in the window at
 * its own shape, and a menu is drawn 4:3 in the middle of it. */
static int pointer_on_menu(float *x, float *y)
{
    int px, py, width, height;
    float aspect = nv2a_gpu_presented_aspect(), shown_w, shown_h, menu_w, menu_h;
    if (!xbox_FramebufferPointer(&px, &py, &width, &height)) return 0;
    if (aspect <= 0) aspect = 4.0f / 3;
    shown_h = (float)height;
    shown_w = shown_h * aspect;
    if (shown_w > width) { shown_w = (float)width; shown_h = shown_w / aspect; }
    menu_h = shown_h;
    menu_w = menu_h * 4 / 3;
    if (menu_w > shown_w) { menu_w = shown_w; menu_h = menu_w * 3 / 4; }
    *x = (px - (width - menu_w) / 2) * 640 / menu_w;
    *y = (py - (height - menu_h) / 2) * 480 / menu_h;
    return *x >= 0 && *x < 640 && *y >= 0 && *y < 480;
}

/* Presses made for the pointer, one at a time: held, then let go as long. */
static const char *s_press_target;
static int s_press_count;
static DWORD s_press_next;

static void press(const char *target, int count)
{
    if (s_press_count && strcmp(s_press_target, target)) return;     /* one kind at a time */
    s_press_target = target;
    s_press_count += count;
}

static void press_step(void)
{
    DWORD now = GetTickCount();
    if (!s_press_count || (long)(now - s_press_next) < 0) return;
    xbox_KbmPulse(s_press_target, PRESS_MS);
    s_press_next = now + 2 * PRESS_MS;
    s_press_count--;
}

static void menu_pointer(void)
{
    static int pointer_on;
    static float last_x = -1, last_y = -1;
    static uint32_t last_focus, last_screen;
    static DWORD pointer_busy_until;    /* the pointer's own presses still landing */
    uint32_t screen = s_menu_screen, focus = 0, hovered = 0;
    int on = s_in_menu && screen, pressed, wheel, over, moved;
    DWORD now = GetTickCount();
    float x, y;
    if (on != pointer_on) {
        xbox_FramebufferSetMenuPointer(on);
        pointer_on = on;
        s_press_count = 0;
    }
    if (!on) {
        xbox_FramebufferMouseClicks(0);     /* play's clicks are not a menu's */
        xbox_FramebufferMouseClicks(1);
        return;
    }
    press_step();
    pressed = (xbox_FramebufferMouseClicks(0) ? 1 : 0) | (xbox_FramebufferMouseClicks(1) ? 2 : 0);
    wheel = xbox_KbmMenuWheel();
    over = pointer_on_menu(&x, &y);
    if (over) hovered = entry_at(screen, x, y);
    guest_read(screen + 0x198, &focus);
    moved = over && (x != last_x || y != last_y);

    /* The keys or a pad moved the focus: the arrow goes until the mouse moves. */
    if (s_menu_screens == last_screen && last_focus && focus != last_focus && !moved && !pressed &&
            !s_press_count && (long)(now - pointer_busy_until) >= 0)
        xbox_FramebufferHidePointer();

    /* Moving onto an entry focuses it; a still pointer leaves the keys be. */
    if (hovered && hovered != focus && (pressed || moved)) {
        set_focus(screen, focus, hovered);
        focus = hovered;
    }
    if (over) { last_x = x; last_y = y; }
    last_focus = focus;
    last_screen = s_menu_screens;
    if (pressed || wheel) pointer_busy_until = now + 500;
    if (pressed & 1) {
        if (hovered && !wrap_option(hovered, 1)) press(is_option_row(hovered) ? "RIGHT" : "A", 1);
    } else if (pressed & 2) {
        if (hovered && is_option_row(hovered)) { if (!wrap_option(hovered, 0)) press("LEFT", 1); }
        else press(s_pause_menu ? "START" : "B", 1);
    }
    if (wheel) {
        int row = hovered ? is_option_row(hovered) : focus && is_option_row(focus);
        press(row ? (wheel > 0 ? "RIGHT" : "LEFT") : (wheel > 0 ? "UP" : "DOWN"), wheel > 0 ? wheel : -wheel);
    }
}

/* The PC release's arrow, its tip the hot spot. */
static void load_pc_cursor(void)
{
    char directory[MAX_PATH];
    uint32_t width, height, *pixels;
    if (!pc_game_dir(directory, sizeof directory)) return;
    if (!(pixels = pc_texture(directory, "cursor.dds", &width, &height))) return;
    xbox_FramebufferSetCursorImage(pixels, (int)width, (int)height, (int)(34 * width / 256), (int)(6 * height / 256));
    free(pixels);
}

static DWORD WINAPI video_watch(LPVOID unused)
{
    uint32_t seen = 0;
    int window_fullscreen = 0;
    (void)unused;
    for (;;) {
        uint32_t items, value;
        int s, changed = 0, now_fullscreen;
        Sleep(100);

        /* Alt+Enter: the page follows the window. */
        now_fullscreen = xbox_FramebufferFullscreen();
        if (now_fullscreen != window_fullscreen) {
            window_fullscreen = now_fullscreen;
            if (now_fullscreen != s_values[SET_DISPLAY]) {
                s_values[SET_DISPLAY] = now_fullscreen;
                changed = 1;
                if (seen) item_write(seen, SET_DISPLAY + 1, now_fullscreen);
            }
        }

        items = history_items("video.hst", SETTING_COUNT);
        follow_menus();
        if (items != seen && items) {
            fprintf(stderr, "[VIDEO] options page values at 0x%08X\n", items);
            /* The history starts at the saved settings; one changed since
             * (Alt+Enter) is put back on the page. */
            for (s = 0; s < SETTING_COUNT; s++) item_write(items, s + 1, s_values[s]);
        }
        seen = items;
        for (s = 0; items && s < SETTING_COUNT && guest_read(items + 8u * (uint32_t)s, &value); s++) {
            int v = clamp((int)value, 0, k_settings[s].count - 1);
            if ((int)value != v) item_write(items, s + 1, v);
            if (v == s_values[s]) continue;
            s_values[s] = v;
            apply_setting(s);
            changed = 1;
        }
        if (changed) {
            fprintf(stderr, "[VIDEO]");
            for (s = 0; s < SETTING_COUNT; s++)
                fprintf(stderr, " %s %s", k_settings[s].key, k_settings[s].values[s_values[s]]);
            fprintf(stderr, "\n");
            settings_save();
        }
    }
    return 0;
}

void video_settings_start(const char *game_dir)
{
    /* Turok's rigid programs project through c[96], its skinned ones through
     * c[187] (their bone palette is at c[96] + a0). */
    static const int projections[] = {187, 96};
    HANDLE thread;
    snprintf(s_game_dir, sizeof s_game_dir, "%s", game_dir);
    write_patch(game_dir);
    nv2a_gpu_set_projection_registers(projections, 2);
    apply_aspect();
    xbox_FramebufferSetWindowAspect(aspect_ratio());
    apply_frames();
    load_pc_icons();
    load_pc_cursor();
    apply_setting(SET_ANISOTROPY);
    apply_setting(SET_MSAA);
    apply_setting(SET_ICONS);
    apply_setting(SET_MOUSE_LOOK);
    if (s_values[SET_DISPLAY]) xbox_FramebufferSetFullscreen(1);
    thread = CreateThread(NULL, 0, video_watch, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
}
