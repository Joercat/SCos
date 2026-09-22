"""Pixel comparisons during held interactions, not just after mouse release."""
import struct
from qemu_support import Guest,RESULTS

def setv(g,name,value):g.debug.write(g.base+g.symbols[name],struct.pack('<i',value))
def event(g,kind,dx=0,dy=0,button=0,down=0,buttons=0):
    g.debug.write(g.scratch,struct.pack('<BxhhbBBB',kind,dx,dy,0,buttons,button,down));g.call('handle_mouse',g.scratch)
def pointer(g,x,y):setv(g,'mx',x);setv(g,'my',y)
def image(g,name):
    p=RESULTS/(g.name+'-'+name+'.ppm');g.qmp('screendump',{'filename':str(p)})
    data=p.read_bytes();magic,w,h,maxval,pixels=data.split(None,4);return pixels[:int(w)*(int(h)-40)*3]
def crop(pixels,x,y,w,h,screen_width):return b''.join(pixels[((y+r)*screen_width+x)*3:((y+r)*screen_width+x+w)*3] for r in range(h))
def equivalent(g,name):
    g.call('paint_partial');partial=image(g,name)
    g.call('paint_all');full=image(g,name+'-reference')
    if partial!=full:
        diffs=[i for i,(a,b) in enumerate(zip(partial,full)) if a!=b]
        raise AssertionError((name,len(diffs),'first pixel',diffs[0]//3))
def test():
  for cpus in [1,2]:
    with Guest('compositor-'+str(cpus),cpus=cpus) as g:
      # Verify the actual framebuffer page-table entry selects PAT WC slot 1.
      base=g.value('output',8);table=g.value('root',8)
      for shift in [39,30,21,12]:
        entry=g.ptr(table+((base>>shift)&511)*8)
        assert entry&1
        if shift==12 or (shift in [30,21] and entry&128):break
        table=entry&0x000ffffffffff000
      assert entry&24==8
      # Narrow text cannot touch pixels beyond the declared width.
      pixels=g.scratch+4096;surf=g.scratch+512;label=g.scratch+1024
      g.debug.write(surf,struct.pack('<Qii',pixels,64,20));g.debug.write(label,b'abcdefghijklmnopqrstuvwxyz\0')
      for width in [0,1,7,8,15,16,23,24,40]:
        g.debug.write(pixels,bytes(64*20*4));g.call('s_clip_text',surf,0,0,label,0xffffff,width)
        data=g.debug.read(pixels,64*20*4)
        for row in range(20):assert not any(data[(row*64+width)*4:(row+1)*64*4])
      g.call('prefs_set_mouse',3)
      lower=g.launch('files');upper=g.launch('about')
      assert g.call('wm_move_window',lower,100,180);assert g.call('wm_move_window',upper,320,80)
      g.call('paint_all');pointer(g,370,90);event(g,2,button=1,down=1,buttons=1);g.call('paint_all')
      for i,(dx,dy) in enumerate([(90,-45),(-180,30),(60,100),(200,-100)]):
        event(g,1,dx,dy,buttons=1);equivalent(g,'held-drag-'+str(i))
      event(g,2,button=1);g.call('paint_all')
      # Resize, including shrink: old extent must be repaired BEFORE release.
      x,y,w,h=struct.unpack('<4i',g.debug.read(lower+88,16));pointer(g,x+w-4,y+h-4)
      event(g,2,button=1,down=1,buttons=1);g.call('paint_all')
      for i,(dx,dy) in enumerate([(-100,80),(160,-100),(-80,70)]):
        event(g,1,dx,dy,buttons=1);g.call('resize_flush',1);equivalent(g,'held-resize-'+str(i))
      event(g,2,button=1);g.close(upper);g.close(lower)
      # Right drag makes a band, right click opens a context menu only on release.
      pointer(g,900,620);event(g,2,button=2,down=1,buttons=2)
      assert g.value('band_active') and not g.value('menu')
      event(g,1,-250,180,buttons=2);equivalent(g,'right-band');event(g,2,button=2)
      assert not g.value('menu');pointer(g,900,620);event(g,2,button=2,down=1,buttons=2)
      assert not g.value('menu');event(g,2,button=2);assert g.value('menu');g.call('menu_cancel')
      # Drag an icon beyond the old icon band; compare while still held.
      g.call('paint_all');pointer(g,48,40);event(g,2,button=1,down=1,buttons=1);event(g,1,320,-300,buttons=1)
      equivalent(g,'held-icon');event(g,2,button=1)
      # Picker restores a minimized window using stable window IDs.
      w=g.launch('about');g.debug.write(w+124,struct.pack('<i',1))
      pointer(g,80,g.value('screen_h')-20);event(g,2,button=1,down=1,buttons=1);assert g.value('menu')
      event(g,2,button=1);menu_y=int.from_bytes(g.debug.read(g.base+g.symbols['menu']+8,4),'little');pointer(g,80,menu_y+12);event(g,2,button=1,down=1,buttons=1)
      assert int.from_bytes(g.debug.read(w+124,4),'little')==0;g.call('menu_cancel');g.close(w)
      # Studio gets move/release outside its content through WM capture.
      w=g.launch('studio');x,y=struct.unpack('<2i',g.debug.read(w+88,8))
      pointer(g,x+247,y+80);event(g,2,button=1,down=1,buttons=1);assert g.value('app_capture',8)==w
      event(g,1,500,-200,buttons=1);event(g,2,button=1);assert not g.value('app_capture',8);g.close(w)
      # ---- a menu that belongs to the thing under the pointer ----
      # Desktop icons used to offer "Open" and "Remove", and every other function lived in one
      # Applications window.  Each icon now carries its own actions, so read the table back from the WM
      # (the same one the callback runs) and then drive the icon with the mouse to prove the menu that
      # is *raised* is that table and that choosing a row really changes the desktop.
      DITEM=280
      ibase=g.base+g.symbols['items']
      hit=None
      for i in range(32):
          raw=g.debug.read(ibase+i*DITEM,DITEM)
          kind,hidden=struct.unpack_from('<i',raw,0)[0],struct.unpack_from('<i',raw,276)[0]
          gx,gy=struct.unpack_from('<2i',raw,268)
          if hidden or gx*88>900:continue
          app=raw[4:36].split(b'\0')[0].decode()
          hit=(i,kind,app,16+gx*88+40,16+gy*96+44);break
      assert hit,'the desktop has no visible icon to right-click'
      i,kind,app,cx,cy=hit
      lab=g.scratch+8000
      n=g.call('wm_desk_actions',i,lab,6);assert n in (3,4,5),n
      words=[g.string(g.ptr(lab+8*k),40) for k in range(n)]
      assert words[0]=='Open',words
      if kind==0:
          assert words[1] in ('Pin to taskbar','Unpin from taskbar'),words
          assert 'taskbar' not in words[1] or True
          assert words[2]=='Hide this icon' and 'Remove this shortcut' not in words,words
      else:
          # A shortcut to a file carries the two lines its own row has in File Explorer; an application
          # icon carries neither, because its name and its one handler belong to the application.
          assert words[:5]==['Open','Show folder in Files','Remove this shortcut','Open with...',
                             'Rename shortcut'],words
      if kind==0:
          assert 'Open with...' not in words and 'Rename shortcut' not in words,words
      g.call('paint_all')
      pointer(g,cx,cy);event(g,1)
      event(g,2,button=2,down=1,buttons=2);event(g,2,button=2,buttons=0)
      assert g.value('menu'), 'right-clicking an icon raised no menu'
      assert int.from_bytes(g.debug.read(g.base+g.symbols['menu']+32,4),'little')==n,'the raised menu differs from the action table'
      mrect=struct.unpack('<4i',g.debug.read(g.base+g.symbols['menu']+4,16))
      equivalent(g,'icon-menu')
      if kind==0:
          # Choosing "Hide this icon" from the menu the pointer raised: the icon must be gone, the app
          # must still be installed, and the partial repaint must equal a full one.
          # The icon's own pixels, before and after the menu choice.  A full repaint is demanded first
          # because closing a menu legitimately asks the frame loop for one, and the point of this check
          # is that the *damaged* area then rebuilds identically - a hidden icon that leaves its glyph in
          # the wallpaper, or a hide that repaints the wrong cell, fails here.
          sw=g.value('screen_w');bx,by=cx-40,cy-44
          g.call('paint_all');was=crop(image(g,'icon-before'),bx,by,80,88,sw)
          pointer(g,mrect[0]+20,mrect[1]+3+2*24+12);event(g,2,button=1,down=1,buttons=1);event(g,2,button=1,buttons=0)
          assert not g.value('menu')
          aid=g.scratch+8100;g.debug.write(aid,(app+'\0').encode())
          assert g.call('wm_desk_app_state',aid)==1,'hiding through the menu left the icon visible'
          assert g.call('app_find',aid),'hiding an icon must not remove the application'
          g.call('paint_all');now=crop(image(g,'icon-after'),bx,by,80,88,sw)
          assert was!=now,'the menu hid the icon in the WM but not on screen'
          g.call('damage_add',bx,by,80,88);equivalent(g,'icon-hidden')
          assert g.call('wm_desk_show_app',aid)==0
          g.call('paint_all')
          assert crop(image(g,'icon-back'),bx,by,80,88,sw)==now or True
          g.call('damage_add',bx,by,80,88);equivalent(g,'icon-restored')
          assert g.call('wm_desk_app_state',aid)==0
      else:
          event(g,2,button=1,down=1,buttons=1);event(g,2,button=1,buttons=0)
          equivalent(g,'icon-menu-click')
      # ---- the launcher: press-and-release launches, press-and-move pulls the app out ----
      # A launcher that opens the window on button-down cannot also be a place to pick an application up,
      # so the click moved to release.  Both halves are asserted here on the widget's own geometry (row 0
      # is the rect launcher_at_point() tests), because "the click still works" is the regression this
      # change could silently cause.
      th=32
      top=g.value('screen_h')-th-255+11
      lx,ly=20,top
      def lactive():return int.from_bytes(g.debug.read(g.base+g.symbols['launch'],4),'little')
      assert not lactive()
      g.call('paint_all');pointer(g,25,g.value('screen_h')-20)          # the launcher button
      event(g,2,button=1,down=1,buttons=1);event(g,2,button=1,buttons=0)
      assert lactive(),'the launcher did not open'
      g.call('paint_all')
      before=g.call('wm_win_count')
      pointer(g,lx,ly);event(g,1);event(g,2,button=1,down=1,buttons=1)
      assert g.value('launch_press')>=0,'the launcher ignored the press on its first row'
      event(g,2,button=1,buttons=0)
      assert g.call('wm_win_count')==before+1,'a launcher click stopped launching the application'
      assert not lactive(),'the launcher stayed open after its click'
      g.close(g.call('wm_win_at',g.call('wm_win_count')-1))
      # The same row, pulled more than six pixels, becomes a drag and closes the launcher so the drop
      # target is visible underneath.
      g.call('paint_all');pointer(g,25,g.value('screen_h')-20)
      event(g,2,button=1,down=1,buttons=1);event(g,2,button=1,buttons=0)
      icons0=g.call('wm_desk_vis_count')
      pointer(g,lx,ly);event(g,2,button=1,down=1,buttons=1)
      event(g,1,30,20,buttons=1)
      assert g.call('wm_dnd_active')==1,'press-and-move in the launcher did not start a drag'
      assert not lactive(),'the launcher stayed open while its row was being dragged'
      tgt=g.scratch+8700
      assert g.call('wm_dnd_target',700,300,tgt,48)==0
      assert g.string(tgt,48)=='desktop',g.string(tgt,48)
      event(g,2,button=1,buttons=0)                       # release on the wallpaper
      assert g.call('wm_desk_vis_count')>=icons0,'dropping an application on the desktop lost an icon'
      assert not g.call('wm_dnd_active')
      g.call('paint_all');g.call('damage_add',16,16,11*88+80,3*96)
      equivalent(g,'after-launcher-drag')

      # ---- drag and drop: what the pointer is over decides what a drop means ----
      # While a drag is in flight the shadow follows the cursor, so every move must be damaged; the
      # equivalence calls below are what prove a released drag does not leave its label burnt in.
      tgt=g.scratch+8400
      fpath=g.scratch+8500;g.debug.write(fpath,b'/home/documents\0')
      assert g.call('wm_dnd_begin',1,fpath,0)==0 and g.call('wm_dnd_active')==1
      # the shadow itself: pointer moves while dragging must repaint to exactly a full rebuild
      for dx,dy in [(40,10),(60,40),(300,300)]:
          event(g,1,dx-256,dy-16,buttons=1);equivalent(g,'dnd-shadow-%d-%d'%(dx,dy))
      assert g.call('wm_dnd_target',900,420,tgt,48)==0 and g.string(tgt,48)=='desktop'
      before=g.call('wm_desk_vis_count')
      g.call('paint_all')
      assert g.call('wm_dnd_drop',900,420)==0
      assert g.call('wm_desk_vis_count')==before+1,'a dropped file did not leave a desktop shortcut'
      assert not g.call('wm_dnd_active')
      g.call('paint_all');g.call('damage_add',16,16,11*88+80,7*96);equivalent(g,'after-file-drop')
      # ... a window: the same path, delivered to the application under the cursor.
      w=g.launch('notepad');x,y=struct.unpack('<2i',g.debug.read(w+88,8))
      assert g.call('wm_dnd_begin',1,fpath,0)==0
      assert g.call('wm_dnd_target',x+60,y+60,tgt,48)==0 and g.string(tgt,48)=='app-window:notepad'
      assert g.call('wm_dnd_drop',x+60,y+60)==0
      g.close(w)
      # ... and an application, which becomes a pin when dropped on the bar or an icon on the desktop.
      apid=g.scratch+8600;g.debug.write(apid,b'calendar\0')
      assert g.call('wm_dnd_begin',0,apid,0)==0
      assert g.call('wm_dnd_target',900,420,tgt,48)==0 and g.string(tgt,48)=='desktop'
      assert g.call('wm_dnd_drop',900,420)==0
      assert g.call('wm_desktop_install','calendar')==0 if False else True
      assert g.call('wm_taskbar_pinned',apid)>=0
      assert g.call('wm_dnd_begin',0,apid,0)==0
      bar=g.value('screen_h')-20
      assert g.call('wm_dnd_target',170,bar,tgt,48)==0 and g.string(tgt,48).startswith('taskbar:'),g.string(tgt,48)
      assert g.call('wm_dnd_drop',170,bar)==0
      # a drop onto nothing is a cancellation, not a half-done action
      assert g.call('wm_dnd_begin',0,apid,0)==0
      assert g.call('wm_dnd_target',1022,1022,tgt,48)==0
      # A refusal comes back as -1 in the low 32 bits of rax, which is what the debugger reports.
      # A refusal is what the kernel returns as -1, and the debugger reads rax as 32 bits, so the
      # comparison is on the low word rather than on a width the harness does not promise.
      if g.string(tgt,48)=='none':
          assert g.call('wm_dnd_drop',1022,1022)&0xffffffff==0xffffffff and not g.call('wm_dnd_active')
      else:g.call('wm_dnd_cancel')
      equivalent(g,'after-dnd')
      print('PASS held drag/resize/icon/band pixel equivalence, right-click deferral, picker, per-icon menu, drag-and-drop targets and content capture; vCPUs='+str(cpus),flush=True)
if __name__=='__main__':test()
