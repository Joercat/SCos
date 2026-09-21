"""Real guest application lifecycle, Files shortcut, pin and notification checks."""
import ctypes as C,struct
from qemu_support import Guest,RESULTS
from test_cat import package
from test_compositor import event,pointer,image
class Files(C.Structure):
    _fields_=[('path',C.c_char*256),('names',(C.c_char*48)*64),('n',C.c_int),('hover_row',C.c_int),('hover_btn',C.c_int),('synth',C.c_ubyte*64),('notice',C.c_int),('last_tick',C.c_uint64),('last_row',C.c_int),('targets',(C.c_char*192)*64),('scroll',C.c_int)]
def stringarg(g,text,offset=0):g.debug.write(g.scratch+offset,text.encode()+b'\0');return g.scratch+offset
def callstr(g,name,text):return g.call(name,stringarg(g,text))
def click(g,w,x,y):
    g.debug.write(g.scratch,struct.pack('<BxhhbBBB',2,0,0,0,1,1,1));g.invoke(g.ptr(g.ptr(w+72)+72),w,g.scratch,x,y)
def install(g,id='manage-test'):
    g.save('home/apps/'+id+'.cat',package("assert(scos.write('kept.txt','kept'));return {}",id=id))
    w=g.launch('home/apps/'+id+'.cat');assert g.state(w)==(0,'');callstr(g,'wm_desktop_install',id);return w

def crop(pixels,x,y,w,h,screen_width):return b''.join(pixels[((y+r)*screen_width+x)*3:((y+r)*screen_width+x+w)*3] for r in range(h))

