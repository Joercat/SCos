/*
 * SCos native - in-memory virtual file system.
 *
 * Mirrors the web simulation's localStorage filesystem: a tree of directories
 * and string files. Optionally persisted to an ATA disk image (see ata.c).
 */
#include "scos.h"

struct vfs_node *vfs_root;

static void node_free_recursive(struct vfs_node *n);

static struct vfs_node *node_new(const char *name, int is_dir, struct vfs_node *parent)
{
    struct vfs_node *n = palloc(sizeof(struct vfs_node));
    memset(n, 0, sizeof(*n));
    strncpy(n->name, name, VFS_NAME - 1);
    n->is_dir = is_dir;
    n->parent = parent;
    if (parent) {
        n->sibling = parent->child;
        parent->child = n;
    }
    return n;
}

static struct vfs_node *dir_child(struct vfs_node *dir, const char *name)
{
    for (struct vfs_node *c = dir->child; c; c = c->sibling)
        if (!strcmp(c->name, name)) return c;
    return NULL;
}

void vfs_init_defaults(void)
{
    if (vfs_root) node_free_recursive(vfs_root);
    vfs_root = node_new("", 1, NULL);

    struct vfs_node *home = node_new("home", 1, vfs_root);
    struct vfs_node *docs = node_new("documents", 1, home);
    node_new("downloads", 1, home);
    node_new("desktop", 1, home);

    static const char welcome[] =
        "Welcome to SCos!\nThis is your personal computer system.\n"
        "Now running as a native x86 kernel.";
    static const char changelog[] =
        "SCos 2.0.0 Changelog:\n"
        "- Rebuilt from web simulation into a real bare-metal kernel\n"
        "- VBE framebuffer desktop with mouse-driven window manager\n"
        "- PS/2 keyboard and mouse drivers, PIT clock, CMOS RTC\n"
        "- Terminal, Files, Notepad, Calendar, Settings, About apps\n"
        "- Optional ATA disk persistence ('save' in the terminal)";

    struct vfs_node *f;
    f = node_new("welcome.txt", 0, docs);
    f->data = palloc(sizeof(welcome)); f->size = sizeof(welcome) - 1; f->cap = sizeof(welcome);
    memcpy(f->data, welcome, f->size); f->data[f->size] = 0;
    f = node_new("changelog.txt", 0, docs);
    f->data = palloc(sizeof(changelog)); f->size = sizeof(changelog) - 1; f->cap = sizeof(changelog);
    memcpy(f->data, changelog, f->size); f->data[f->size] = 0;

    struct vfs_node *sys = node_new("system", 1, vfs_root);
    static const char settings[] = "{\n  \"theme\": \"matrix-1\",\n  \"version\": \"2.0.0\"\n}";
    f = node_new("settings.json", 0, sys);
    f->data = palloc(sizeof(settings)); f->size = sizeof(settings) - 1; f->cap = sizeof(settings);
    memcpy(f->data, settings, f->size); f->data[f->size] = 0;
    static const char about[] =
        "SCos - Simulated Computer Operating System\n"
        "Version 2.0.0 (native kernel)\n"
        "Developed with heart by the SCos team";
    f = node_new("about.txt", 0, sys);
    f->data = palloc(sizeof(about)); f->size = sizeof(about) - 1; f->cap = sizeof(about);
    memcpy(f->data, about, f->size); f->data[f->size] = 0;
    static const char network[] =
        "{\n"
        "  \"google.com\": { \"ip\": \"172.217.160.142\", \"status\": \"online\", \"latency\": \"45ms\" },\n"
        "  \"example.com\": { \"ip\": \"93.184.216.34\", \"status\": \"online\", \"latency\": \"60ms\" },\n"
        "  \"local.net\": { \"ip\": \"192.168.1.1\", \"status\": \"online\", \"latency\": \"5ms\" },\n"
        "  \"offline.site\": { \"ip\": \"10.0.0.1\", \"status\": \"offline\", \"latency\": \"timeout\" }\n"
        "}";
    f = node_new("network.json", 0, sys);
    f->data = palloc(sizeof(network)); f->size = sizeof(network) - 1; f->cap = sizeof(network);
    memcpy(f->data, network, f->size); f->data[f->size] = 0;
}

