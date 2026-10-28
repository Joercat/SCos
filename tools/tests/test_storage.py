"""Per-app private storage: separation, quota, and erasure on uninstall.

Storage separation is ALWAYS on, unlike the owner's per-app isolation choice.
An application can name only a file, never a path; the directory comes from the
kernel's own registration.  On top of that the VFS refuses another app's
`home/appdata/root/<id>/` even when a native app calls vfs_* directly, and
uninstalling an app wipes its directory so a reinstall starts clean.
"""
from qemu_support import Guest
from test_cat import package


def stringarg(g, text, offset=0):
    g.debug.write(g.scratch + offset, text.encode() + b'\0')
    return g.scratch + offset


def callstr(g, name, text, offset=0):
    return g.call(name, stringarg(g, text, offset))


def install(g, app_id, source):
    g.save('home/apps/' + app_id + '.cat', package(source, id=app_id))
    assert g.call('lua_app_install', stringarg(g, 'home/apps/' + app_id + '.cat')), app_id


def launch(g, app_id):
    w = g.launch('home/apps/' + app_id + '.cat')
    status = g.state(w)
    assert status == (0, ''), (app_id, status)
    return w


HOSTILE = r"""
local out = {}
local function attempt(name, f)
  local ok, err = pcall(f)
  out[#out+1] = name .. '=' .. (ok and 'ALLOWED' or 'denied')
end
-- Every one of these is a way to name somebody else's private directory.
attempt('parent',  function() local v = scos.read('../victim/secret.txt'); if v then error('LEAK '..v) end end)
attempt('sibling', function() local v = scos.read('victim/secret.txt'); if v then error('LEAK '..v) end end)
attempt('absolute',function() local v = scos.read('/home/appdata/root/victim/secret.txt'); if v then error('LEAK '..v) end end)
attempt('dotslash',function() local v = scos.read('./../victim/secret.txt'); if v then error('LEAK '..v) end end)
attempt('write',   function() scos.write('../victim/planted.txt','pwned') end)
attempt('remove',  function() scos.remove('../victim/secret.txt') end)
attempt('rename',  function() scos.rename('mine.txt','../victim/stolen.txt') end)
attempt('exists',  function() local e = scos.exists('../victim/secret.txt'); if e then error('LEAK exists') end end)
attempt('size',    function() local n = scos.file_size('../victim/secret.txt'); if n then error('LEAK size') end end)
-- Its own namespace still works, and the listing shows only its own files.
attempt('own-write',function() scos.write('mine.txt','hello') end)
local files = scos.files()
table.sort(files)
out[#out+1] = 'files=' .. #files .. ':' .. table.concat(files, ',')
scos.write('report.txt', table.concat(out, ' '))
return {}
"""


