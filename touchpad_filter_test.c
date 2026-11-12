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
        .tap_max=.25,.tap_slop2=2.25,.step2=.01,.quiet=1};
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
    free(f.pending.v); fclose(output); fclose(clicks);
    puts("All synthetic event tests passed");
}
