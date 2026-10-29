/* Original SCos drawing primitives; native GOP scanout replaces VBE/MTRR setup. */
#include "scos.h"
struct surface screen;
int screen_w,screen_h;
static struct boot_framebuffer output;
static int clip_on,clip_x,clip_y,clip_r,clip_b;
void fb_scene_clip(int x,int y,int w,int h){clip_on=1;clip_x=x;clip_y=y;clip_r=x+w;clip_b=y+h;}
void fb_scene_unclip(void){clip_on=0;}
void desktop_framebuffer(const struct boot_framebuffer *fb){output=*fb;}
u32 fb_bpp(void){return 32;}
void fb_init(void){
 screen_w=(int)output.width;screen_h=(int)output.height;
 screen=(struct surface){palloc_owned((size_t)screen_w*screen_h*4,HEAP_WM),screen_w,screen_h,0,0,0,0};
 if(!screen.px)panic("desktop framebuffer allocation failed");
}
static u32 component(u32 c,u32 mask){unsigned shift=0;while(!(mask&1)){mask>>=1;shift++;}return ((c*mask/255)<<shift);}
/* --------------------------------------------------- accelerated output path ----
 *
 * The compositor draws every frame into `screen.px` in RAM and then copies the damaged rectangles to
 * the console.  That copy is the one stage a 2D engine can take over, and until now it was offered to
 * the engine only as a **single flat colour**: the previous `engine_output_fill()` scanned the whole
 * rectangle and gave up at the first pixel that differed.  A desktop is almost never one colour - a
 * window is a flat chrome border around a flat background around text, the wallpaper is flat around
 * icons - so on a machine whose driver was bound, self-tested and reported as "engine drives the
 * console", a measured boot with an emulated CL-GD5446 put **0 pixels through the engine and 17,300,000
 * through the CPU**.  The driver was not the problem; the offer was.
 *
 * So a damage rectangle is now decomposed into maximal flat rectangles in one pass over its rows, and
 * those go to the engine; only the pixels no flat rectangle covers are copied by the CPU.  Both halves
 * are counted by `gpu_engine_note_work()`, which is what makes "the GPU is accelerating this" a
 * measurement in the boot report and the test suite rather than an inference from a bound driver.
 *
 * Four rules keep this honest:
 *  - `screen.px` stays the truth buffer.  This decides *how* bytes reach the card, never what they are;
 *    a rectangle the engine will not take is copied by the same loop that always did it.
 *  - A flat run narrower than ENGINE_MIN_RUN, or a rectangle under ENGINE_MIN_AREA pixels, stays on the
 *    CPU: a bitBLT costs a register round trip and a busy-wait, and for a cursor-sized rect that is more
 *    than the copy it replaces.
 *  - A rectangle whose flat area is a small part of it (text, icons, a photo) is copied whole instead:
 *    paying transfers for a few bands and then copying the rest is slower than not asking.
 *  - A transfer that fails is abandoned for the *whole* damage rectangle, which the CPU then copies from
 *    the shadow.  Re-writing pixels the engine did land is harmless - identical bytes - while leaving a
 *    region unpainted is the one outcome this path must never produce.
 */
#define ENGINE_MIN_RUN   12      /* a flat run narrower than this is not worth a transfer */
#define ENGINE_MIN_H     2      /* nor is a rectangle one row tall */
#define ENGINE_MIN_AREA  2048u   /* pixels; below this the register round trip costs more than the copy */
#define ENGINE_MIN_SHARE 8u     /* flat area must be at least 1/8 of the rectangle, else the CPU copies */
#define ENGINE_MAX_RECTS 128    /* flat rectangles one damage rect may decompose into */
#define ENGINE_MAX_CALLS 16     /* transfers issued per damage rect; the smallest rects are dropped first */

struct engine_rect { int x0,x1,y0,y1; u32 color; };
struct engine_open { int x0,x1,y0; u32 color; };

static void copy_row_span(int x,int row,int n)
{
 if(n<=0)return;
 const u32 *src=screen.px+(size_t)row*screen_w+x;
 /* Cast before the arithmetic, not after: `stride' is in pixels, so a void* that had the pixel offset
  * added to it would write row*stride *bytes* into the aperture - which lands in the first few rows of
  * the screen and leaves the row it was meant for untouched. */
 volatile u32 *dst=(volatile u32*)(uintptr_t)output.base+(size_t)row*output.stride+x;
 size_t count=(size_t)n;
 __asm__ volatile("rep movsl":"+S"(src),"+D"(dst),"+c"(count)::"memory");
}