def test():
  for ide in [False,True]:
    with Guest('manage-ide' if ide else 'manage-usb',ide=ide) as g:
      g.call('graphics_report',g.scratch,1024);report=g.string(g.scratch,1024)
      assert 'CPU software' in report and 'no hardware backend' in report and 'QEMU' in report,report
      assert g.value('notice_count')>=1
      g.call('paint_all');initial=crop(image(g,'warning'),g.value('screen_w')-330,10,320,108,g.value('screen_w'))
      # A notification must overlay a modal, and dismiss without resolving it.
      g.call('wm_dialog',stringarg(g,'Modal',0),stringarg(g,'Do not resolve',256),0,0,0)
      modal=g.call('wm_win_at',0);g.call('wm_move_window',modal,g.value('screen_w')-330,10);g.call('paint_all')
      assert crop(image(g,'warning-over-modal'),g.value('screen_w')-330,10,320,108,g.value('screen_w'))==initial
      g.call('wm_notify',stringarg(g,'Overlay'),stringarg(g,'Above the modal',256),1);g.call('paint_all')
      before=g.call('wm_win_count');pointer(g,g.value('screen_w')-30,10+(g.value('notice_count')-1)*114+10)
      event(g,2,button=1,down=1,buttons=1);assert g.call('wm_win_count')==before and g.call('wm_dialog_active')
      g.debug.send('c');g.press('esc');g.pause();g.run();assert not g.call('wm_dialog_active')
      # Desktop paths with aliases must use stable IDs, not label reconstruction.
      w=g.call('wm_open_app',stringarg(g,'files'),stringarg(g,'home//desktop/../desktop',256));assert w;g.run()
      f=Files.from_buffer_copy(g.debug.read(g.ptr(w+80),C.sizeof(Files)))
      row=next(i for i in range(f.n) if f.synth[i]==1 and bytes(f.targets[i]).split(b'\0')[0]==b'about')
      click(g,w,48,52+row*24);click(g,w,48,52+row*24);g.run()
      assert g.call('wm_win_count')==2
      child=g.call('wm_win_at',1);assert g.string(g.ptr(g.ptr(child+72)))=='about';g.close(child);g.close(w)
      # Protected directory produces only its notice, not the empty-directory line.
      w=g.call('wm_open_app',stringarg(g,'files'),stringarg(g,'/system',256));g.run();f=Files.from_buffer_copy(g.debug.read(g.ptr(w+80),C.sizeof(Files)));assert f.notice==1 and f.n==0;g.snapshot('files-system');g.close(w)
      # Real shortcut pinning, adaptive capacity and full-screen notification paint.
      wide=g.call('wm_taskbar_capacity');screenw=g.value('screen_w')
      g.debug.write(g.base+g.symbols['screen_w'],struct.pack('<i',640));narrow=g.call('wm_taskbar_capacity');g.debug.write(g.base+g.symbols['screen_w'],struct.pack('<i',screenw));assert 0<narrow<wide<=8
      a=install(g);b=g.launch('manage-test');assert callstr(g,'wm_taskbar_pin','manage-test')
      assert callstr(g,'wm_taskbar_pinned','manage-test')
      for name in ['calendar','settings','about']:
        assert callstr(g,'wm_taskbar_pin',name)
      assert not callstr(g,'wm_taskbar_pin','sysmon')
      # GUI uninstall cancellation cannot remove files or stop windows.
      callstr(g,'app_request_uninstall','manage-test');assert g.call('wm_dialog_active')
      g.debug.send('c');g.press('esc');g.pause();g.run();assert g.read('home/apps/manage-test.cat')
      term=g.launch('terminal');g.type('appuninstall manage-test\n');g.run(.3)
      tab=g.ptr(g.ptr(term+80));assert g.string(tab,256)=='appuninstall manage-test'
      g.type('n\n');g.run(.3);assert g.read('home/apps/manage-test.cat')
      g.type('appuninstall manage-test\n');g.type('y\n');g.run(.4)
      assert g.read('home/apps/manage-test.cat') is None
      assert g.read('home/appdata/manage-test/kept.txt')==b'kept'
      assert not callstr(g,'app_find','manage-test') and not callstr(g,'wm_taskbar_pinned','manage-test')
      assert g.call('wm_win_count')==1;g.close(term)
      # Reuse the same slot more than the registration cap, without rebooting.
      for i in range(34):
        g.save('home/apps/reuse.cat',package('return {}',id='reuse'))
        assert callstr(g,'lua_app_install','home/apps/reuse.cat')
        assert g.call('app_uninstall',stringarg(g,'reuse'),g.scratch+256,256)
      assert g.call('app_count')==14
      assert not g.call('app_uninstall',stringarg(g,'settings'),g.scratch+256,256)
      # Long Notepad lines keep the insertion point visible and accept body clicks.
      g.save('home/documents/long.txt','a'*500)
      note=g.call('wm_open_app',stringarg(g,'notepad'),stringarg(g,'home/documents/long.txt',256));g.run()
      d=g.ptr(note+80);assert int.from_bytes(g.debug.read(d+20,4),'little')>0
      g.debug.send('c');g.press('home');g.pause();g.run();assert int.from_bytes(g.debug.read(d+20,4),'little')==0
      click(g,note,86,40);assert int.from_bytes(g.debug.read(d+16,4),'little')==10;g.close(note)
      ui=install(g,'ui-test');w=g.launch('applications')
      click(g,w,60,72+12*28+8);click(g,w,260,24);assert g.call('wm_dialog_active');g.type('\n');g.run()
      assert g.read('home/apps/ui-test.cat') is None and g.call('wm_win_count')==1
      g.snapshot('applications');g.close(w)
      if ide:
        # app_uninstall itself persists removals; no extra save is called.
        before=g.read('system/desktop.json');g.reboot();assert g.read('system/desktop.json')==before
        assert g.read('home/apps/manage-test.cat') is None
        assert g.read('home/appdata/manage-test/kept.txt')==b'kept'
        assert b'manage-test' not in (g.read('home/taskbar.txt') or b'')
      print('PASS session/durable removal, terminal y/n, GUI cancel, slot reuse, protected builtins, Files app links, adaptive pins, notifications and GPU fallback: '+('IDE' if ide else 'USB'),flush=True)
if __name__=='__main__':test()
