"""Bounded Studio clipboard/selection tests against the actual guest editor."""
import struct,zlib
from qemu_support import Guest
from test_cat import package

def keys(g,*k):
    g.debug.send('c');g.press(*k);g.pause();g.run(.2)
def text(g,w):return g.string(g.ptr(g.ptr(w+80)),65537)
def project(g,source):
    b=bytearray(package(source,id='edit-test'));b[:8]=b'SCOSPRJ1';struct.pack_into('<I',b,16,0);struct.pack_into('<I',b,16,zlib.crc32(b))
    g.save('home/projects/edit-test.project',b);g.debug.write(g.scratch,b'home/projects/edit-test.project\0')
    w=g.call('app_open_document',g.scratch);assert w;g.run();return w

def test():
  for ide in [False,True]:
    with Guest('edit-ide' if ide else 'edit-usb',ide=ide) as g:
      w=project(g,'abcdef\nsecond\n')
      keys(g,'shift','right');keys(g,'shift','right');keys(g,'shift','right');keys(g,'ctrl','c')
      assert g.value('clipboard_len')==3
      keys(g,'ctrl','x');assert text(g,w)=='def\nsecond\n'
      keys(g,'ctrl','z');assert text(g,w)=='abcdef\nsecond\n'
      keys(g,'ctrl','y');assert text(g,w)=='def\nsecond\n'
      keys(g,'ctrl','v');assert text(g,w)=='abcdef\nsecond\n'
      # Navigation without Shift must clear selection, including Home/End.
      keys(g,'ctrl','a');keys(g,'home');g.type('x');assert text(g,w)=='abcdef\nsecond\nx'
      g.close(w)
      exact='a'*16384;w=project(g,exact);keys(g,'ctrl','a');keys(g,'ctrl','c');assert g.value('clipboard_len')==16384
      keys(g,'ctrl','x');assert text(g,w)=='';keys(g,'ctrl','v');assert text(g,w)==exact;g.close(w)
      over='b'*16385;w=project(g,over);keys(g,'ctrl','a');keys(g,'ctrl','x');assert text(g,w)==over
      assert g.value('clipboard_len')==16384
      keys(g,'ctrl','v');assert text(g,w)==exact;keys(g,'ctrl','z');assert text(g,w)==over;g.close(w)
      full='c'*65536;w=project(g,full);keys(g,'ctrl','end');keys(g,'ctrl','v');assert text(g,w)==full
      # Enter at the exact source limit must be rejected as one atomic edit.
      keys(g,'ret');assert text(g,w)==full
      # Field paste cannot overflow its much smaller capacity.
      mouse=g.ptr(g.ptr(w+72)+72);g.debug.write(g.scratch,struct.pack('<BxhhbBBB',2,0,0,0,1,1,1));g.invoke(mouse,w,g.scratch,30,110)
      keys(g,'ctrl','a');keys(g,'ctrl','v');keys(g,'ctrl','s')
      saved=g.read('home/projects/edit-test.project');assert saved[36:68].split(b'\0')[0]==b'edit-test';assert saved[128:]==full.encode()
      g.close(w)
      # Mouse drag selects a range, and replacing it does not erase the document.
      w=project(g,'abcdef');mouse=g.ptr(g.ptr(w+72)+72)
      g.debug.write(g.scratch,struct.pack('<BxhhbBBB',2,0,0,0,1,1,1));g.invoke(mouse,w,g.scratch,246,54)
      g.debug.write(g.scratch,struct.pack('<BxhhbBBB',1,0,0,0,1,0,0));g.invoke(mouse,w,g.scratch,270,54)
      g.debug.write(g.scratch,struct.pack('<BxhhbBBB',2,0,0,0,0,1,0));g.invoke(mouse,w,g.scratch,270,54)
      keys(g,'ctrl','x');assert text(g,w)=='def';keys(g,'ctrl','v');assert text(g,w)=='abcdef';g.close(w)
      print('PASS bounded copy/cut/paste, exact/over limits, atomic edits, field overflow, keyboard and mouse selection: '+('IDE' if ide else 'USB'),flush=True)
if __name__=='__main__':test()
