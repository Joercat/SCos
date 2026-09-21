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
      print('PASS held drag/resize/icon/band pixel equivalence, right-click deferral, picker and content capture; vCPUs='+str(cpus),flush=True)
if __name__=='__main__':test()
