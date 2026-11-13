#define main filter_program_main
#include "touchpad_filter.c"
#undef main
#include <assert.h>
static off_t size(int fd) { return lseek(fd,0,SEEK_END); }
static void send_frame(struct filter *f, double now, int tracking, int x, int two, int button) {
    struct buffer b={0};
    append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_SLOT,.value=0});
    if (tracking!=-2) append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=tracking});
    append(&b,(struct input_event){.type=EV_ABS,.code=ABS_X,.value=x});
    if (two) {
        append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_SLOT,.value=1});
        append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=two>0?42:-1});
    }
    if (button>=0) append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=button});
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    off_t before=size(f->out);
    struct input_event *copy=malloc(b.n*sizeof(*copy)); assert(copy);
    size_t count=b.n; memcpy(copy,b.v,count*sizeof(*copy));
    int immediate=f->accepted || !f->session || two>0 || button>=0;
    process_frame(f,&b,now);
    /* Every immediately forwarded frame must retain the original bytes. */
    if (immediate && size(f->out)>before) {
        struct input_event *actual=malloc(count*sizeof(*actual)); assert(actual);
        assert(pread(f->out,actual,count*sizeof(*actual),size(f->out)-(off_t)(count*sizeof(*actual)))==(ssize_t)(count*sizeof(*actual)));
        assert(memcmp(actual,copy,count*sizeof(*actual))==0); free(actual);
    }
    free(copy); free(b.v);
}
int main(void) {
    FILE *output=tmpfile(); assert(output);
    unsigned char active[2]={0};
    struct filter f={.out=fileno(output),.slot_count=2,.active=active,
        .xres=10,.yres=10,.threshold=.03,.min_move2=.09,.click_window=2,
        .tap_max=.25,.tap_slop2=2.25,.step2=.01,.quiet=1,.drag_window=.3};
    /* Only idle one-finger taps are discarded. */
    send_frame(&f,1,1,0,0,-1); send_frame(&f,1.05,-1,0,0,-1);
    assert(size(f.out)==0 && !f.session && !f.pending.n);
    /* Motion releases all pending contact frames; no synthetic button events. */
    send_frame(&f,2,2,0,0,-1); assert(size(f.out)==0);
    send_frame(&f,2.04,-2,3,0,-1);
    assert(f.accepted && f.has_move && !f.pending.n && size(f.out)>0);
    send_frame(&f,2.06,-1,3,0,-1);
    off_t before=size(f.out);
    send_frame(&f,2.1,3,3,0,-1); assert(f.accepted && size(f.out)>before);
    send_frame(&f,2.15,-1,3,0,-1);
    /* Native tap-drag continuation survives the move window expiring. */
    f.last_move=.2;
    send_frame(&f,2.3,4,3,0,-1); assert(f.accepted);
    send_frame(&f,2.8,-2,30,0,-1); send_frame(&f,2.9,-1,30,0,-1);
    /* A quick two-finger tap needs neither movement nor a minimum duration. */
    before=size(f.out);
    send_frame(&f,6,5,30,1,-1);
    assert(f.accepted && size(f.out)>before);
    send_frame(&f,6.005,-1,30,-1,-1);
    /* Native right-click tap-drag continuation is also preserved. */
    send_frame(&f,6.1,6,30,0,-1); assert(f.accepted);
    send_frame(&f,6.5,-1,30,0,-1);
    /* Adding a second finger releases the pending first contact immediately. */
    before=size(f.out);
    send_frame(&f,8,7,30,0,-1); assert(size(f.out)==before);
    send_frame(&f,8.001,-2,30,1,-1); assert(f.accepted && !f.pending.n);
    send_frame(&f,8.01,-1,30,-1,-1);
    /* Physical click retains its native device and bypasses all thresholds. */
    send_frame(&f,10,8,30,0,-1); assert(!f.accepted);
    before=size(f.out);
    send_frame(&f,10.001,-2,30,0,1);
    assert(f.accepted && f.buttons==1 && size(f.out)>before && !f.pending.n);
    send_frame(&f,10.002,-2,30,0,0); assert(!f.buttons);
    send_frame(&f,10.003,-1,30,0,-1);
    /* Button-only events are forwarded, too. */
    before=size(f.out);
    send_frame(&f,11,-2,30,0,1); send_frame(&f,11.01,-2,30,0,0);
    assert(size(f.out)>before);
    /* A discarded contact changes the physical slot, but not the output slot.
     * Subsequent events may omit ABS_MT_SLOT entirely. */
    struct buffer omitted={0};
    append(&omitted,(struct input_event){.type=EV_ABS,.code=ABS_MT_SLOT,.value=1});
    append(&omitted,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=90});
    append(&omitted,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    before=size(f.out);
    process_frame(&f,&omitted,20);
    append(&omitted,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=-1});
    append(&omitted,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&omitted,20.05);
    assert(size(f.out)==before);
    append(&omitted,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=91});
    append(&omitted,(struct input_event){.type=EV_ABS,.code=ABS_X,.value=30});
    append(&omitted,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&omitted,21);
    append(&omitted,(struct input_event){.type=EV_ABS,.code=ABS_X,.value=40});
    append(&omitted,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&omitted,21.04);
    assert(f.accepted && size(f.out)>before);
    struct input_event first;
    assert(pread(f.out,&first,sizeof(first),before)==sizeof(first));
    assert(first.type==EV_ABS && first.code==ABS_MT_SLOT && first.value==1);
    free(omitted.v);
    free(f.pending.v); fclose(output);
    puts("All native forwarding tests passed");
}
