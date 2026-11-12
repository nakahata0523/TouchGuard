#define main filter_program_main
#include "touchpad_filter.c"
#undef main
#include <assert.h>
static void send_frame(struct filter *f, double now, int tracking, int x, int two) {
    struct buffer b={0};
    append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_SLOT,.value=0});
    if (tracking != -2) append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=tracking});
    append(&b,(struct input_event){.type=EV_ABS,.code=ABS_X,.value=x});
    if (two) {
        append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_SLOT,.value=1});
        append(&b,(struct input_event){.type=EV_ABS,.code=ABS_MT_TRACKING_ID,.value=two>0?42:-1});
    }
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(f,&b,now); free(b.v);
}
static off_t size(int fd) { return lseek(fd,0,SEEK_END); }
int main(void) {
    FILE *output=tmpfile(), *clicks=tmpfile(); assert(output && clicks);
    unsigned char active[2]={0};
    struct filter f={.out=fileno(output),.click=fileno(clicks),.slot_count=2,.active=active,
        .xres=10,.yres=10,.threshold=.1,.min_move2=1,.click_window=2,
        .tap_max=.25,.tap_slop2=2.25,.step2=.01,.quiet=1,.drag_window=.3};
    send_frame(&f,1,1,0,0); send_frame(&f,1.05,-1,0,0);
    assert(!f.session && size(f.out)==0 && size(f.click)==0);
    send_frame(&f,2,2,0,0); send_frame(&f,2.11,-2,10,0);
    assert(f.accepted && f.has_move && size(f.out)>0 && f.pending.n==0);
    send_frame(&f,2.12,-1,10,0); off_t before=size(f.out);
    send_frame(&f,2.2,3,10,0); send_frame(&f,2.25,-1,10,0);
    assert(size(f.out)==before && size(f.click)==4*(off_t)sizeof(struct input_event));
    before=size(f.click);
    send_frame(&f,5,4,10,0); send_frame(&f,5.05,-1,10,0);
    assert(size(f.click)==before);
    send_frame(&f,6,5,10,1); send_frame(&f,6.11,-2,10,0);
    assert(f.accepted && f.max_fingers==2);
    send_frame(&f,6.12,-1,10,-1); assert(!f.session && f.fingers==0);
    /* A physical press stays down across motion and suppresses a duplicate tap. */
    before=size(f.click);
    struct buffer b={0};
    append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=1});
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&b,7);
    assert(f.buttons==1 && size(f.click)==before+2*(off_t)sizeof(struct input_event));
    send_frame(&f,7.01,6,10,0);
    send_frame(&f,7.12,-2,30,0);
    assert(f.accepted && f.buttons==1);
    send_frame(&f,7.13,-1,30,0);
    append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=0});
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&b,7.14);
    assert(!f.buttons && size(f.click)==before+4*(off_t)sizeof(struct input_event));
    send_frame(&f,7.2,7,30,0);
    append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=1});
    append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=0});
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&b,7.21);
    before=size(f.click);
    send_frame(&f,7.22,-1,30,0);
    assert(size(f.click)==before);
    /* Tap, second contact, motion, lift: exactly one held click. */
    f.last_move=8; f.has_move=1; f.tap_armed=0;
    send_frame(&f,8.01,8,30,0); send_frame(&f,8.05,-1,30,0);
    assert(f.tap_armed);
    before=size(f.click);
    send_frame(&f,8.15,9,30,0);
    assert(f.dragging && f.accepted && !f.tap_armed);
    send_frame(&f,8.16,-2,31,0);
    assert(f.dragging && size(f.click)==before+2*(off_t)sizeof(struct input_event));
    send_frame(&f,8.2,-1,31,0);
    assert(!f.dragging && size(f.click)==before+4*(off_t)sizeof(struct input_event));
    struct input_event held[4];
    assert(pread(f.click,held,sizeof(held),before)==sizeof(held));
    assert(held[0].code==BTN_LEFT && held[0].value==1 && held[2].value==0);
    /* An expired second contact does not start dragging. */
    send_frame(&f,8.3,10,31,0); send_frame(&f,8.35,-1,31,0);
    assert(f.tap_armed);
    send_frame(&f,8.7,11,31,0); assert(!f.dragging && !f.accepted);
    send_frame(&f,8.75,-1,31,0);
    /* Two fingers release the synthetic hold. */
    send_frame(&f,8.8,12,31,0); assert(f.dragging);
    send_frame(&f,8.81,-2,31,1); assert(!f.dragging);
    send_frame(&f,8.82,-1,31,-1);
    /* A physical left press takes ownership until physical release. */
    f.tap_armed=1; f.last_tap=9;
    send_frame(&f,9.1,13,31,0); assert(f.dragging);
    before=size(f.click);
    append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=1});
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&b,9.11);
    assert(size(f.click)==before);
    send_frame(&f,9.12,-1,31,0);
    assert(!f.dragging && f.buttons==1 && size(f.click)==before);
    append(&b,(struct input_event){.type=EV_KEY,.code=BTN_LEFT,.value=0});
    append(&b,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
    process_frame(&f,&b,9.13);
    assert(!f.buttons && size(f.click)==before+2*(off_t)sizeof(struct input_event));
    free(b.v);
    free(f.pending.v); fclose(output); fclose(clicks);
    puts("All synthetic event tests passed");
}
