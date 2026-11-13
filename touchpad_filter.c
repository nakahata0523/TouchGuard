#define _POSIX_C_SOURCE 200809L
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>

#define BITS (8 * sizeof(unsigned long))
#define WORDS(n) (((n) + BITS - 1) / BITS)
#define HAS(a,n) (((a)[(n)/BITS] >> ((n)%BITS)) & 1UL)
#define MAX_BUFFER_EVENTS 1048576U
struct buffer { struct input_event *v; size_t n, cap; };
struct filter {
    int out, slot, slot_min, slot_count, fingers;
    unsigned char *active;
    double x, y, xres, yres, start_x, start_y, start, last_move;
    double threshold, min_move2, click_window, tap_max, tap_slop2, step2;
    double max_distance2, drag_window, continuation_until;
    int session, accepted, max_fingers, has_move, quiet;
    unsigned int buttons;
    int physical_used;
    struct buffer pending;
};
static volatile sig_atomic_t stopping;
static void stop_handler(int sig) { (void)sig; stopping = 1; }
static void die(const char *s) { perror(s); exit(EXIT_FAILURE); }
static void append(struct buffer *b, struct input_event ev) {
    if (b->n == MAX_BUFFER_EVENTS) { fprintf(stderr, "Event buffer limit exceeded\n"); exit(1); }
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        void *p = realloc(b->v, cap * sizeof(*b->v));
        if (!p) die("realloc");
        b->v = p; b->cap = cap;
    }
    b->v[b->n++] = ev;
}
static void write_events(int fd, const struct input_event *v, size_t n) {
    const char *p = (const char *)v;
    size_t bytes = n * sizeof(*v);
    while (bytes) {
        ssize_t r = write(fd, p, bytes);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) die("write uinput");
        p += r; bytes -= (size_t)r;
    }
}
static double distance2(struct filter *f, double x, double y) {
    double dx = x / f->xres, dy = y / f->yres;
    return dx * dx + dy * dy;
}
static void emit(int fd, struct buffer *b) {
    if (b->n) write_events(fd, b->v, b->n);
}
/* Keep native button events and contact information together, so libinput can
 * apply clickfinger, button areas, tapping and dragging itself. Only an idle
 * single-finger contact is withheld while its intent is unknown. */
