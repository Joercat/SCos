"""Retained native-QEMU regression support. No guest test hooks, no host disks.
Run tests from the repository root after make and tools/setup_qemu.py.
Debug calls run only at the foreground idle boundary; GUI events use QMP.
"""
from pathlib import Path
import json, os, re, shlex, shutil, socket, struct, subprocess, time
ROOT=Path(__file__).resolve().parents[2]
RESULTS=ROOT/'build/test-results'
class Debugger:
    def __init__(self,path):
        self.sock=socket.socket(socket.AF_UNIX);self.sock.settimeout(20);self.sock.connect(str(path));self.command('?')
    def receive(self):
        while self.sock.recv(1)!=b'$': pass
        data=b''
        while (c:=self.sock.recv(1))!=b'#': data+=c
        self.sock.recv(2);self.sock.sendall(b'+');return data.decode()
    def send(self,text):
        b=text.encode();self.sock.sendall(b'$'+b+b'#'+f'{sum(b)%256:02x}'.encode());assert self.sock.recv(1)==b'+'
    def command(self,text): self.send(text);return self.receive()
    def read(self,addr,n):
        return b''.join(bytes.fromhex(self.command(f'm{addr+i:x},{min(1024,n-i):x}')) for i in range(0,n,1024))
    def write(self,addr,data):
        for i in range(0,len(data),1024):
            b=data[i:i+1024];assert self.command(f'M{addr+i:x},{len(b):x}:'+b.hex())=='OK'
    def reg(self,i):return int.from_bytes(bytes.fromhex(self.command(f'p{i:x}')),'little')
    def setreg(self,i,v):assert self.command(f'P{i:x}='+v.to_bytes(8 if i<17 else 4,'little').hex())=='OK'
def stage_aside(root=None, name='gsp'):
    """Move a staged `build/gsp' out of the way and hand back where it went.

    The suites that pack their own disk image write firmware into that directory, and `make gsp-firmware' may
    have left real staged firmware there for `make dist' to pack.  A suite that deletes it, or refuses to run
    beside it, punishes someone for staging files - so the directory is moved aside instead and put back in the
    same `finally' that tears the fixture down."""
    base = Path(root) if root else ROOT
    staged, held = base / 'build' / name, base / 'build' / (name + '.staged-aside')
    if staged.exists():
        if held.exists():
            raise AssertionError('build/%s.staged-aside is left over from a run that did not finish: move it '
                                 'back to build/%s, then run this again' % (name, name))
        os.rename(staged, held)
    return held


def restore_staged(held, name='gsp'):
    """Put back what stage_aside() moved, overwriting the fixture's copy of the directory."""
    if held and Path(held).exists():
        final = Path(held).parent / name
        shutil.rmtree(final, ignore_errors=True)
        os.rename(held, final)