/* End of the run of one colour that starts at `at`, comparing four pixels per iteration while it can.
 * A flat band is the common case and the scan has to stay cheaper than the copy it replaces; a textured
 * row falls out of the wide loop on its first check and finishes in the byte loop below. */
static int run_end(const u32 *row,int at,int w)
{
 u32 v=row[at];
 u64 pair=((u64)v<<32)|(u64)v;
 int cx=at+1;
 while(cx+4<=w){
  u64 a,b;
  __builtin_memcpy(&a,row+cx,8);
  __builtin_memcpy(&b,row+cx+2,8);
  if(a!=pair||b!=pair)break;
  cx+=4;
 }
 while(cx<w&&row[cx]==v)cx++;
 return cx;
}

/* Close one run that has stopped growing into a rectangle, if it is worth a transfer.
 *
 * A full table is not a failure: it means this region is more fragmented than the bound allows, and the
 * rectangles that did not fit are simply left to the CPU copy that follows.  The alternative - abandoning
 * the whole damage rectangle because the *last* run did not fit - is what made a full-screen flip of a
 * real desktop go entirely to the CPU while the small rectangles inside it went to the engine. */
static void engine_emit(struct engine_rect *out,int *rects,const struct engine_open *o,int xbase,int y1)
{
 int w=o->x1-o->x0,h=y1-o->y0;
 if(h<ENGINE_MIN_H)return;
 if((u32)w*(u32)h<ENGINE_MIN_AREA)return;
 if(*rects>=ENGINE_MAX_RECTS)return;
 /* The run coordinates are relative to the damage rectangle (the scan starts at column 0 of `row');
  * everything the caller sees is in scanout space.  Mixing the two is a rectangle painted at the
  * wrong x - the kind of mistake a readback of the same screen cannot catch, because the shadow is
  * still correct. */
 out[(*rects)++]=(struct engine_rect){xbase+o->x0,xbase+o->x1,o->y0,y1,o->color};
}

/* One pass over the rows of the damage rectangle.  Runs of one colour are carried from row to row; a run
 * that is still the same at the next row extends its rectangle, and one that is not is closed.  The open
 * list and the runs of the current row are both ordered by x, so the merge is a walk, not a search.
 * Saturating the open or output bound only costs coverage, never correctness. */
static int engine_decompose(int x,int y,int w,int h,struct engine_rect *out)
{
 static struct engine_open cur[ENGINE_MAX_RECTS],nxt[ENGINE_MAX_RECTS];
 int rects=0,opens=0;
 for(int r=0;r<h;r++){
  const u32 *row=screen.px+(size_t)(y+r)*screen_w+x;
  int oi=0,nopen=0,rx=0;
  while(rx<w){
   int cx=run_end(row,rx,w);
   /* Anything still open that begins before this run cannot be extended by it. */
   while(oi<opens&&cur[oi].x0<rx){
    engine_emit(out,&rects,&cur[oi],x,y+r);
    oi++;
   }
   int extended=0;
   if(oi<opens&&cur[oi].x0==rx){
    if(cur[oi].x1==cx&&cur[oi].color==row[rx]){
     if(nopen<ENGINE_MAX_RECTS)nxt[nopen++]=cur[oi];
     extended=1;
    }else engine_emit(out,&rects,&cur[oi],x,y+r);
    oi++;
   }
   if(!extended&&cx-rx>=ENGINE_MIN_RUN&&nopen<ENGINE_MAX_RECTS)
    nxt[nopen++]=(struct engine_open){rx,cx,y+r,row[rx]};
   rx=cx;
  }
  while(oi<opens){
   engine_emit(out,&rects,&cur[oi],x,y+r);
   oi++;
  }
  for(int i=0;i<nopen;i++)cur[i]=nxt[i];
  opens=nopen;
 }
 while(opens>0){
  opens--;
  engine_emit(out,&rects,&cur[opens],x,y+h);
 }
 return rects;
}

/* The transfers this rectangle gets are the largest of its flat rectangles, biggest first: a fixed budget
 * of round trips should go to the bands that save the most pixels, not to whichever closed first. */