struct vfs_node *vfs_lookup(const char *path)
{
    if (!path) return NULL;
    struct vfs_node *cur = vfs_root;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return vfs_root;
    char part[VFS_NAME];
    while (*p) {
        const char *slash = p;
        while (*slash && *slash != '/') slash++;
        int len = (int)(slash - p);
        if (len == 0) { p = slash + 1; continue; }
        if (len >= VFS_NAME) return NULL;
        strncpy(part, p, len);
        part[len] = 0;
        if (!strcmp(part, ".")) { /* stay */ }
        else if (!strcmp(part, "..")) { if (cur->parent) cur = cur->parent; }
        else {
            cur = dir_child(cur, part);
            if (!cur) return NULL;
        }
        p = slash;
        while (*p == '/') p++;
    }
    return cur;
}

int vfs_is_dir(struct vfs_node *n) { return n && n->is_dir; }

int vfs_list(struct vfs_node *dir, char names[][VFS_NAME], int max)
{
    int count = 0;
    if (!dir || !dir->is_dir) return 0;
    /* directories first, then files (creation order reversed internally) */
    for (int pass = 0; pass < 2; pass++)
        for (struct vfs_node *c = dir->child; c; c = c->sibling) {
            if ((pass == 0) != (c->is_dir != 0)) continue;
            if (count >= max) return count;
            strcpy(names[count++], c->name);
        }
    return count;
}

char *vfs_read(const char *path, u32 *len)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n || n->is_dir) return NULL;
    if (len) *len = n->size;
    return n->data;
}

static void node_set_data(struct vfs_node *n, const char *data, u32 len)
{
    if (n->cap < len + 1) {
        u32 cap = len + 1 + 64;
        char *nd = palloc(cap);
        n->data = nd;
        n->cap = cap;
    }
    memcpy(n->data, data, len);
    n->data[len] = 0;
    n->size = len;
}

int vfs_write(const char *path, const char *data, u32 len)
{
    char dirpath[256];
    vfs_parent_path(path, dirpath);
    struct vfs_node *dir = vfs_lookup(dirpath);
    if (!dir || !dir->is_dir) return 0;

    const char *base = path + strlen(path);
    while (base > path && *(base - 1) != '/') base--;
    if (!*base) return 0;

    struct vfs_node *n = dir_child(dir, base);
    if (!n) n = node_new(base, 0, dir);
    if (n->is_dir) return 0;
    node_set_data(n, data, len);
    return 1;
}

int vfs_mkdir(const char *path)
{
    char dirpath[256];
    vfs_parent_path(path, dirpath);
    struct vfs_node *dir = vfs_lookup(dirpath);
    if (!dir || !dir->is_dir) return 0;
    const char *base = path + strlen(path);
    while (base > path && *(base - 1) != '/') base--;
    if (!*base) return 0;
    if (dir_child(dir, base)) return 0;
    node_new(base, 1, dir);
    return 1;
}

static void node_free_recursive(struct vfs_node *n)
{
    struct vfs_node *c = n->child;
    while (c) {
        struct vfs_node *next = c->sibling;
        node_free_recursive(c);
        c = next;
    }
    if (n->data) pfree(n->data, n->cap);
    pfree(n, sizeof(*n));
}

int vfs_delete(const char *path)
{
    struct vfs_node *n = vfs_lookup(path);
    if (!n || n == vfs_root || !n->parent) return 0;
    struct vfs_node *p = n->parent;
    struct vfs_node *prev = NULL;
    for (struct vfs_node *c = p->child; c; prev = c, c = c->sibling)
        if (c == n) break;
    if (prev) prev->sibling = n->sibling; else p->child = n->sibling;
    node_free_recursive(n);
    return 1;
}

u32 vfs_usage_bytes(void)
{
    u32 total = 0;
    /* walk iteratively */
    struct vfs_node *stack[64];
    int sp = 0;
    stack[sp++] = vfs_root;
    while (sp) {
        struct vfs_node *n = stack[--sp];
        total += n->size + (u32)strlen(n->name);
        for (struct vfs_node *c = n->child; c; c = c->sibling)
            if (sp < 64) stack[sp++] = c;
    }
    return total;
}

char *vfs_parent_path(const char *path, char *out)
{
    int len = (int)strlen(path);
    int i = len - 1;
    while (i >= 0 && path[i] == '/') i--;
    while (i >= 0 && path[i] != '/') i--;
    if (i < 0) { out[0] = '/'; out[1] = 0; return out; }
    strncpy(out, path, i + 1);
    out[i + 1] = 0;
    return out;
}
