"""Real guest application lifecycle, Files shortcut, pin and notification checks."""
import ctypes as C,struct
from qemu_support import Guest,RESULTS
from test_cat import package
from test_compositor import event,pointer,image
class Files(C.Structure):
    _fields_=[('path',C.c_char*256),('names',(C.c_char*48)*64),('n',C.c_int),('hover_row',C.c_int),('hover_btn',C.c_int),('synth',C.c_ubyte*64),('notice',C.c_int),('last_tick',C.c_uint64),('last_row',C.c_int),('targets',(C.c_char*192)*64),('scroll',C.c_int)]
def stringarg(g,text,offset=0):g.debug.write(g.scratch+offset,text.encode()+b'\0');return g.scratch+offset
def callstr(g,name,text):return g.call(name,stringarg(g,text))
def signed(v):
    """The debugger hands back a register's low 32 bits, so a kernel -1 arrives as 0xFFFFFFFF."""
    return v-2**32 if v&(1<<31) else v
def setv(g,name,value):g.debug.write(g.base+g.symbols[name],struct.pack('<i',value))
def pointer(g,x,y):setv(g,'mx',x);setv(g,'my',y)
def event(g,kind,dx=0,dy=0,button=0,down=0,buttons=0):
    g.debug.write(g.scratch,struct.pack('<BxhhbBBB',kind,dx,dy,0,buttons,button,down));g.call('handle_mouse',g.scratch)
def click_desktop(g,x,y):
    """A press and release on the wallpaper layer, through the WM's own input entry point.  The position
    goes in through mx/my because a button event carries deltas, not coordinates."""
    pointer(g,x,y);event(g,2,button=1,down=1,buttons=1);event(g,2,button=1,buttons=0)
def rclick(g,w,x,y):
    g.debug.write(g.scratch,struct.pack('<BxhhbBBB',2,0,0,0,2,2,1));g.invoke(g.ptr(g.ptr(w+72)+72),w,g.scratch,x,y)
