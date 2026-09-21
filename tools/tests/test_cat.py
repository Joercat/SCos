"""Production guest tests for CAT/Studio/API v2. Keep artifacts under build/."""
import struct,zlib
from qemu_support import Guest

def package(source,id='test-app',permissions=0):
    source=source.encode();p=bytearray(128+len(source));p[:8]=b'SCOSCAT1';struct.pack_into('<7I',p,8,128,len(source),0,2,permissions,560,360);p[36:36+len(id)]=id.encode();p[68:76]=b'Test App';p[128:]=source;struct.pack_into('<I',p,16,zlib.crc32(p));return bytes(p)
def strarg(g,text,offset=0):g.debug.write(g.scratch+offset,text.encode()+b'\0');return g.scratch+offset
def build(g,source,id='test-app',flags=0):
    raw=source.encode();g.debug.write(g.scratch+512,raw);g.debug.write(g.scratch+131072,struct.pack('<32s40sIII4xQI4x',id.encode(),b'Test App',560,360,flags,g.scratch+512,len(raw)));g.debug.write(g.scratch,('home/apps/'+id+'.cat').encode()+b'\0');ok=g.call('cat_build',g.scratch,g.scratch+131072,g.scratch+220000,256);return ok,g.string(g.scratch+220000)
def check(g,source,error=None,flags=0):
    ok,msg=build(g,source,flags=flags);assert ok,msg
    w=g.launch('home/apps/test-app.cat');status=g.state(w)
    if error:assert status[0] and error in status[1],status
    else:assert status==(0,''),status
    g.close(w);assert not g.call('heap_owner_bytes',w)

