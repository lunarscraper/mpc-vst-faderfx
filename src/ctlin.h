/* ctlin.h - Fader FX's own MIDI input "Control In" (1.1.0): an ALSA sequencer client named after the plugin ("Fader FX",
 * a second instance "Fader FX 2") with one writable port "Control In". It subscribes itself to every hardware MIDI input
 * (USB controllers such as a Faderfox EC4, the DIN input), also to ones plugged in later (the system announce port), the
 * way Acid FX, Euclidier FX and DrumGen do; a MIDI track can also pick it as its output port, and CC Map's LINK A feeds
 * every port named "Control In".
 *
 * Read without blocking at the start of each block, on the audio thread; no thread of its own. libasound is dlopen()ed
 * on the device, so the build needs no ALSA headers and no -lasound; without it (offline tests) there is no port.
 * Events are read raw (snd_seq_event_t: type at byte 0, data at byte 16), as Percolator's midiin.h does with the
 * decoder and Oneiroi's alsa_midi_in.h without. */
#pragma once
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { void *seq; int port, me; } ci_port_t;

static struct {
    int ok;
    int (*open)(void **, const char *, int, int);
    int (*set_name)(void *, const char *);
    int (*port)(void *, const char *, unsigned, unsigned);
    int (*nonblock)(void *, int);
    int (*input)(void *, void **);
    int (*close)(void *);
    int (*client_id)(void *);
    int (*connect_from)(void *, int, int, int);
    int (*ci_malloc)(void **);
    void (*ci_free)(void *);
    void (*ci_set_client)(void *, int);
    int (*ci_get_client)(const void *);
    int (*next_client)(void *, void *);
    int (*pi_malloc)(void **);
    void (*pi_free)(void *);
    void (*pi_set_client)(void *, int);
    void (*pi_set_port)(void *, int);
    int (*pi_get_port)(const void *);
    unsigned (*pi_get_cap)(const void *);
    unsigned (*pi_get_type)(const void *);
    int (*next_port)(void *, void *);
    int (*any_port_info)(void *, int, int, void *);
} ci_A;

#define CI_SYM(f, n) (*(void **)(&ci_A.f) = dlsym(h, n))
static inline void ci_load(void) {
    if (ci_A.ok) return;
    void *h = dlopen("libasound.so.2", RTLD_NOW);
    if (!h) { ci_A.ok = -1; return; }
    CI_SYM(open, "snd_seq_open"); CI_SYM(set_name, "snd_seq_set_client_name");
    CI_SYM(port, "snd_seq_create_simple_port"); CI_SYM(nonblock, "snd_seq_nonblock");
    CI_SYM(input, "snd_seq_event_input"); CI_SYM(close, "snd_seq_close"); CI_SYM(client_id, "snd_seq_client_id");
    CI_SYM(connect_from, "snd_seq_connect_from");
    CI_SYM(ci_malloc, "snd_seq_client_info_malloc"); CI_SYM(ci_free, "snd_seq_client_info_free");
    CI_SYM(ci_set_client, "snd_seq_client_info_set_client"); CI_SYM(ci_get_client, "snd_seq_client_info_get_client");
    CI_SYM(next_client, "snd_seq_query_next_client");
    CI_SYM(pi_malloc, "snd_seq_port_info_malloc"); CI_SYM(pi_free, "snd_seq_port_info_free");
    CI_SYM(pi_set_client, "snd_seq_port_info_set_client"); CI_SYM(pi_set_port, "snd_seq_port_info_set_port");
    CI_SYM(pi_get_port, "snd_seq_port_info_get_port"); CI_SYM(pi_get_cap, "snd_seq_port_info_get_capability");
    CI_SYM(pi_get_type, "snd_seq_port_info_get_type"); CI_SYM(next_port, "snd_seq_query_next_port");
    CI_SYM(any_port_info, "snd_seq_get_any_port_info");
    void **f = (void **)&ci_A.open;
    ci_A.ok = 1;
    for (size_t i = 0; i < (sizeof ci_A - sizeof(int)) / sizeof(void *); i++) if (!f[i]) ci_A.ok = -1;
}

enum { CI_CAP_READ = 1u << 0, CI_CAP_WRITE = 1u << 1, CI_CAP_SUBS_READ = 1u << 5, CI_CAP_SUBS_WRITE = 1u << 6,
       CI_CAP_NO_EXPORT = 1u << 7, CI_TYPE_MIDI_GENERIC = 1u << 1, CI_TYPE_HARDWARE = 1u << 16,
       CI_TYPE_APPLICATION = 1u << 20, CI_EV_CONTROLLER = 10, CI_EV_PORT_START = 63 };