def menu_row(g,i):
    """Screen rect of one row of the menu the WM currently has raised."""
    x,y,w,h=struct.unpack('<4i',g.debug.read(g.base+g.symbols['menu']+4,16))
    return x,y,w,h,y+3+i*24+12
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
      # 'no hardware backend linked' used to be the only negative SCos could state.  A driver can now
      # be a file on the disk, so the report has to say which of the two negatives this is; on this
      # machine nothing matched the console's adapter at all, and the notification the user saw at boot
      # has to agree with it rather than reassure.
      assert 'CPU software' in report and 'no driver module loaded for this chip' in report and 'QEMU' in report,report
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
      # A freshly opened window has no hover and no selection; if this mirror ever drifts out of step
      # with struct files, that shows up here as a wrong offset rather than as a confusing missing row.
      assert f.hover_row==-1 and f.hover_btn==-1 and f.scroll==0 and f.n>0,(f.hover_row,f.hover_btn,f.scroll,f.n)
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
      # Fill the bar one pin at a time and prove the limit is the limit: the refusal must be about
      # capacity, not about the application - the same refused app pins as soon as another lets go of a
      # slot. Which entries the boot's default pins happened to leave in place must not matter here.
      cap=g.call('wm_taskbar_capacity')
      names=['manage-test','calendar','settings','about','sysmon','notepad','browser','blackjack','solitaire','files','terminal','studio']
      pinned=[]
      for n in names:
        if len(pinned)>=cap:break
        if g.call('wm_taskbar_pinned',stringarg(g,n)):pinned.append(n);continue
        if callstr(g,'wm_taskbar_pin',n):pinned.append(n)
      assert len(pinned)==cap,(len(pinned),cap)
      rest=[n for n in names if n not in pinned]
      assert rest and not callstr(g,'wm_taskbar_pin',rest[0]),'a pin beyond capacity was accepted'
      callstr(g,'wm_taskbar_pin',pinned[-1])
      assert not g.call('wm_taskbar_pinned',stringarg(g,pinned[-1])),'unpinning did not free the slot'
      assert callstr(g,'wm_taskbar_pin',rest[0]),'a pin was still refused after a slot freed up'
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
      # Eleven native apps, two internal WM clients and the installed CAT packages - and no manager,
      # which is why uninstalling must be reachable from the icon and the terminal rather than from a
      # window that lists everything.
      assert g.call('app_count')==13
      assert not g.call('app_uninstall',stringarg(g,'settings'),g.scratch+256,256)
      # Long Notepad lines keep the insertion point visible and accept body clicks.
      g.save('home/documents/long.txt','a'*500)
      note=g.call('wm_open_app',stringarg(g,'notepad'),stringarg(g,'home/documents/long.txt',256));g.run()
      d=g.ptr(note+80);assert int.from_bytes(g.debug.read(d+20,4),'little')>0
      g.debug.send('c');g.press('home');g.pause();g.run();assert int.from_bytes(g.debug.read(d+20,4),'little')==0
      click(g,note,86,40);assert int.from_bytes(g.debug.read(d+16,4),'little')==10;g.close(note)
      # A row of Files gets its own menu, and two of its entries are real work rather than a detour into
      # a terminal: the file is renamed in place, and a document can be handed to a different
      # application.  Both are driven by a right-click on the row and a click on the menu row the user
      # would aim at, so what is proven is the widget, not an internal call made to look like one.
      g.save('home/documents/renamed-me.txt','keep me')
      w=g.call('wm_open_app',stringarg(g,'files'),stringarg(g,'home/documents',256));g.run()
      f=Files.from_buffer_copy(g.debug.read(g.ptr(w+80),C.sizeof(Files)))
      row=next(i for i in range(f.n) if bytes(f.names[i]).split(b'\0')[0]==b'renamed-me.txt')
      rclick(g,w,48,48+row*24+12)
      assert g.value('menu'),'right-clicking a Files row raised no menu'
      items=[g.string(g.ptr(g.base+g.symbols['menu_file_items']+8*k),40) for k in range(5)]
      assert items==['Open','Open with...','Rename...','Pin to Desktop','Delete'],items
      x,y,mw,mh,my_=menu_row(g,2)
      assert x<600,'the rename row of the menu is off screen'
      click_desktop(g,x+20,my_)
      assert g.call('wm_dialog_active'),'Rename... did not open its input dialog'
      g.type('renamed-done.txt\n');g.run(.4)
      assert g.read('home/documents/renamed-done.txt')==b'keep me'
      assert g.read('home/documents/renamed-me.txt') is None,'rename left the old name in place'
      # "Open with..." hands the file to the application chosen from the list the menu builds.
      # The list itself follows the rename: no stale row, and the row menu still resolves.
      f=Files.from_buffer_copy(g.debug.read(g.ptr(w+80),C.sizeof(Files)))
      listed=[bytes(x).split(b'\0')[0] for x in f.names[:f.n]]
      assert b'renamed-done.txt' in listed and b'renamed-me.txt' not in listed,listed
      row=next(i for i in range(f.n) if bytes(f.names[i]).split(b'\0')[0]==b'renamed-done.txt')
      before=g.call('wm_win_count')
      rclick(g,w,48,48+row*24+12)
      x,y,mw,mh,my_=menu_row(g,1)
      click_desktop(g,x+20,my_)
      assert g.value('menu'),'Open with... raised no application list'
      labels=[g.string(g.ptr(g.base+g.symbols['openwith_labels']+8*k),40) for k in range(g.value('openwith_count'))]
      assert 'Notepad' in labels,labels
      pick=labels.index('Notepad')
      x,y,mw,mh,my_=menu_row(g,pick)
      click_desktop(g,x+20,my_)
      g.run(.2)
      # Which slot the new window landed in, and whether a single instance took the file instead of a new
      # window opening, are the compositor's business - so ask what the user would see: a Notepad window
      # whose title names this file.  (w->data holds each app's private struct, and the window title is at
      # struct window + 4, two dereferences past the app pointer at + 72.)
      titles=[]
      for k in range(g.call('wm_win_count')):
        wk=g.call('wm_win_at',k)
        if wk and g.string(g.ptr(g.ptr(wk+72)))=='notepad':titles.append(g.string(wk+4,64))
      assert 'Notepad - renamed-done.txt' in titles,titles
      assert g.call('wm_win_count')>=before
      for k in range(g.call('wm_win_count')):
        wk=g.call('wm_win_at',k)
        if wk and g.string(g.ptr(g.ptr(wk+72)))=='notepad':g.close(wk)
      g.close(w)

      # Uninstall is reached from the icon the application owns, not from a manager window that lists
      # every application: right-click the desktop icon, choose the one line its menu offers for a .cat
      # app, and the same reusable confirmation the terminal drives appears.  Which grid cell the icon is
      # in is read from the desktop's own table, because that is whose business it is.
      ui=install(g,'ui-test')
      DITEM=280;ibase=g.base+g.symbols['items']
      rows=[g.debug.read(ibase+i*DITEM,DITEM) for i in range(g.value('nitems'))]
      target=None
      for i,r in enumerate(rows):   # only the live prefix of the table counts; past nitems is stale memory
        kind,hidden=struct.unpack_from('<i',r,0)[0],struct.unpack_from('<i',r,276)[0]
        if not kind and hidden==0 and r[4:36].split(b'\0')[0]==b'ui-test':target=i;break
      assert target is not None,'the installed app has no desktop icon'
      # Move the icon into a cell on the left of the wallpaper: the notice panel owns the top-right, and
      # an icon under a window or a notice would be that surface's click rather than the desktop's.  The
      # cell is chosen from the desktop's own table, so this does not fight the WM's grid.
      used={(struct.unpack_from('<2i',r,268)[0],struct.unpack_from('<2i',r,268)[1])
            for r in rows if struct.unpack_from('<i',r,276)[0]==0 and struct.unpack_from('<i',r,0)[0]>=0
            and struct.unpack_from('<i',r,0)[0]<=1}
      cell=None
      for gy in range(7):
        for gx in range(1,7):
          if (gx,gy) not in used: cell=(gx,gy);break
        if cell:break
      assert cell,'no free desktop cell to move the icon into'
      raw=bytearray(rows[target]);struct.pack_into('<2i',raw,268,*cell)
      g.debug.write(ibase+target*DITEM,bytes(raw))
      setv(g,'icons_dirty',1);g.call('paint_all')
      for k in range(g.call('wm_win_count')):
        wk=g.call('wm_win_at',k)
        if wk:g.call('wm_move_window',wk,60,420)
      px,py=16+cell[0]*88+40,16+cell[1]*96+44
      g.call('paint_all');pointer(g,px,py);event(g,1)
      event(g,2,button=2,down=1,buttons=2);event(g,2,button=2,buttons=0)
      assert g.value('menu'),'right-clicking the installed app icon raised no menu'
      lab=g.scratch+8000;n=g.call('wm_desk_actions',target,lab,6)
      words=[g.string(g.ptr(lab+8*k),40) for k in range(n)]
      assert words[0]=='Open' and words[-1]=='Uninstall app',words
      mrect=struct.unpack('<4i',g.debug.read(g.base+g.symbols['menu']+4,16))
      before=g.call('wm_win_count')
      click_desktop(g,mrect[0]+20,mrect[1]+3+(n-1)*24+12)
      assert g.call('wm_dialog_active'),'the icon menu did not raise the confirmation'
      g.snapshot('uninstall-from-icon');g.type('\n');g.run()
      assert g.read('home/apps/ui-test.cat') is None,'the confirmed uninstall left the package on disk'
      assert g.call('wm_win_count')==before-1,'the uninstalled app kept its window open'
      after=[g.debug.read(ibase+i*DITEM,DITEM) for i in range(g.value('nitems'))]
      left=[i for i,r in enumerate(after) if struct.unpack_from('<i',r,0)[0]==0 and r[4:36].split(b'\0')[0]==b'ui-test']
      assert not left,('entries survived',left,[struct.unpack_from('<i',after[i],276)[0] for i in left])
      assert signed(g.call('wm_desk_app_state',stringarg(g,'ui-test')))==-1,'its desktop icon survived removal'
      assert not g.call('wm_taskbar_pinned',stringarg(g,'ui-test')),'its taskbar pin survived removal'
      g.call('paint_all');g.snapshot('after-icon-uninstall')
      if ide:
        # app_uninstall itself persists removals; no extra save is called.
        before=g.read('system/desktop.json');g.reboot();assert g.read('system/desktop.json')==before
        assert g.read('home/apps/manage-test.cat') is None
        assert g.read('home/appdata/manage-test/kept.txt')==b'kept'
        assert b'manage-test' not in (g.read('home/taskbar.txt') or b'')
      print('PASS session/durable removal, terminal y/n, GUI cancel, slot reuse, protected builtins, Files app links, adaptive pins, notifications and GPU fallback: '+('IDE' if ide else 'USB'),flush=True)
if __name__=='__main__':test()
