/*
 * Decoy files: bait, planted on purpose, at every layer of the system.
 *
 * The owner asked for fake, useless files "from the kernel to a file in the home
 * directory" so that a hostile package has something specific to reach for and
 * the isolation test has something specific to fail on.  That is exactly what
 * these are: a filename a real attacker would try first (`boot.key',
 * `accounts.db', `passwords.txt', another account's `backup.key', another app's
 * `notes.txt'), filled with a body that says in plain words that it is not a
 * secret and that nothing reads it.
 *
 * Two properties make them useful rather than decorative:
 *
 *   * They are in the REAL tree, behind the REAL guards.  A probe that reads
 *     one from an isolated package is not reading a fixture the sandbox knows
 *     about - it is reading a path the VFS protects like any other, and the
 *     answer it gets is the answer the guard gives.
 *   * They do not contain anything.  If a sandboxed app ever does read one, the
 *     report says so, the suite fails, and nothing of the machine's has leaked
 *     in the meantime.  A decoy that held a secret would be a liability; a
 *     decoy that holds its own name is a tripwire.
 *
 * They are planted when the file tree is initialised, before the account
 * database exists, so the kernel-side write is the same unprivileged one that
 * builds `home/documents'.  They are never re-planted over an edit: a file that
 * someone changed by hand stays changed, and `decoys_present' says which ones
 * still match what was planted.
 */
#include "scos.h"

struct decoy_file {
    const char *path;
};

/* Ordered, because the isolation test names its probes by index
 * (`decoy3_read=CONTAINED') and the suite maps each index back to a path
 * through decoys_path().  Adding one here means adding one there, which is the
 * point: the list is the specification of what the sandbox is expected to
 * refuse. */
static const struct decoy_file decoy_files[] = {
    { "kernel/symbols.map" },                          /* the kernel itself */
    { "kernel/drivers/gsp.key" },
    { "system/keys/boot.key" },                        /* boot "signing" bait */
    { "system/security/accounts.db" },                 /* the account database */
    { "system/security/keys.bin" },                    /* not the real db, nor a key */
    { "vault/volume.key" },                            /* the disk encryption key */
    { "home/root-notes.txt" },                         /* a plain file in home */
    { "home/documents/passwords.txt" },
    { "home/users/root/backup.key" },                  /* another account's tree */
    { "home/appdata/root/notepad/notes.txt" },         /* another app's storage */
};

int decoys_count(void) { return (int)(sizeof decoy_files / sizeof decoy_files[0]); }

const char *decoys_path(int i)
{
    if (i < 0 || i >= decoys_count()) return 0;
    return decoy_files[i].path;
}

/* One body for all of them, with the path in it so a leaked copy is traceable
 * to the bait it came from.  The buffer is shared and the caller must not keep
 * the pointer across another call - the suite reads one at a time. */
const char *decoys_body(int i)
{
    static char body[256];
    if (i < 0 || i >= decoys_count()) return "";
    sc_copy(body, sizeof body, "SCOS-DECOY 1 ");
    sc_cat(body, sizeof body, decoy_files[i].path);
    sc_cat(body, sizeof body,
           "\nPlanted on purpose. This holds no key, no password and no\n"
           "data of yours, and nothing in the system reads it. A sandboxed\n"
           "app that can read or change this file has escaped its sandbox.\n");
    return body;
}

/* Is the file still the decoy that was planted?  1 = identical, 0 = changed or
 * missing.  Used by the shell's `decoys' command, the tester app's report and
 * the suite, so all three tell the same story about the same bytes. */
int decoys_present(int i)
{
    const char *path = decoys_path(i);
    const char *body = decoys_body(i);
    struct vfs_node *n;
    size_t len;
    if (!path) return 0;
    /* vfs_lookup(), not vfs_read(): some decoys are planted inside protected
     * metadata (system/security), where a read through the account guard is
     * refused - correctly - and the kernel-side check must still be able to say
     * whether the bytes are the ones that were planted.  The kernel calls this
     * with proc_current() == 0, which is the only context that can see them. */
    n = vfs_lookup(path);
    if (!n || n->is_dir || !n->data) return 0;
    len = strlen(body);
    return n->size == len && !memcmp(n->data, body, len);
}

static void decoy_mkdir(const char *path)
{
    if (!vfs_lookup(path)) vfs_mkdir(path);
}

/* Called from system_files_init(), i.e. after the saved tree has been replayed
 * and before the account database is opened. */
void decoys_plant(void)
{
    decoy_mkdir("kernel");
    decoy_mkdir("kernel/drivers");
    decoy_mkdir("system");
    decoy_mkdir("system/keys");
    decoy_mkdir("system/security");
    decoy_mkdir("vault");
    decoy_mkdir("home/documents");
    decoy_mkdir("home/users");
    decoy_mkdir("home/users/root");
    decoy_mkdir("home/appdata");
    decoy_mkdir("home/appdata/root");
    decoy_mkdir("home/appdata/root/notepad");
    for (int i = 0; i < decoys_count(); i++) {
        const char *path = decoy_files[i].path;
        const char *body = decoys_body(i);
        if (!vfs_lookup(path))
            if (!vfs_write(path, body, (u32)strlen(body)))
                klog("decoys: could not plant %s", path);
    }
}

/* Shell support: `decoys' prints the list with whether each still matches. */
int decoys_describe(char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < decoys_count() && n < cap - 1; i++) {
        const char *p = decoy_files[i].path;
        while (*p && n < cap - 1) out[n++] = *p++;
        if (n < cap - 1) out[n++] = decoys_present(i) ? '+' : '!';
        if (n < cap - 1) out[n++] = '\n';
    }
    if (n < cap) out[n] = 0;
    return n;
}