/* listen to one hardware MIDI input: readable, exported, HARDWARE type (so not other plugins' virtual ports) */
static inline void ci_subscribe(ci_port_t *p, int client, int port) {
    if (client == p->me || client == 0) return;
    void *pi = NULL;
    if (ci_A.pi_malloc(&pi) < 0 || !pi) return;
    if (ci_A.any_port_info(p->seq, client, port, pi) >= 0) {
        const unsigned cap = ci_A.pi_get_cap(pi), type = ci_A.pi_get_type(pi);
        if ((cap & (CI_CAP_READ | CI_CAP_SUBS_READ)) == (CI_CAP_READ | CI_CAP_SUBS_READ) && !(cap & CI_CAP_NO_EXPORT) &&
            (type & CI_TYPE_HARDWARE))
            ci_A.connect_from(p->seq, p->port, client, port);
    }
    ci_A.pi_free(pi);
}
static inline void ci_subscribe_all(ci_port_t *p) {
    void *ci = NULL, *pi = NULL;
    if (ci_A.ci_malloc(&ci) < 0 || ci_A.pi_malloc(&pi) < 0) { if (ci) ci_A.ci_free(ci); if (pi) ci_A.pi_free(pi); return; }
    ci_A.ci_set_client(ci, -1);
    while (ci_A.next_client(p->seq, ci) >= 0) {
        const int c = ci_A.ci_get_client(ci);
        ci_A.pi_set_client(pi, c);
        ci_A.pi_set_port(pi, -1);
        while (ci_A.next_port(p->seq, pi) >= 0) ci_subscribe(p, c, ci_A.pi_get_port(pi));
    }
    ci_A.ci_free(ci);
    ci_A.pi_free(pi);
}

/* On the message thread (create). No port (p->seq NULL) if ALSA isn't there. */
static int ci_instances;
static __attribute__((unused)) void ci_open(ci_port_t *p, const char *name) {
    memset(p, 0, sizeof *p);
    if (getenv("FADERFX_NO_ALSA")) return;
    ci_load();
    if (ci_A.ok != 1 || ci_A.open(&p->seq, "default", 2 /* SND_SEQ_OPEN_INPUT */, 0) < 0) { p->seq = NULL; return; }
    char n[64];
    const int k = __atomic_add_fetch(&ci_instances, 1, __ATOMIC_RELAXED);
    if (k > 1) snprintf(n, sizeof n, "%s %d", name, k);
    else snprintf(n, sizeof n, "%s", name);
    ci_A.set_name(p->seq, n);
    p->me = ci_A.client_id(p->seq);
    p->port = ci_A.port(p->seq, "Control In", CI_CAP_WRITE | CI_CAP_SUBS_WRITE, CI_TYPE_MIDI_GENERIC | CI_TYPE_APPLICATION);
    if (p->port < 0 || ci_A.nonblock(p->seq, 1) < 0) { ci_A.close(p->seq); p->seq = NULL; return; }
    ci_A.connect_from(p->seq, p->port, 0 /* SYSTEM */, 1 /* ANNOUNCE */);   /* hot-plug */
    ci_subscribe_all(p);
}

/* The next CC waiting at the port: 1 and channel / number / value, or 0 when none is waiting. A newly appeared
 * hardware port is subscribed on the way; other events are skipped. */
static __attribute__((unused)) int ci_read_cc(ci_port_t *p, int *ch, int *cc, int *val) {
    if (!p->seq) return 0;
    for (int tries = 0; tries < 64; tries++) {
        unsigned char *ev = NULL;
        const int r = ci_A.input(p->seq, (void **)&ev);
        if (r == -28 /* -ENOSPC: the queue overran and was emptied */) continue;
        if (r < 0 || !ev) return 0;   /* -EAGAIN: nothing waiting */
        if (ev[0] == CI_EV_CONTROLLER) {
            unsigned param; int value;
            memcpy(&param, ev + 20, 4);
            memcpy(&value, ev + 24, 4);
            *ch = ev[16] & 0x0F; *cc = (int)(param & 0x7F); *val = value < 0 ? 0 : value > 127 ? 127 : value;
            return 1;
        }
        if (ev[0] == CI_EV_PORT_START) ci_subscribe(p, ev[16], ev[17]);
    }
    return 0;
}

static __attribute__((unused)) void ci_close(ci_port_t *p) {
    if (p->seq) ci_A.close(p->seq);
    memset(p, 0, sizeof *p);
}