class Guest:
    # `devices` are extra emulated PCI functions added to the run, one spec per entry;
    # the GPU detection test uses them to attach display adapters beside the boot one.
    def __init__(self,name='cat',ide=False,cpus=1,devices=(),vga='std'):
        self.name=name;self.ide=ide;self.cpus=cpus;self.devices=list(devices);self.vga=vga
    def __enter__(self):
        RESULTS.mkdir(parents=True,exist_ok=True)
        args=['python3','tools/run_qemu.py','--memory','128','--dry-run']
        args+=['--vga',self.vga]
        for spec in self.devices:args+=['--device',spec]
        if not self.ide:args+=['--usb-boot']
        lines=subprocess.check_output(args,cwd=ROOT,text=True).splitlines();self.directory=Path(lines[0].split(': ',1)[1]);cmd=shlex.split(lines[-1]);cmd=[('pc' if self.ide and x=='q35' else x) for x in cmd if x!='-no-reboot'];cmd+=['-gdb',f'unix:{self.directory}/debug,server=on,wait=off']
        cmd[cmd.index('-smp')+1]=str(self.cpus)
        self.log=(self.directory/'host.log').open('wb');self.process=subprocess.Popen(cmd,stdout=self.log,stderr=self.log)
        try:self.connect();return self
        except BaseException:self.process.terminate();self.process.wait();self.log.close();raise
    def serial(self):return (self.directory/'serial.log').read_text(errors='replace') if (self.directory/'serial.log').exists() else ''
    # Boot now stops at a full-screen sign-in gate before wm_run().  The guest
    # tests do not type on a virtual keyboard: they drive the SAME production
    # entry points the secure screen calls, through the existing GDB debug port.
    # No test-only bypass exists in the kernel; a debugger on a real machine is
    # already physical access.  A persistent ATA save means the account may
    # already exist, so authenticate first and create it only on a fresh boot.
    AUTH_MARKER='security: sign-in required'
    WM_MARKER='wm: entering main loop'
    # The shipped default account: a normal account named root, password scos.
    # No test-only account needs to be created any more.
    TEST_USER=b'root'
    TEST_PASSWORD=b'scos'
    provision_password=b'scos'   # updated by suites that change it before rebooting
    def _wait(self,marker,baseline=0):
        for _ in range(2400):
            if self.serial().count(marker)>baseline:return
            assert self.process.poll() is None;time.sleep(.025)
        raise AssertionError('timed out waiting for %r\n%s'%(marker,self.serial()[-2500:]))
    def _provision_account(self):
        d=self.debug
        d.write(self.scratch,self.TEST_USER+b'\0')
        d.write(self.scratch+64,self.TEST_PASSWORD+b'\0')
        d.write(self.scratch+128,self.TEST_PASSWORD+b'\0')
        d.write(self.scratch+256,b'\0'*192)
        # A fresh image always has the shipped root/scos account.  A reboot that
        # follows a password change is handled by the test that changed it.
        for pw in (self.TEST_PASSWORD, self.provision_password):
            d.write(self.scratch+64,pw+b'\0')
            d.write(self.scratch+256,b'\0'*192)
            if self.call('security_api_authenticate',self.scratch,self.scratch+64,self.scratch+256,192):return
        reason=self.string(self.scratch+256,192) or self.string(self.call('security_api_last_error'),192)
        raise AssertionError('could not sign in as the default account: %s\n%s'%\
                             (reason,self.serial()[-2000:]))
    def connect(self,wm_baseline=0):
        self._wait(self.AUTH_MARKER,getattr(self,'_auth_marker',0))
        self.base=int(re.findall(r'Kernel physical span: (0x[0-9a-f]+)',self.serial())[-1],16)
        self.symbols={p[2]:int(p[0],16) for line in subprocess.check_output(['nm','--defined-only',str(ROOT/'build/kernel.elf')],text=True).splitlines() if len(p:=line.split())==3}
        self.socket=socket.socket(socket.AF_UNIX);self.socket.settimeout(20);self.socket.connect(str(self.directory/'qmp.sock'));self.stream=self.socket.makefile('rb');self.stream.readline();self.qmp('qmp_capabilities');self.debug=Debugger(self.directory/'debug');self.idle();self.scratch=self.call('palloc_owned',262144,0)
        self._provision_account()
        # The harness opts out of the first-launch isolation prompt (exactly as
        # a user can with `choices set isolation.prompt off`) so unrelated UI
        # suites still exercise their original click/launch paths.  The prompt
        # itself is tested explicitly in tools/tests/test_security.py.
        self.debug.write(self.scratch + 4096, b'isolation.prompt\0')
        self.call('security_choice_set', self.scratch + 4096, 0)
        # GDB calls leave the CPU stopped.  Let the secure screen hand over to
        # wm_run(), then stop again at the same idle boundary the old harness
        # used, so every existing test's first g.call() still finds it parked.
        self.run(1.0)
        self._wait(self.WM_MARKER,wm_baseline)
        self._auth_marker=self.serial().count(self.AUTH_MARKER)
    def qmp(self,cmd,args=None):
        self.socket.sendall((json.dumps({'execute':cmd,**({'arguments':args} if args else {})})+'\n').encode())
        while True:
            reply=json.loads(self.stream.readline())
            if 'return' in reply:return reply['return']
            assert 'error' not in reply,reply
    def idle(self):
        a=self.base+self.symbols['cpu_hlt'];assert self.debug.command(f'Z0,{a:x},1')=='OK';self.debug.send('c');assert self.debug.receive().startswith('T05');assert self.debug.command(f'z0,{a:x},1')=='OK'
    def call(self,name,*args):return self.invoke(self.base+self.symbols[name],*args)
    def invoke(self,address,*args):
        d=self.debug;saved=d.command('g');rip=d.reg(16);stack=((d.reg(7)-4096)&~15)-8;d.write(stack,struct.pack('<Q',rip));d.setreg(7,stack);d.setreg(17,d.reg(17)&~512)
        for i,arg in zip([5,4,3,2,8,9],args):d.setreg(i,arg)
        d.setreg(16,address);assert d.command(f'Z0,{rip:x},1')=='OK';d.send('c');assert d.receive().startswith('T05');result=d.reg(0);assert d.command(f'z0,{rip:x},1')=='OK';assert d.command('G'+saved)=='OK';return result
    def pause(self):self.debug.sock.sendall(b'\x03');self.debug.receive();self.idle()
    def run(self,seconds=.2):self.debug.send('c');time.sleep(seconds);self.pause()
    def ptr(self,p):return int.from_bytes(self.debug.read(p,8),'little')
    def value(self,name,n=4):return int.from_bytes(self.debug.read(self.base+self.symbols[name],n),'little')
    def string(self,p,n=256):return self.debug.read(p,n).split(b'\0')[0].decode(errors='replace')
    def save(self,path,data):
        if isinstance(data,str):data=data.encode()
        self.debug.write(self.scratch,path.encode()+b'\0');self.debug.write(self.scratch+512,data);assert self.call('vfs_write',self.scratch,self.scratch+512,len(data))
    def read(self,path):
        self.debug.write(self.scratch,path.encode()+b'\0');p=self.call('vfs_read',self.scratch,self.scratch+256)
        return self.debug.read(p,int.from_bytes(self.debug.read(self.scratch+256,4),'little')) if p else None
    def launch(self,name):
        self.debug.write(self.scratch,name.encode()+b'\0');w=self.call('wm_open_app',self.scratch,0);assert w,name;self.run();return w
    def close(self,w):self.call('wm_close_window',w);self.run()
    def state(self,w):
        d=self.ptr(w+80);return int.from_bytes(self.debug.read(d+28,4),'little'),self.string(d+52,224)
    def press(self,*keys):self.qmp('send-key',{'keys':[{'type':'qcode','data':k} for k in keys],'hold-time':25});time.sleep(.2)
    def type(self,text):
        codes={' ':'spc','\n':'ret','/':'slash','.':'dot','-':'minus'}
        self.debug.send('c')
        for c in text:self.press(codes.get(c,c))
        time.sleep(.15);self.pause()
    def snapshot(self,name):self.qmp('screendump',{'filename':str(RESULTS/(self.name+'-'+name+'.ppm'))})
    def reboot(self):
        before=self.serial().count(self.WM_MARKER);auth_before=getattr(self,'_auth_marker',0)
        assert self.debug.command('D')=='OK';self.debug.sock.close();self.qmp('system_reset');self.stream.close();self.socket.close()
        self._wait(self.AUTH_MARKER,auth_before)
        self.connect(wm_baseline=before)
    def __exit__(self,*args):
        (RESULTS/(self.name+'.log')).write_text(self.serial());self.process.terminate();self.process.wait(timeout=10);self.log.close();self.debug.sock.close();self.stream.close();self.socket.close();shutil.rmtree(self.directory)