static void sort_by_area(struct engine_rect *r,int count)
{
 for(int i=1;i<count;i++){
  struct engine_rect key=r[i];
  u32 key_area=(u32)(key.x1-key.x0)*(u32)(key.y1-key.y0);
  int j=i-1;
  while(j>=0&&(u32)(r[j].x1-r[j].x0)*(u32)(r[j].y1-r[j].y0)<key_area){
   r[j+1]=r[j];
   j--;
  }
  r[j+1]=key;
 }
}

/* Paint as much of the damage rectangle as the engine will take and copy the remainder, returning the
 * number of pixels the engine painted (0 means it painted nothing and the caller must copy the whole
 * rectangle itself). */
static u32 engine_paint_region(int x,int y,int w,int h){
 static struct engine_rect rects[ENGINE_MAX_RECTS];
 int count=engine_decompose(x,y,w,h,rects);
 if(count<=0)return 0;
 sort_by_area(rects,count);
 if(count>ENGINE_MAX_CALLS)count=ENGINE_MAX_CALLS;
 u32 engine_pixels=0;
 for(int i=0;i<count;i++)
  engine_pixels+=(u32)(rects[i].x1-rects[i].x0)*(u32)(rects[i].y1-rects[i].y0);
 if((u64)engine_pixels*ENGINE_MIN_SHARE<(u64)w*(u64)h)return 0;
 u32 painted=0;
 for(int i=0;i<count;i++){
  const struct engine_rect *r=&rects[i];
  if(gpu_engine_fill_rectangle(r->x0,r->y0,r->x1-r->x0,r->y1-r->y0,r->color)!=0)return 0;
  painted+=(u32)(r->x1-r->x0)*(u32)(r->y1-r->y0);
 }
 if(gpu_engine_wait_idle()!=0)return 0;
 /* What the decomposition left out, one row at a time, so a textured interior costs only the pixels no
  * rectangle covers instead of the whole band.  `at` never moves backwards, so a rectangle already
  * inside a copied gap is skipped rather than copied twice. */
 for(int row=y;row<y+h;row++){
  int at=x,end=x+w;
  for(int i=0;i<count;i++){
   const struct engine_rect *r=&rects[i];
   if(row<r->y0||row>=r->y1)continue;
   if(r->x0>at)copy_row_span(at,row,r->x0-at);
   if(r->x1>at)at=r->x1;
  }
  if(at<end)copy_row_span(at,row,end-at);
 }
 __asm__ volatile("sfence":::"memory");
 return painted;
}

/* One text line of the console is 16 rows by columns*8 pixels of uncached device memory.  On a card
 * with a bit-block transfer engine that move belongs in the GPU's own video RAM, where it is a few
 * register writes, and not in a loop the boot CPU runs a pixel at a time.  A device without an engine,
 * or one that declines the rectangle, takes the loop below; both paths are counted so the boot report
 * says which one did the work. */
int fb_scroll_output(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color){
 if(!gpu_engine_drives_output()||!output.base)return 0;
 if(output.red!=0xff0000||output.green!=0xff00||output.blue!=0xff)return 0;
 if(step==0||w==0||h==0||x+w>output.width||y+h+step>output.height)return 0;
 if(w>0xffff||h>0xffff||step>0xffff)return 0;
 if(gpu_engine_screen_to_screen_blit((int)x,(int)y,(int)w,(int)h,(int)x,(int)(y+step))!=0)return 0;
 /* The band left behind by the move is below the rectangle that was copied, not the last `step`
  * rows of it: the source of the copy reaches down to y+h+step, so that is the row the engine has
  * to paint over.  Getting this wrong scrolls garbage into the visible area instead of blanking. */
 if(gpu_engine_fill_rectangle((int)x,(int)(y+h),(int)w,(int)step,color)!=0)return 0;
 if(gpu_engine_wait_idle()!=0)return 0;
 gpu_engine_note_work(w*(h+step),0);
 return 1;
}

/* The per-pixel loop fb_scroll_output replaces, kept here so the fallback and the first paint of a
 * cleared band share one implementation. */