static void process_frame(struct filter *f, struct buffer *frame, double now) {
    /* evdev omits ABS_MT_SLOT while the selected slot is unchanged. A dropped
     * contact may have changed that selection only on the physical device.
     * Make every frame self-contained before buffering or forwarding it. */
    if (frame->n) {
        append(frame,(struct input_event){0});
        memmove(frame->v+1,frame->v,(frame->n-1)*sizeof(*frame->v));
        frame->v[0]=(struct input_event){.type=EV_ABS,
            .code=ABS_MT_SLOT,.value=f->slot};
    }
    int previous_fingers=f->fingers, had_button=f->buttons!=0, updated=0;
    double previous_x=f->x, previous_y=f->y;
    for (size_t i=0; i<frame->n; i++) {
        struct input_event *ev=&frame->v[i];
        if (ev->type==EV_KEY &&
            (ev->code==BTN_LEFT || ev->code==BTN_RIGHT || ev->code==BTN_MIDDLE)) {
            unsigned int bit=1U << (ev->code-BTN_LEFT);
            had_button=1;
            if (ev->value==1) f->buttons|=bit;
            else if (ev->value==0) f->buttons&=~bit;
        }
        if (ev->type!=EV_ABS) continue;
        switch (ev->code) {
        case ABS_MT_SLOT: f->slot=ev->value; break;
        case ABS_MT_TRACKING_ID: {
            int64_t index=(int64_t)f->slot-f->slot_min;
            if (index<0 || index>=f->slot_count) {
                fprintf(stderr,"Invalid multitouch slot\n"); exit(1);
            }
            int active=ev->value>=0;
            f->fingers+=active-f->active[index];
            f->active[index]=(unsigned char)active;
            break;
        }
        case ABS_X: f->x=ev->value; updated=1; break;
        case ABS_Y: f->y=ev->value; updated=1; break;
        }
    }
    int started=!previous_fingers && f->fingers;
    int ended=previous_fingers && !f->fingers;
    if (started) {
        f->session=1; f->start=now; f->start_x=f->x; f->start_y=f->y;
        f->max_fingers=f->fingers; f->max_distance2=0;
        f->physical_used=had_button; f->pending.n=0;
        int recent=f->has_move && now>=f->last_move && now-f->last_move<=f->click_window;
        int continuation=now<=f->continuation_until;
        f->accepted=recent || continuation;
        if (f->accepted && !f->quiet)
            printf("TOUCH ALLOW: %s\n",recent ? "recent motion" : "drag continuation");
        f->continuation_until=0;
    }
    if (f->session) {
        if (had_button) f->physical_used=1;
        if (f->fingers>f->max_fingers) f->max_fingers=f->fingers;
        if (previous_fingers>f->max_fingers) f->max_fingers=previous_fingers;
        /* ABS_X/Y in a lift-off frame need not describe a live contact.
         * Do not let those coordinates authorize this or the next tap. */
        if (f->fingers && f->max_fingers==1) {
            double d=distance2(f,f->x-f->start_x,f->y-f->start_y);
            if (d>f->max_distance2) f->max_distance2=d;
        }
        double duration=now-f->start;
        int moving=f->max_fingers==1 && f->max_distance2>=f->min_move2;
        /* These bypass the single-finger gate without any time threshold. */
        int bypass=had_button || f->max_fingers>=2;
        if (!f->accepted && (moving || bypass)) {
            f->accepted=1;
            if (!f->quiet)
                printf("TOUCH ALLOW: %s, displacement=%.3f mm, duration=%.1f ms, fingers=%d\n",
                    had_button ? "button event" : f->max_fingers>=2 ? "multiple fingers" : "motion",
                    sqrt(f->max_distance2),duration*1000,f->max_fingers);
            emit(f->out,&f->pending); f->pending.n=0;
        }
        if (f->accepted && f->fingers && f->max_fingers==1 && updated &&
            duration>=f->threshold && moving &&
            (started || distance2(f,f->x-previous_x,f->y-previous_y)>=f->step2)) {
            f->last_move=now; f->has_move=1;
        }
        if (f->accepted) emit(f->out,frame);
        else for (size_t i=0;i<frame->n;i++) append(&f->pending,frame->v[i]);
        if (ended) {
            /* Let the OS handle the continuation of a native tap/drag,
             * including a multi-finger tap followed by a one-finger drag. */
            if (f->accepted && !f->physical_used && duration<=f->tap_max &&
                (f->max_fingers>=2 || f->max_distance2<=f->tap_slop2))
                f->continuation_until=now+f->drag_window;
            if (!f->quiet)
                printf("TOUCH END: %s, displacement=%.3f mm, duration=%.1f ms, fingers=%d, button=%d\n",
                    f->accepted ? "forwarded" : "dropped",sqrt(f->max_distance2),
                    duration*1000,f->max_fingers,f->physical_used);
            f->session=0; f->accepted=0; f->pending.n=0;
        }
    } else emit(f->out,frame);
    frame->n=0;
}
static int check_ioctl(int fd, unsigned long request, unsigned long arg) {
    int r=ioctl(fd, request, arg);
    if (r<0) die("ioctl");
    return r;
}
static int new_uinput(void) {
    int fd=open("/dev/uinput", O_WRONLY|O_CLOEXEC);
    if (fd<0) die("open /dev/uinput");
    return fd;
}
static int clone_device(int dev, struct input_absinfo *absinfo) {
    unsigned long types[WORDS(EV_CNT)]={0}, codes[WORDS(KEY_CNT)]={0};
    unsigned long props[WORDS(INPUT_PROP_CNT)]={0};
    int fd=new_uinput();
    check_ioctl(dev, EVIOCGBIT(0, sizeof(types)), (unsigned long)types);
    for (unsigned int type=1; type<EV_CNT; type++) {
        if (!HAS(types,type) || type==EV_MSC) continue;
        unsigned long request;
        unsigned int count;
        switch (type) {
        case EV_KEY: request=UI_SET_KEYBIT; count=KEY_CNT; break;
        case EV_ABS: request=UI_SET_ABSBIT; count=ABS_CNT; break;
        case EV_REL: request=UI_SET_RELBIT; count=REL_CNT; break;
        case EV_SW: request=UI_SET_SWBIT; count=SW_CNT; break;
        default: fprintf(stderr,"Unsupported input event type: %u\n",type); exit(1);
        }
        check_ioctl(fd, UI_SET_EVBIT, type);
        memset(codes,0,sizeof(codes));
        check_ioctl(dev,EVIOCGBIT(type,sizeof(codes)),(unsigned long)codes);
        for (unsigned int code=0; code<count; code++) if (HAS(codes,code)) {
            check_ioctl(fd,request,code);
            if (type==EV_ABS) {
                struct uinput_abs_setup setup={.code=(unsigned short)code};
                check_ioctl(dev,EVIOCGABS(code),(unsigned long)&setup.absinfo);
                absinfo[code]=setup.absinfo;
                check_ioctl(fd,UI_ABS_SETUP,(unsigned long)&setup);
            }
        }
    }
    check_ioctl(dev,EVIOCGPROP(sizeof(props)),(unsigned long)props);
    for (unsigned int i=0;i<INPUT_PROP_CNT;i++) if (HAS(props,i)) check_ioctl(fd,UI_SET_PROPBIT,i);
    struct uinput_setup setup={0};
    char name[UINPUT_MAX_NAME_SIZE]={0};
    check_ioctl(dev,EVIOCGNAME(sizeof(name)),(unsigned long)name);
    snprintf(setup.name,sizeof(setup.name),"%.58s (filtered touchpad)",name);
    check_ioctl(dev,EVIOCGID,(unsigned long)&setup.id);
    check_ioctl(fd,UI_DEV_SETUP,(unsigned long)&setup);
    check_ioctl(fd,UI_DEV_CREATE,0);
    return fd;
}
static void usage(const char *name) {
    printf("Usage: %s [options] /dev/input/eventN\n"
           "  --threshold-ms N     default 30\n  --min-move-mm N      default 1.5\n"
           "  --click-window-ms N  default 1500\n  --tap-max-ms N       default 250\n"
           "  --tap-slop-mm N      default 1.5\n  --motion-step-mm N   default 0.1\n"
           "  --drag-window-ms N   default 300\n  --quiet             disable event logging (default)\n  --verbose           enable event logging\n",name);
}
int main(int argc, char **argv) {
    struct filter f={.quiet=1};
    double values[]={30,1.5,1500,250,1.5,0.1,300};
    const struct option opts[]={
        {"threshold-ms",1,0,0}, {"min-move-mm",1,0,1}, {"click-window-ms",1,0,2},
        {"tap-max-ms",1,0,3}, {"tap-slop-mm",1,0,4}, {"motion-step-mm",1,0,5},
        {"drag-window-ms",1,0,6}, {"quiet",0,0,'q'}, {"verbose",0,0,'v'}, {"help",0,0,'h'}, {0,0,0,0}
    };
    int opt;
    while ((opt=getopt_long(argc,argv,"qvh",opts,NULL))!=-1) {
        if (opt=='h') { usage(argv[0]); return 0; }
        if (opt=='q') { f.quiet=1; continue; }
        if (opt=='v') { f.quiet=0; continue; }
        if (opt<0 || opt>6) return 2;
        char *end; errno=0; double v=strtod(optarg,&end);
        if (errno || end==optarg || *end || !isfinite(v) || v<=0 || !isfinite(v*v) || v*v==0) {
            fprintf(stderr,"Thresholds must be finite positive numbers\n"); return 2;
        }
        values[opt]=v;
    }
    if (optind+1!=argc) { usage(argv[0]); return 2; }
    f.threshold=values[0]/1000; f.min_move2=values[1]*values[1];
    f.click_window=values[2]/1000; f.tap_max=values[3]/1000;
    f.tap_slop2=values[4]*values[4]; f.step2=values[5]*values[5];
    f.drag_window=values[6]/1000;
    int dev=open(argv[optind],O_RDONLY|O_CLOEXEC);
    if (dev<0) die("open input device");
    unsigned long absbits[WORDS(ABS_CNT)]={0}, keys[WORDS(KEY_CNT)]={0}, active[WORDS(KEY_CNT)]={0};
    check_ioctl(dev,EVIOCGBIT(EV_ABS,sizeof(absbits)),(unsigned long)absbits);
    check_ioctl(dev,EVIOCGBIT(EV_KEY,sizeof(keys)),(unsigned long)keys);
    if (!HAS(absbits,ABS_MT_SLOT)||!HAS(absbits,ABS_MT_TRACKING_ID)||!HAS(absbits,ABS_X)||!HAS(absbits,ABS_Y)||!HAS(keys,BTN_LEFT)) {
        fprintf(stderr,"Unsupported touchpad\n"); return 1;
    }
    struct input_absinfo absinfo[ABS_CNT]={0};
    struct sigaction sa={.sa_handler=stop_handler};
    sigemptyset(&sa.sa_mask); sigaction(SIGINT,&sa,NULL); sigaction(SIGTERM,&sa,NULL);
    f.out=clone_device(dev,absinfo);
    f.xres=absinfo[ABS_X].resolution; f.yres=absinfo[ABS_Y].resolution;
    if (f.xres<=0 || f.yres<=0) { fprintf(stderr,"Invalid coordinate resolution\n"); return 1; }
    f.slot=absinfo[ABS_MT_SLOT].value; f.slot_min=absinfo[ABS_MT_SLOT].minimum;
    int64_t slots=(int64_t)absinfo[ABS_MT_SLOT].maximum-f.slot_min+1;
    if (slots<=0 || slots>65536) { fprintf(stderr,"Invalid slot range\n"); return 1; }
    f.slot_count=(int)slots; f.active=calloc((size_t)slots,1);
    if (!f.active) die("calloc");
    f.x=absinfo[ABS_X].value; f.y=absinfo[ABS_Y].value;
    check_ioctl(dev,EVIOCGRAB,1);
    check_ioctl(dev,EVIOCGKEY(sizeof(active)),(unsigned long)active);
    /* Check slots as well: BTN_TOUCH is optional on some devices. */
    int32_t *tracking=calloc((size_t)slots+1,sizeof(*tracking));
    if (!tracking) die("calloc");
    tracking[0]=ABS_MT_TRACKING_ID;
    check_ioctl(dev,EVIOCGMTSLOTS(((size_t)slots+1)*sizeof(*tracking)),(unsigned long)tracking);
    int touching=HAS(active,BTN_TOUCH);
    for (int i=1;i<=slots;i++) if (tracking[i]>=0) touching=1;
    free(tracking);
    if (touching) { fprintf(stderr,"Lift fingers before starting\n"); return 1; }
    setvbuf(stdout,NULL,_IOLBF,0);
    if (!f.quiet) puts("Touchpad filter started");
    struct buffer frame={0};
    int status=0;
    while (!stopping) {
        struct input_event events[128];
        ssize_t bytes=read(dev,events,sizeof(events));
        if (bytes<0 && errno==EINTR) continue;
        if (bytes<=0 || bytes%(ssize_t)sizeof(events[0])) { if (bytes<0) perror("read input"); status=1; break; }
        for (size_t i=0;i<(size_t)bytes/sizeof(events[0]) && !stopping;i++) {
            struct input_event ev=events[i];
            if (ev.type==EV_SYN && ev.code==SYN_DROPPED) {
                fprintf(stderr,"SYN_DROPPED: stopping filter\n"); status=1; stopping=1; break;
            }
            if (ev.type==EV_SYN && ev.code==SYN_REPORT) {
                if (frame.n) append(&frame,(struct input_event){.type=EV_SYN,.code=SYN_REPORT});
                struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC,&ts)<0) die("clock_gettime");
                process_frame(&f,&frame,(double)ts.tv_sec+(double)ts.tv_nsec/1e9);
            } else if (ev.type!=EV_SYN && ev.type!=EV_MSC) {
                ev.time=(struct timeval){0}; append(&frame,ev);
            }
        }
    }
    ioctl(dev,EVIOCGRAB,0); ioctl(f.out,UI_DEV_DESTROY);
    close(f.out); close(dev); free(f.active); free(frame.v); free(f.pending.v);
    return status;
}