def test():
    with Guest('storage') as g:
        # A victim's private file, planted through the kernel (the harness is
        # not an app process).
        # The storage layer creates home/appdata/<account>/<id>/; the shell and
        # app code cannot create it, so the harness asks the same module.
        assert g.call('app_storage_ensure', stringarg(g, 'victim'))
        g.save('home/appdata/root/victim/secret.txt', 'topsecret')

        # ---- pure validators: one place, one answer ----------------------
        def owns(app_id, path):
            g.debug.write(g.scratch + 8192, app_id.encode() + b'\0')
            g.debug.write(g.scratch + 8448, path.encode() + b'\0')
            return g.call('app_storage_owns_path', g.scratch + 8192, g.scratch + 8448)

        assert owns('stor-a', 'home/appdata/root/stor-a') == 1
        assert owns('stor-a', 'home/appdata/root/stor-a/x.txt') == 1
        assert owns('stor-a', 'home/appdata/root/stor-b/x.txt') == 0
        assert owns('stor-a', 'home/appdata/root/stor-a/../stor-b/x.txt') == 0
        assert owns('stor-a', 'home/appdata/root/stor-a/../stor-a/x.txt') == 1
        assert owns('stor-a', 'home/appdata/root/stor-a/../../system/x') == 0
        assert owns('stor-a', 'home/documents/x.txt') == 0
        for bad in ('', '.', '..', '.hidden', 'a/b', 'a b', '../../etc/passwd'):
            g.debug.write(g.scratch + 8192, bad.encode() + b'\0')
            assert g.call('app_storage_name_valid', g.scratch + 8192) == 0, bad
        for bad_id in ('', '.', '..', 'a/b', 'a b', 'x' * 31):
            g.debug.write(g.scratch + 8192, bad_id.encode() + b'\0')
            assert g.call('app_storage_id_valid', g.scratch + 8192) == 0, bad_id
        assert g.call('app_storage_id_valid', stringarg(g, 'stor-a', 8192)) == 1

        # ---- a real app cannot reach another app's data ------------------
        install(g, 'hostile', HOSTILE)
        host = launch(g, 'hostile')
        report = (g.read('home/appdata/root/hostile/report.txt') or b'').decode()
        assert report, 'the hostile app wrote no report'
        for name in ('parent', 'sibling', 'absolute', 'dotslash', 'write',
                     'remove', 'rename', 'exists', 'size'):
            assert name + '=denied' in report, (name, report)
        assert 'own-write=ALLOWED' in report, report
        assert 'files=1:mine.txt' in report, report   # its own listing, nothing else
        assert g.read('home/appdata/root/victim/secret.txt') == b'topsecret', \
            'an app changed another app\'s private file'
        assert g.read('home/appdata/root/victim/planted.txt') is None
        assert g.read('home/appdata/root/victim/stolen.txt') is None
        g.close(host)

        # The victim's namespace is still intact and separate.
        assert g.read('home/appdata/root/hostile/mine.txt') == b'hello'
        assert g.read('home/appdata/root/hostile/secret.txt') is None

        # ---- quota: bytes, then the file count --------------------------
        bytes_app = r"""
local out = {}
local function w(name, n)
  local ok, err = pcall(scos.write, name, string.rep('x', n))
  out[#out+1] = name .. '=' .. (ok and 'ok' or 'quota')
end
w('big', 40000)                       -- one file, under the byte cap
for i = 1, 14 do w(string.format('f%02d', i), 10) end
w('over', 30000)                      -- would break the 64 KiB byte cap
scos.write('bytes-report.txt', table.concat(out, ' '))
return {}
"""
        install(g, 'stor-bytes', bytes_app)
        b = launch(g, 'stor-bytes')
        br = (g.read('home/appdata/root/stor-bytes/bytes-report.txt') or b'').decode()
        assert br, 'the byte-quota app wrote no report'
        assert 'big=ok' in br and 'f14=ok' in br, br
        assert 'over=quota' in br, br
        assert g.read('home/appdata/root/stor-bytes/over') is None
        assert len(g.read('home/appdata/root/stor-bytes/big')) == 40000
        g.close(b)

        files_app = r"""
local out = {}
local function w(name, n)
  local ok, err = pcall(scos.write, name, string.rep('x', n))
  out[#out+1] = name .. '=' .. (ok and 'ok' or 'quota')
end
for i = 1, 17 do w(string.format('f%02d', i), 10) end
scos.remove('f01')                    -- free one slot for the report itself
scos.write('files-report.txt', table.concat(out, ' '))
return {}
"""
        install(g, 'stor-files', files_app)
        f = launch(g, 'stor-files')
        fr = (g.read('home/appdata/root/stor-files/files-report.txt') or b'').decode()
        assert fr, 'the file-quota app wrote no report'
        assert 'f16=ok' in fr and 'f17=quota' in fr, fr
        assert g.read('home/appdata/root/stor-files/f17') is None
        listing = [n for n in ('f%02d' % i for i in range(2, 17))
                   if g.read('home/appdata/root/stor-files/' + n)]
        assert len(listing) == 15, len(listing)
        g.close(f)

        # ---- the isolated (CPL3) host enforces the same quota --------------
        # Isolated apps do not go through the Lua wrapper: their writes are
        # syscalls, and the quota must be the same answer there.  The cgroup
        # limit is deliberately raised first, so only the shared validator can
        # produce the right result.
        iso_app = r"""
local out = {}
local function w(name, n)
  local ok, err = pcall(scos.write, name, string.rep('x', n))
  out[#out+1] = name .. '=' .. (ok and 'ok' or ('refused:' .. tostring(err)))
end
w('big', 40000)
for i = 1, 14 do w(string.format('i%02d', i), 10) end
w('over', 30000)
scos.write('iso-report.txt', table.concat(out, ' '))
return {}
"""
        install(g, 'stor-iso', iso_app)
        g.call('user_set_resource_limit', 2, 1 << 20)
        assert g.call('user_resource_limit', 2) == 1 << 20
        assert g.call('wm_launch_app_override', stringarg(g, 'stor-iso', 8192), 0, 1)
        for _ in range(20):
            g.run(.25)
            if g.read('home/appdata/root/stor-iso/iso-report.txt'):
                break
        ir = (g.read('home/appdata/root/stor-iso/iso-report.txt') or b'').decode()
        assert ir, 'the isolated app wrote no report'
        assert 'big=ok' in ir and 'i14=ok' in ir, ir
        assert 'over=refused' in ir, 'the CPL3 host allowed more than the byte cap'
        # The two hosts must say the same thing: a refusal is loud and worded
        # identically, never a silent no-op that looks like a successful write.
        assert 'app data quota exceeded (16 files / 64 KiB)' in ir, ir
        assert g.read('home/appdata/root/stor-iso/over') is None
        assert g.call('proc_isolation_count') >= 1, 'the app did not run isolated'
        for _ in range(4):
            if not g.call('wm_win_count'):
                break
            g.close(g.call('wm_win_at', 0))
        assert g.call('wm_win_count') == 0, 'the isolated window would not close'
        assert g.call('proc_isolation_count') == 0, 'the CPL3 process outlived its window'
        g.call('user_set_resource_limit', 2, 65536)
        assert g.call('user_resource_limit', 2) == 65536

        # ---- another account never shares that directory -------------------
        # App data is per account as well as per app: the same package run by
        # two accounts has two directories, and the shell cannot read across.
        assert g.call('security_api_create_user', stringarg(g, 'bob', 8192),
                      1, stringarg(g, 'Km4!dV9#pLq2Xz7%', 16384)) == 1
        g.debug.write(g.scratch + 16384, b'\0' * 192)
        g.debug.write(g.scratch + 8192, b'bob\0')
        g.debug.write(g.scratch + 16384, b'Km4!dV9#pLq2Xz7%\0')
        shared = r"""
local who = scos.app_id() .. ':' .. tostring(scos.read('marker.txt'))
scos.write('who.txt', who)
scos.write('marker.txt', 'written')
return {}
"""
        install(g, 'shared-app', shared)
        w = launch(g, 'shared-app')
        assert g.read('home/appdata/root/shared-app/who.txt') == b'shared-app:nil'
        g.close(w)
        # As root, bob's tree does not exist yet and cannot be created by hand.
        assert g.call('vfs_lookup', stringarg(g, 'home/appdata/bob', 8192)) == 0
        assert g.call('vfs_mkdir', stringarg(g, 'home/appdata/bob', 8192)) == 0, \
            'an account may create another account\'s private root'
        g.debug.write(g.scratch + 8192, b'home/appdata/bob/shared-app/planted.txt\0')
        assert g.call('vfs_lookup', g.scratch + 8192) == 0

        # Sign in as bob and run the same package.
        g.debug.write(g.scratch + 8192, b'bob\0')
        g.debug.write(g.scratch + 16384, b'Km4!dV9#pLq2Xz7%\0')
        g.debug.write(g.scratch + 24576, b'\0' * 192)
        assert g.call('security_api_authenticate', g.scratch + 8192,
                      g.scratch + 16384, g.scratch + 24576, 192) == 1
        b = launch(g, 'shared-app')
        assert g.read('home/appdata/bob/shared-app/who.txt') == b'shared-app:nil', \
            'bob saw the other account\'s app data'
        g.close(b)
        # root's copy is invisible here, in the shell as well as in an app.
        assert g.read('home/appdata/root/shared-app/who.txt') is None
        for path in ('home/appdata/root', 'home/appdata/root/shared-app/who.txt',
                     'home/appdata/bob/../root/shared-app/who.txt'):
            g.debug.write(g.scratch + 8192, path.encode() + b'\0')
            assert g.call('vfs_access', g.scratch + 8192, 0) == 0, \
                'bob could read %r' % path
        g.debug.write(g.scratch + 8192, b'home/appdata/bob/shared-app/who.txt\0')
        assert g.call('vfs_access', g.scratch + 8192, 0) == 1

        # Uninstalling as bob erases BOTH accounts' data: the package is
        # registered for the machine, so a reinstall must not come back to
        # either account's saves.
        g.debug.write(g.scratch + 8192, b'shared-app\0')
        g.debug.write(g.scratch + 16384, b'\0' * 256)
        assert g.call('app_uninstall', g.scratch + 8192, g.scratch + 16384, 256) == 1
        for gone in ('home/appdata/bob/shared-app', 'home/appdata/root/shared-app'):
            assert g.call('vfs_lookup', stringarg(g, gone, 8192)) == 0, \
                'uninstall left %r behind' % gone
        # back to root for the rest of the suite
        g.debug.write(g.scratch + 8192, b'root\0')
        g.debug.write(g.scratch + 16384, b'scos\0')
        g.debug.write(g.scratch + 24576, b'\0' * 192)
        assert g.call('security_api_authenticate', g.scratch + 8192,
                      g.scratch + 16384, g.scratch + 24576, 192) == 1

        # ---- uninstall erases the data; reinstall starts clean -----------
        install(g, 'wipe-me', "assert(scos.write('kept.txt','kept'));return {}")
        w = launch(g, 'wipe-me')
        assert g.read('home/appdata/root/wipe-me/kept.txt') == b'kept'
        g.close(w)
        g.debug.write(g.scratch + 8192, b'wipe-me\0')
        g.debug.write(g.scratch + 16384, b'\0' * 256)
        assert g.call('app_uninstall', g.scratch + 8192, g.scratch + 16384, 256) == 1
        msg = g.string(g.scratch + 16384, 256)
        assert 'app data erased' in msg or 'Private app data erased' in msg, msg
        assert 'WARNING' not in msg, msg
        assert g.read('home/apps/wipe-me.cat') is None
        assert g.read('home/appdata/root/wipe-me/kept.txt') is None, 'uninstall kept the data'
        g.debug.write(g.scratch + 8192, b'home/appdata/root/wipe-me\0')
        assert g.call('vfs_lookup', g.scratch + 8192) == 0, \
            'the private directory survived uninstall'

        # Reinstall the same id: the old save must not come back.
        install(g, 'wipe-me', r"""
local kept = scos.read('kept.txt')
scos.write('fresh.txt', kept and 'STALE' or 'clean')
return {}
""")
        again = launch(g, 'wipe-me')
        assert g.read('home/appdata/root/wipe-me/fresh.txt') == b'clean', \
            'a reinstalled app saw the previous installation\'s data'
        assert g.read('home/appdata/root/wipe-me/kept.txt') is None
        g.close(again)

        # A protected built-in cannot be uninstalled and cannot lose its data.
        g.debug.write(g.scratch + 8192, b'settings\0')
        g.debug.write(g.scratch + 16384, b'\0' * 256)
        assert g.call('app_uninstall', g.scratch + 8192, g.scratch + 16384, 256) == 0

    print('PASS per-app storage separation (all traversal shapes denied), flat '
          'namespace, per-account separation, the same 16-file/64 KiB quota and '
          'the same loud refusal in both hosts, and data erased for every '
          'account on uninstall')


if __name__ == '__main__':
    test()