void fb_scroll_cpu(u32 x,u32 y,u32 w,u32 h,u32 step,u32 color){
 volatile u32 *fb=(void*)(uintptr_t)output.base;
 for(u32 row=0;row<h;row++){
  volatile u32 *d=fb+(size_t)(y+row)*output.stride+x;
  const volatile u32 *src=fb+(size_t)(y+row+step)*output.stride+x;
  for(u32 c=0;c<w;c++)d[c]=src[c];
 }
 for(u32 row=h;row<h+step;row++){
  volatile u32 *d=fb+(size_t)(y+row)*output.stride+x;
  for(u32 c=0;c<w;c++)d[c]=color;
 }
 gpu_engine_note_work(0,w*(h+step));
}
void fb_flip_rect(int x,int y,int w,int h){
 if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
 if(w<=0||h<=0||x>=screen_w||y>=screen_h)return;
 if(w>screen_w-x)w=screen_w-x;
 if(h>screen_h-y)h=screen_h-y;
 volatile u32 *fb=(void*)(uintptr_t)output.base;
 int native=output.red==0xff0000&&output.green==0xff00&&output.blue==0xff;
 /* The engine gets first refusal on every damaged rectangle, not just the flat ones: see the note above
  * engine_paint_region().  When it paints part of a rectangle, the pixels it did not take have already
  * been copied by that function, so the CPU total stays a measurement of what the CPU actually did. */
 if(native&&gpu_engine_drives_output()){
  u32 painted=engine_paint_region(x,y,w,h);
  if(painted){
   gpu_engine_note_work(painted,(u32)w*(u32)h-painted);
   return;
  }
 }
 if(native)gpu_engine_note_work(0,(u32)w*(u32)h);
 for(int r=0;r<h;r++){
  const u32 *src=screen.px+(size_t)(y+r)*screen_w+x;
  volatile u32 *dst=fb+(size_t)(y+r)*output.stride+x;
  if(native){size_t count=(size_t)w;__asm__ volatile("rep movsl":"+S"(src),"+D"(dst),"+c"(count)::"memory");continue;}
  for(int c=0;c<w;c++){
   u32 rgb=src[c];
   dst[c]=native?rgb:
       component((rgb>>16)&255,output.red)|component((rgb>>8)&255,output.green)|component(rgb&255,output.blue);
  }
 }
 __asm__ volatile("sfence":::"memory");
}
void fb_flip(void){fb_flip_rect(0,0,screen_w,screen_h);}
void fb_clear(u32 color)
{
    for (int i = 0; i < screen_w * screen_h; i++) screen.px[i] = color;
}

/* ------------------------------------------------------------ surface ---- */
/* A surface's own clip rectangle, if it has one.  Kept as two small static
 * helpers rather than inline arithmetic at each use so the `cw == 0' rule is
 * written once. */
void s_clip_set(struct surface *s, int x, int y, int w, int h)
{
    int x1 = x + w, y1 = y + h;
    if (!s) return;
    if (!s->cw) { s->cx = x; s->cy = y; s->cw = w; s->ch = h; }
    else {
        if (x > s->cx) s->cx = x;
        if (y > s->cy) s->cy = y;
        if (x1 < s->cx + s->cw) s->cw = x1 - s->cx;
        if (y1 < s->cy + s->ch) s->ch = y1 - s->cy;
    }
    /* `cw == 0' is this struct's "no clip", so an intersection that closes the
     * rectangle completely must still be expressible: keep the rectangle and
     * mark it empty in the second axis. */
    if (s->cw < 0) s->cw = 0;
    if (s->ch < 0) s->ch = 0;
    if (!s->cw || !s->ch) { s->cx = 0; s->cy = 0; s->cw = 1; s->ch = 0; }
}

void s_clip_off(struct surface *s) { if (s) s->cx = s->cy = s->cw = s->ch = 0; }

void s_pixel(struct surface *s, int x, int y, u32 c)
{
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    if(s==&screen&&clip_on&&(x<clip_x||y<clip_y||x>=clip_r||y>=clip_b))return;
    if ((s->cw || s->ch) && (x < s->cx || y < s->cy || x >= s->cx + s->cw || y >= s->cy + s->ch))
        return;
    s->px[y * s->w + x] = c;
}