def test():
  for ide in [False,True]:
    with Guest('cat-ide' if ide else 'cat-usb',ide=ide) as g:
      # 11 visible native applications, 2 internal WM clients, and no application manager: the
      # functions that hub window used to gather belong to the surfaces they act on.
      assert g.call('app_count')==13
      # Raw source and project files are never launchable applications.
      g.save('home/apps/raw.lua','return {}');g.debug.write(g.scratch,b'home/apps/raw.lua\0');assert not g.call('lua_app_install',g.scratch)
      good=package('return {}')
      for payload in [b'',good[:50],good+b'x',good[:-1],bytes(bytearray(good[:130])+b'xx'+good[132:])]:
        g.save('home/apps/bad.cat',payload);g.debug.write(g.scratch,b'home/apps/bad.cat\0');assert not g.call('lua_app_install',g.scratch)
      for offset,value in [(20,99),(24,2),(28,1),(32,4096),(108,1)]:
        bad=bytearray(good);struct.pack_into('<I',bad,offset,value);struct.pack_into('<I',bad,16,0);struct.pack_into('<I',bad,16,zlib.crc32(bad))
        g.save('home/apps/bad.cat',bad);g.debug.write(g.scratch,b'home/apps/bad.cat\0');assert not g.call('lua_app_install',g.scratch)
      g.save('home/apps/collision.cat',package('return {}',id='settings'));g.debug.write(g.scratch,b'home/apps/collision.cat\0');assert not g.call('lua_app_install',g.scratch)
      ok,msg=build(g,'return {paint=function( end}');assert not ok and msg
      assert g.read('home/apps/test-app.cat') is None
      ok,msg=build(g,'return {}');assert ok,msg
      assert g.read('home/apps/test-app.cat')==package('return {}')
      before=g.read('home/apps/test-app.cat');ok,msg=build(g,'return {bad=');assert not ok and g.read('home/apps/test-app.cat')==before
      print('PASS CAT format, raw-source refusal, compiler diagnostics, byte-exact build, failed-build preservation',flush=True)
      source='''assert(scos.version==2)
assert(scos.rgb(1,2,3)==0x010203)
assert(scos.blend(0,0xffffff,100)==0xffffff and scos.blend(0,0xffffff,0)==0)
assert(scos.hit(3,4,0,0,10,10) and not scos.hit(10,10,0,0,10,10))
assert(scos.clamp(200,0,100)==100)
assert(scos.text_width('abc')==24)
local a,b=scos.screen();assert(a>=640 and b>=480)
assert(scos.app_id()=='test-app')
local bytes,limit=scos.memory();assert(bytes>=limit and limit==2097152)
assert(scos.date().year>=2026)
assert(scos.api_info().version==2)
local iw,ih=scos.icon_size();assert(iw==24 and ih==24);scos.set_icon(scos.icons().chart)
assert(type(scos.focused())=='boolean')
assert(scos.notify('API notification'));assert(not scos.notify('throttled'))
assert(scos.write('old.txt','hello'));assert(scos.exists('old.txt'))
assert(scos.file_size('old.txt')==5);assert(scos.rename('old.txt','new.txt'))
assert(scos.read('new.txt')=='hello');assert(#scos.files()==1)
assert(scos.remove('new.txt'));assert(not scos.exists('new.txt'))
assert(#scos.themes()==4)
scos.interval(200)
return {paint=function(w,h)
local t=scos.theme();scos.clear(t.win_bg)
scos.pixel(1,1,0xffffff);scos.frame(8,8,120,30,t.main)
scos.line(8,8,30,30,t.main);scos.circle(160,30,12,t.main)
scos.disc(200,30,12,t.main);scos.gradient(8,48,140,30,t.main,t.win_bg)
scos.icon(0,170,50,t.main);scos.text_scaled(220,50,'CAT',t.text,2)
scos.button(8,90,150,28,'button',false)
scos.checkbox(8,130,'checkbox',true);scos.progress(8,160,200,18,70)
scos.text(8,200,'API v2',t.text)
for width=0,40 do scos.text_clip(8,220,width,'long label',t.text) end
local rows,done=scos.text_wrap(8,240,160,32,'wrapped text',t.text);assert(rows==1 and done)
end}'''
      check(g,source)
      check(g,'scos.minimize();assert(not scos.focused());return {}')
      check(g,'assert(scos.resize(640,400));assert(scos.move(-100,-100));local w=scos.window();assert(w.width==640 and w.height==400 and w.x>=0 and w.y>=0);return {}')
      check(g,'return {paint=function() scos.resize(640,400) end}','paint')
      check(g,'scos.resize(12,12);return {}','range')
      check(g,'scos.theme_apply("blue-sky");return {}','permission')
      check(g,'return {paint=function() scos.disc(0,0,1024,1) end}','drawing budget')
      check(g,'while true do end','budget')
      check(g,'return {paint=function() while true do end end}','budget')
      check(g,'scos.write("../x","x");return {}','filename')
      check(g,'local x=string.rep("a",10000000);return {}','memory')
      print('PASS expanded drawing/control/window/data APIs and runtime failure containment',flush=True)
      # Theme changes require both metadata capability AND real modal approval.
      ok,msg=build(g,'assert(scos.theme_custom("user-test",{main=0x123456,text=0xeeeeee,win_bg=0x202030,bg_top=0x010203,bg_bot=0x303040,mode=3,spacing=24,grid=0x121212}));return {}',flags=1);assert ok,msg
      w=g.launch('test-app');assert g.state(w)==(0,'');assert g.call('wm_dialog_active')
      g.type('\n');assert g.call('theme_count')==5
      theme=g.call('theme_current');assert g.string(g.ptr(theme))=='user-test'
      assert g.read('home/themes/user-test.theme') is not None
      g.close(w)
      # Cancellation must not write or apply the requested theme.
      ok,msg=build(g,'assert(scos.theme_custom("user-cancel",{}));return {}',flags=1);assert ok,msg
      w=g.launch('test-app');g.debug.send('c');g.press('esc');g.pause();assert not g.call('wm_dialog_active');assert g.call('theme_count')==5
      assert g.read('home/themes/user-cancel.theme') is None;g.close(w)
      for i in range(7):
        ok,msg=build(g,'assert(scos.theme_custom("user-slot'+str(i)+'",{mode='+str(i%4)+'}));return {}',flags=1);assert ok,msg
        w=g.launch('test-app');g.type('\n');assert g.call('theme_count')==6+i;g.close(w)
      ok,msg=build(g,'assert(scos.theme_custom("user-overflow",{}));return {}',flags=1);assert ok,msg
      w=g.launch('test-app');g.type('\n');assert g.call('theme_count')==12;assert g.read('home/themes/user-overflow.theme') is None;g.close(w)
      check(g,'assert(scos.theme_status()=="failed");return {}')
      g.call('theme_set_index',4);g.call('settings_save');g.call('wm_theme_changed')
      print('PASS theme cancellation, eight custom slots and overflow without overwrite',flush=True)
      # Short Settings window still exposes the full twelve-theme layout via scrolling.
      settings=g.launch('settings');assert g.call('wm_resize_window',settings,560,300)
      # Native struct app mouse callback, using the same event shape as WM delivery.
      mouse=g.ptr(g.ptr(settings+72)+72);g.debug.write(g.scratch,struct.pack('<BxhhbBBB',3,0,0,-20,0,0,0))
      g.invoke(mouse,settings,g.scratch,20,100);g.run()
      assert int.from_bytes(g.debug.read(g.ptr(settings+80)+12,4),'little')>0
      g.snapshot('settings-short');g.close(settings)
      # Check all native apps remain C clients and repaint against the custom palette.
      windows=[]
      # Eleven native applications, and no manager among them: which apps exist and what each can do is
      # answered by the surfaces (launcher, desktop icons, taskbar, Files), so there is no hub window
      # left to launch, paint or close.
      for name in ['files','terminal','notepad','browser','calendar','settings','about','blackjack','sysmon','solitaire','studio']:
        w=g.launch(name);windows.append(w)
      assert g.call('wm_win_count')==11;g.snapshot('native-apps')
      assert not g.call('app_find', strarg(g,'applications')), 'the Applications manager came back'
      for w in windows:g.close(w)
      print('PASS confirmed persistent custom palette/background and all 11 built-in C app lifecycles',flush=True)
      # Studio: keyboard shortcuts drive its real editor/compiler/save/run path.
      studio=g.launch('studio');g.debug.send('c');g.press('ctrl','s');g.pause()
      assert g.read('home/projects/my-app.project')[:8]==b'SCOSPRJ1'
      g.debug.send('c');g.press('f5');g.pause();assert g.call('wm_win_count')==2
      child=g.call('wm_win_at',1);assert g.state(child)==(0,'')
      assert g.read('home/apps/my-app.cat')[:8]==b'SCOSCAT1'
      g.close(child);g.snapshot('studio');g.close(studio)
      # File association opens a bundled project in native Studio, not raw-source launch.
      demo=g.read('home/demos/counter.cat');assert demo[:8]==b'SCOSCAT1'
      template=bytearray(demo);template[:8]=b'SCOSPRJ1';struct.pack_into('<I',template,16,0);struct.pack_into('<I',template,16,zlib.crc32(template));g.save('home/projects/sample-counter.project',template)
      g.debug.write(g.scratch,b'home/projects/sample-counter.project\0');studio=g.call('app_open_document',g.scratch);assert studio;g.run()
      d=g.ptr(studio+80);original=g.string(g.ptr(d),65536);assert original==template[128:].decode()
      g.type('x');assert g.string(g.ptr(d),65536)=='x'+original
      g.debug.send('c');g.press('ctrl','z');g.pause();assert g.string(g.ptr(d),65536)==original
      g.debug.send('c');g.press('ctrl','y');g.pause();assert g.string(g.ptr(d),65536)=='x'+original
      # Opening another document in a dirty single-instance editor requires approval.
      g.debug.write(g.scratch,b'home/projects/sample-sketch.project\0');assert g.call('app_open_document',g.scratch)==studio
      assert g.call('wm_dialog_active');g.debug.send('c');g.press('esc');g.pause();assert g.string(g.ptr(d),65536)=='x'+original
      g.close(studio);recovery=g.read('home/projects/studio-recovery.project');assert recovery[128:]==('x'+original).encode()
      assert recovery[36:68].split(b'\0')[0]==b'counter'
      g.debug.write(g.scratch,b'home/projects/studio-recovery.project\0');studio=g.call('app_open_document',g.scratch);assert studio;g.run();assert g.string(g.ptr(g.ptr(studio+80)),65536)=='x'+original;g.close(studio)
      g.debug.write(g.scratch,b'home/apps/my-app.cat\0');child=g.call('app_open_document',g.scratch);assert child;g.run();assert g.state(child)==(0,'');g.close(child)
      print('PASS sample projects, document dispatch, Studio undo/redo, discard cancellation and metadata-preserving recovery',flush=True)
      # Demos are not registered until the user explicitly installs them.
      for name in ['counter','sketch']:
        icons_before=g.call('wm_desk_vis_count')
        g.debug.write(g.scratch,name.encode()+b'\0');assert not g.call('app_find',g.scratch)
        path=('home/demos/'+name+'.cat').encode()+b'\0'
        g.debug.write(g.scratch,path);assert not g.call('app_open_document',g.scratch)
        assert g.call('wm_dialog_active');g.debug.send('c');g.press('esc');g.pause()
        assert g.read('home/apps/'+name+'.cat') is None
        g.debug.write(g.scratch,path);g.call('app_open_document',g.scratch);g.type('\n')
        assert g.read('home/apps/'+name+'.cat')==g.read('home/demos/'+name+'.cat')
        assert g.call('wm_desk_vis_count')==icons_before+1
        assert not g.call('wm_dialog_active');assert g.call('wm_win_count')==0
        child=g.launch(name);assert g.state(child)==(0,'');g.close(child)
      print('PASS optional CAT demos: not preinstalled, cancel, install and execute',flush=True)
      if ide:
        g.reboot()  # No manual save: installation itself must have persisted the tree.
        theme=g.call('theme_current');assert g.string(g.ptr(theme))=='user-test'
        assert g.read('home/projects/my-app.project') and g.read('home/apps/my-app.cat')
        assert g.read('home/apps/counter.cat') and g.read('home/apps/sketch.cat')
        w=g.launch('my-app');assert g.state(w)==(0,'');g.close(w)
        print('PASS installer ATA save/reboot: custom theme, Studio project, CAT discovery and execution',flush=True)
      print('PASS Studio save/build/run on '+('IDE/PS2' if ide else 'USB/xHCI'),flush=True)
if __name__=='__main__':test()