void s_fill(struct surface *s, int x, int y, int w, int h, u32 c)
{
    if(s==&screen&&clip_on){if(x<clip_x){w-=clip_x-x;x=clip_x;}if(y<clip_y){h-=clip_y-y;y=clip_y;}if(w>clip_r-x)w=clip_r-x;if(h>clip_b-y)h=clip_b-y;}
    if (s->cw || s->ch) {
        if (x < s->cx) { w -= s->cx - x; x = s->cx; }
        if (y < s->cy) { h -= s->cy - y; y = s->cy; }
        if (x + w > s->cx + s->cw) w = s->cx + s->cw - x;
        if (y + h > s->cy + s->ch) h = s->cy + s->ch - y;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if(w<=0||h<=0)return;
    for (int j = 0; j < h; j++) {
        u32 *row = s->px + (y + j) * s->w + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

void s_frame_rect(struct surface *s, int x, int y, int w, int h, u32 c)
{
    s_fill(s, x, y, w, 1, c);
    s_fill(s, x, y + h - 1, w, 1, c);
    s_fill(s, x, y, 1, h, c);
    s_fill(s, x + w - 1, y, 1, h, c);
}

void s_line(struct surface *s, int x0, int y0, int x1, int y1, u32 c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        s_pixel(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void s_circle(struct surface *s, int cx, int cy, int r, u32 c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        s_pixel(s, cx + x, cy + y, c); s_pixel(s, cx - x, cy + y, c);
        s_pixel(s, cx + x, cy - y, c); s_pixel(s, cx - x, cy - y, c);
        s_pixel(s, cx + y, cy + x, c); s_pixel(s, cx - y, cy + x, c);
        s_pixel(s, cx + y, cy - x, c); s_pixel(s, cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void s_disc(struct surface *s, int cx, int cy, int r, u32 c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) s_pixel(s, cx + x, cy + y, c);
}

/* Integer square root, used by the filled shapes below.  Newton's method on
 * integers: no floating point anywhere in the kernel's renderer, so the same
 * figures come out of a CPU with or without an FPU and the host tests that
 * compile this file natively see the same pixels as the machine. */
static unsigned long long isqrt_u64(unsigned long long v)
{
    unsigned long long x = v, y = (x + 1) / 2;
    if (!v) return 0;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return x;
}

/* Ellipse outline (midpoint algorithm) and fill (per-row spans).  Both take
 * radii, not a bounding box: a circle is the special case rx == ry, and an
 * app that wants a rounded button or a gauge should not have to rasterise it
 * out of a thousand pixels through the drawing budget. */
void s_ellipse(struct surface *s, int cx, int cy, int rx, int ry, u32 c)
{
    long long a2, b2, x, y, fx, fy, p;

    if (rx <= 0 || ry <= 0) { s_pixel(s, cx, cy, c); return; }
    a2 = (long long)rx * rx;
    b2 = (long long)ry * ry;
    x = 0; y = ry; fx = 0; fy = 2 * a2 * y;
    p = b2 - a2 * ry + a2 / 4;
    s_pixel(s, cx, cy + (int)y, c); s_pixel(s, cx, cy - (int)y, c);
    while (fx < fy) {
        x++; fx += 2 * b2;
        if (p < 0) p += b2 + fx;
        else { y--; fy -= 2 * a2; p += b2 + fx - fy; }
        s_pixel(s, cx + (int)x, cy + (int)y, c); s_pixel(s, cx - (int)x, cy + (int)y, c);
        s_pixel(s, cx + (int)x, cy - (int)y, c); s_pixel(s, cx - (int)x, cy - (int)y, c);
    }
    p = b2 * x * x + a2 * (y - 1) * (y - 1) - a2 * b2;
    while (y > 0) {
        y--; fy -= 2 * a2;
        if (p > 0) p += a2 - fy;
        else { x++; fx += 2 * b2; p += a2 - fy + fx; }
        s_pixel(s, cx + (int)x, cy + (int)y, c); s_pixel(s, cx - (int)x, cy + (int)y, c);
        s_pixel(s, cx + (int)x, cy - (int)y, c); s_pixel(s, cx - (int)x, cy - (int)y, c);
    }
}

void s_ellipse_fill(struct surface *s, int cx, int cy, int rx, int ry, u32 c)
{
    if (rx <= 0 || ry <= 0) { s_pixel(s, cx, cy, c); return; }
    for (int dy = -ry; dy <= ry; dy++) {
        long long t = (long long)ry * ry - (long long)dy * dy;
        if (t < 0) continue;
        int span = (int)isqrt_u64((unsigned long long)((long long)rx * rx * t) /
                                  (unsigned long long)((long long)ry * ry));
        s_fill(s, cx - span, cy + dy, 2 * span + 1, 1, c);
    }
}

/* Filled rectangle with circular corners: every row is inset by its distance
 * from the corner arc, so the straight middle costs one fill and the corners
 * cost one square root each.  `r' larger than half the box is clamped rather
 * than refused - a caller asking for a pill-shaped button should get a pill. */
void s_rrect(struct surface *s, int x, int y, int w, int h, int r, u32 c)
{
    if (w <= 0 || h <= 0) return;
    int max = (w < h ? w : h) / 2;
    if (r < 0) r = 0;
    if (r > max) r = max;
    if (!r) { s_fill(s, x, y, w, h, c); return; }
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = j < r ? r - 1 - j : (j >= h - r ? j - (h - r) : -1);
        if (dy >= 0) {
            long long t = (long long)r * r - (long long)dy * dy;
            inset = r - (int)isqrt_u64((unsigned long long)(t > 0 ? t : 0));
        }
        s_fill(s, x + inset, y + j, w - 2 * inset, 1, c);
    }
}

/* Filled triangle, scanline with fixed-point edge stepping: the same answer on
 * every row as a barycentric test, at a cost proportional to the area covered
 * rather than to the bounding box. */
void s_triangle(struct surface *s, int x0, int y0, int x1, int y1, int x2, int y2, u32 c)
{
    int ymin = y0 < y1 ? y0 : y1, ymax = y0 > y1 ? y0 : y1;
    if (y2 < ymin) ymin = y2;
    if (y2 > ymax) ymax = y2;
    for (int y = ymin; y <= ymax; y++) {
        int xs[8], n = 0;
        int px[3] = { x0, x1, x2 }, py[3] = { y0, y1, y2 };
        for (int e = 0; e < 3; e++) {
            int ax = px[e], ay = py[e], bx = px[(e + 1) % 3], by = py[(e + 1) % 3];
            if (ay == by) {
                if (y == ay && n + 2 <= 8) { xs[n++] = ax; xs[n++] = bx; }
                continue;
            }
            if ((y < ay && y < by) || (y > ay && y > by)) continue;
            if (n >= 8) break;
            long long num = (long long)(bx - ax) * (y - ay);
            xs[n++] = ax + (int)(num / (by - ay));
        }
        if (n < 2) continue;
        int lo = xs[0], hi = xs[0];
        for (int i = 1; i < n; i++) { if (xs[i] < lo) lo = xs[i]; if (xs[i] > hi) hi = xs[i]; }
        s_fill(s, lo, y, hi - lo + 1, 1, c);
    }
}

static u32 blend(u32 a, u32 b, int t)      /* t 0..255 */
{
    u32 ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    u32 br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    u32 r = (ar * (255 - t) + br * t) / 255;
    u32 g = (ag * (255 - t) + bg * t) / 255;
    u32 bl = (ab * (255 - t) + bb * t) / 255;
    return (r << 16) | (g << 8) | bl;
}

void s_vgrad(struct surface *s, int x, int y, int w, int h, u32 top, u32 bot)
{
    for (int j = 0; j < h; j++) {
        u32 c = blend(top, bot, h > 1 ? (j * 255) / (h - 1) : 0);
        s_fill(s, x, y + j, w, 1, c);
    }
}

/* --------------------------------------------------------------- text ---- */
void s_char_bg(struct surface *s, int x, int y, char ch, u32 fg, u32 bg)
{
    if (!bg) { s_char(s, x, y, ch, fg); return; }
    const u8 *glyph = font8x16[(u8)ch];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < FONT_W; col++)
            s_pixel(s, x + col, y + row, (bits & (0x80 >> col)) ? fg : bg);
    }
}

void s_char(struct surface *s, int x, int y, char ch, u32 fg)
{
    const u8 *glyph = font8x16[(u8)ch];
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = glyph[row];
        if (!bits) continue;
        for (int col = 0; col < FONT_W; col++)
            if (bits & (0x80 >> col)) s_pixel(s, x + col, y + row, fg);
    }
}

void s_text(struct surface *s, int x, int y, const char *str, u32 fg)
{
    int x0 = x;
    while (*str) {
        if (*str == '\n') { x = x0; y += FONT_H + 2; str++; continue; }
        s_char(s, x, y, *str, fg);
        x += FONT_W;
        str++;
    }
}

void s_text_bg(struct surface *s, int x, int y, const char *str, u32 fg, u32 bg)
{
    int x0 = x;
    while (*str) {
        if (*str == '\n') { x = x0; y += FONT_H + 2; str++; continue; }
        s_char_bg(s, x, y, *str, fg, bg);
        x += FONT_W;
        str++;
    }
}

void s_text_scaled(struct surface *s, int x, int y, const char *str, u32 fg, int scale)
{
    while (*str) {
        const u8 *glyph = font8x16[(u8)*str];
        for (int row = 0; row < FONT_H; row++) {
            u8 bits = glyph[row];
            if (!bits) continue;
            for (int col = 0; col < FONT_W; col++) {
                if (bits & (0x80 >> col))
                    s_fill(s, x + col * scale, y + row * scale, scale, scale, fg);
            }
        }
        x += FONT_W * scale;
        str++;
    }
}

int s_text_width(const char *str)
{
    return (int)strlen(str) * FONT_W;
}

void s_clip_text(struct surface *s, int x, int y, const char *str, u32 fg, int max_w)
{
    int n=max_w/FONT_W;if(n<=0)return;if(n>127)n=127;
    char tmp[128];int len=0;
    while(str[len]&&str[len]!='\n'&&str[len]!='\r'&&len<n){tmp[len]=str[len];len++;}
    if(len==n&&str[len]&&str[len]!='\n'&&str[len]!='\r'){
        int dots=n<3?n:3;for(int i=0;i<dots;i++)tmp[n-1-i]='.';
    }
    tmp[len]=0;s_text(s,x,y,tmp,fg);
}

/* Draw flowing text as wrapped rows, and report how many were painted.  `max_rows' bounds the work for a
 * caller whose space is finite (0 means no bound).  A row that no longer fits the surface stops the loop:
 * writing below a surface is not clipping, it is corruption, and the count returned says so by being short.
 */
/* Draw flowing text as wrapped rows and report how many were painted.  The row pitch is the text cell
 * itself, and the surface is the only bound: a caller that wants the text to stop at the bottom of its box
 * hands over a surface whose height *is* the box, which is also how a disagreement between the two becomes
 * visible instead of silent.  A row that would fall outside the surface is not painted, and a surface
 * painted short of the string is the caller's cue that it reserved too little. */
int s_text_wrap(struct surface *s, int x, int y, int width, const char *text, u32 fg)
{
    char row[256];
    int cols = s_text_cols(width), rows = 0;

    if (cols > (int)sizeof(row) - 1) cols = (int)sizeof(row) - 1;
    const char *p = text;
    while (p && y + rows * FONT_H + FONT_H <= s->h) {
        p = s_wrap_row(p, cols, row, sizeof row);
        if (row[0]) s_text(s, x, y + rows * FONT_H, row, fg);
        rows++;
    }
    return rows;
}

/* -------------------------------------------------------------- icons ---- */
void s_icon(struct surface *s, int id, int x, int y, u32 c)
{
    /* 24x24 procedural icons */
    switch (id) {
    case ICON_FOLDER:
        s_fill(s, x + 2, y + 6, 8, 3, c);
        s_frame_rect(s, x + 2, y + 8, 20, 12, c);
        s_fill(s, x + 3, y + 9, 18, 10, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 2, y + 8, 20, 12, c);
        break;
    case ICON_TERMINAL:
        s_frame_rect(s, x + 1, y + 3, 22, 18, c);
        s_line(s, x + 4, y + 8, x + 8, y + 11, c);
        s_line(s, x + 8, y + 11, x + 4, y + 14, c);
        s_fill(s, x + 10, y + 14, 8, 2, c);
        break;
    case ICON_NOTEPAD:
        s_frame_rect(s, x + 4, y + 2, 16, 20, c);
        for (int i = 0; i < 4; i++)
            s_fill(s, x + 7, y + 6 + i * 4, 10, 1, c);
        break;
    case ICON_BROWSER:
        s_circle(s, x + 12, y + 12, 10, c);
        s_line(s, x + 2, y + 12, x + 22, y + 12, c);
        s_line(s, x + 12, y + 2, x + 12, y + 22, c);
        s_circle(s, x + 12, y + 12, 5, c);
        break;
    case ICON_CALENDAR:
        s_frame_rect(s, x + 2, y + 4, 20, 17, c);
        s_fill(s, x + 3, y + 5, 18, 4, c);
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++)
                s_fill(s, x + 5 + j * 4, y + 12 + i * 3, 2, 1, c);
        break;
    case ICON_SETTINGS:
        s_circle(s, x + 12, y + 12, 5, c);
        for (int i = 0; i < 8; i++) {
            int dxs[8] = { 0, 3, 4, 3, 0, -3, -4, -3 };
            int dys[8] = { -4, -3, 0, 3, 4, 3, 0, -3 };
            s_fill(s, x + 12 + dxs[i] - 1, y + 12 + dys[i] * 2 - 1, 3, 3, c);
        }
        s_circle(s, x + 12, y + 12, 8, c);
        break;
    case ICON_INFO:
        s_circle(s, x + 12, y + 12, 10, c);
        s_fill(s, x + 11, y + 10, 2, 8, c);
        s_fill(s, x + 11, y + 6, 2, 2, c);
        break;
    case ICON_SOL:
        s_fill(s, x + 3, y + 6, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 3, y + 6, 12, 16, c);
        s_fill(s, x + 8, y + 4, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 8, y + 4, 12, 16, c);
        s_fill(s, x + 13, y + 2, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 13, y + 2, 12, 16, c);
        break;
    case ICON_CHART:
        s_frame_rect(s, x + 2, y + 3, 20, 18, c);
        s_fill(s, x + 5, y + 12, 3, 6, c);
        s_fill(s, x + 10, y + 8, 3, 10, c);
        s_fill(s, x + 15, y + 5, 3, 13, c);
        break;
    case ICON_CARDS:
        s_fill(s, x + 8, y + 2, 14, 18, c);
        s_frame_rect(s, x + 8, y + 2, 14, 18, c);
        s_fill(s, x + 9, y + 3, 12, 16, (c & 0xFEFEFE) >> 1);
        s_frame_rect(s, x + 4, y + 5, 14, 18, c);
        s_fill(s, x + 5, y + 6, 12, 16, (c & 0xFEFEFE) >> 1);
        s_disc(s, x + 11, y + 14, 2, c);
        break;
    }
}

void s_blit(struct surface *d, struct surface *s, int dx, int dy)
{
    int sx = 0, sy = 0, w = s->w, h = s->h;
    if(d==&screen&&clip_on){if(dx<clip_x){sx=clip_x-dx;w-=sx;dx=clip_x;}if(dy<clip_y){sy=clip_y-dy;h-=sy;dy=clip_y;}if(w>clip_r-dx)w=clip_r-dx;if(h>clip_b-dy)h=clip_b-dy;}
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > d->w) w = d->w - dx;
    if (dy + h > d->h) h = d->h - dy;
    if(w<=0||h<=0)return;
    for (int y = 0; y < h; y++)
        memcpy(d->px + (dy + y) * d->w + dx,
               s->px + (sy + y) * s->w + sx, w * 4);
}

/* The SCos logo: "SC" + spinning 'o' ring + "s", tightly kerned. */
void s_scos_logo(struct surface *s, int x, int y, u32 color, int scale, int phase)
{
    int cw = 8 * scale;
    s_text_scaled(s, x, y, "SC", color, scale);
    int r = 4 * scale + 2;
    int ox = x + 2 * cw + r + 2, oy = y + 6 * scale + r;
    static const int dxs[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const int dys[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    for (int k = 0; k < 8; k++) {
        int rel = (k - phase + 16) % 8;
        int on = rel < 3;
        int rr = (on ? 3 : 2) * scale / 2 + (on ? 1 : 0);
        int px = ox + dxs[k] * r / 10, py = oy + dys[k] * r / 10;
        s_disc(s, px, py, rr, on ? color : ((color >> 2) & 0x3F3F3F));
    }
    s_text_scaled(s, x + 2 * cw + 2 * r + 6, y, "s", color, scale);
}

u32 color_blend(u32 a, u32 b, int t)
{
    return blend(a, b, t);
}
