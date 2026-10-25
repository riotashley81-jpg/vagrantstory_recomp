/*
 * Vagrant Story: engine-level in-between frames (render passes,
 * psxrecomp docs/RENDER_PASSES.md). Plan: docs/ENGINE_FRAME_RATE.md.
 *
 * The field / battle / in-engine cutscene loop (BATTLE.PRG vs_battle_exec,
 * rood-reverse 146C.c) runs one 30 fps tick per iteration: pad, OT clear,
 * logic, scene and HUD draw into the OT, then func_8007629C flips
 * vs_main_frameBuf, waits for VSync and draws the OT. So at the swap call the
 * previous iteration's frame is drawn and the next flip shows it (PENDING).
 *
 * Each iteration this plugin keeps the machine as it is when the scene draw
 * starts (registers, main RAM, scratchpad): the tick's logic is done and
 * nothing of the scene is in the OT yet. At the swap call it runs render
 * passes: inside the sandbox the kept state is put back, the camera is
 * placed part of the way from the previous tick, and the draw is replayed
 * with the game's own code up to the swap call (skipping the sound loop),
 * then its OT is drawn into the rect the next flip shows. The sandbox
 * restores the machine afterwards, so the game never sees any of it.
 *
 * With the pass worker (mod_plugins.h, a helper process on another core) the
 * replay runs there instead: at the swap the plugin opens the frame's
 * generation and hands the helper the kept state plus the blended camera and
 * bones; at the next scene draw start, once the display shows the frame, it
 * puts the helper's result on top of the kept state inside a late pass and
 * submits its OT. The main thread then only pays for the GPU submission.
 * Above 60 fps a frame needs several in-between images (three at 120): one
 * helper per image builds them at once (PSX_PASS_WORKERS=1..4, default 1 at
 * 60 fps and 3 otherwise). PSX_PASS_WORKER=0 keeps the replay in-process.
 */
#include "mod_plugins.h"
#include "gpu.h"
#include "cpu_state.h"
#include "psx_cycle_freeze.h"
#include "psx_memory.h"
#include "render_pass_motion.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FR_PACKAGE "vs.enhancement.framerate"
#define FR_FEATURE "framerate"

/* BATTLE.PRG (loads at 0x80068800), vs_battle_exec's loop body: the scene
 * draw after the tick's logic (jal func_8008EC48, the map), and the swap. */
#define FR_DRAW_START      0x8007A5F4u
#define FR_DRAW_START_INSN 0x0C023B12u
#define FR_QUEUE_CALL      0x8007A634u  /* jal vs_battle_processObjectDataQueue */
#define FR_QUEUE_DONE      0x8007A63Cu  /* after it (nop delay slot) */
#define FR_SOUND_LOOP      0x8007A670u  /* for (i < tickspeed / 2) sound/vibration */
#define FR_SOUND_DONE      0x8007A6D4u  /* jal _drawScreenTransition (ot) */
#define FR_SWAP_CALL       0x8007A794u  /* jal func_8007629C (ot + 2081) */
#define FR_SWAP_CALL_INSN  0x0C01D8A7u

/* Main executable (rood-reverse config/SLUS_010.40/symbol_addrs.txt). */
#define VS_PUT_DRAW_ENV    0x80028CB4u
#define VS_DRAW_OTAG       0x80028C44u
#define VS_DRAW_SYNC       0x80028650u
#define VS_OT_TABLE        0x80055C80u  /* u_long* [2], indexed by frameBuf */
#define VS_DRAW_ENV        0x8005E0D0u  /* DRAWENV[2], 0x5C each */
#define VS_DISP_ENV        0x8005E188u  /* DISPENV[2], 0x14 each */
#define VS_FRAME_BUF       0x8005E210u
#define VS_TICKSPEED       0x8005E24Cu
#define DRAWENV_SIZE 0x5Cu
#define DISPENV_SIZE 0x14u
#define OT_SUBMIT_INDEX 2081u
#define SPAD_SIZE 1024u
/* What the draw code reads for the camera (rood-reverse scratch.h and
 * 44F14.c func_800AECA0): vs_scratch.viewMatrix and vs_scratch.camera
 * {position, lookAt, angles}, and the camera rotation D_8005E218. */
#define VS_VIEW_MATRIX     0x1F800014u
#define VS_CAM_POSITION    0x1F800034u
#define VS_CAM_LOOKAT      0x1F800044u
#define VS_CAM_ANGLES      0x1F800054u
#define VS_CAM_ROTATION    0x8005E218u
/* Skeletal actors (rood-reverse 3A1A0.h D_800F4538_t, 44F14.c
 * func_800AEF94): nBones at +0, MATRIX bones[42] at +0x6C, computed for the
 * tick's pose and then drawn by func_800B28A8(actor, camera, attached). */
#define VS_ACTOR_BONES     0x800B3A68u  /* func_800B3A68, called by func_800B28A8 */
#define VS_DRAW_BONES      0x800F3C58u  /* D_800F3C58.bones[63]: the draw copy */
/* BATTLE.PRG gim_t* D_800EB9BC (rood-reverse 5BF94.c vs_battle_loadGim): a
 * full-screen GIM card (the opening credits) is loaded. The game uploads it
 * straight into the frame's buffer during the next tick's logic, after that
 * frame's OT was drawn, so a replay of the draw would show the scene the card
 * covers. No passes while one is up. */
#define VS_GIM_CARD        0x800EB9BCu
#define ACTOR_BONES        0x6Cu
#define ACTOR_MAX_BONES    42u
#define FR_MAX_VALUES      2048u
/* How long the scene draw start waits for the first helper's result. */
#define FR_WORKER_WAIT_MS  6u
#define FR_MAX_WORKERS     4u

extern uint8_t* memory_get_ram_ptr(void);
extern uint8_t* memory_get_scratchpad_ptr(void);

/* The machine as each recent scene draw start found it, by tick. The frame
 * the presenter shows next was drawn one or two iterations before the swap
 * that opens its in-between frames (traced: at the swap the display shows
 * the previous iteration's drawing, and the presenter notices that before
 * or after the swap call depending on the scene's timing), so passes replay
 * that iteration, not the newest one. */
#define FR_HIST 3u
typedef struct FrKept {
    int valid;
    uint32_t tick;
    uint32_t gpr[32], hi, lo, gte_data[32], gte_ctrl[32];
    uint8_t* ram;
    uint32_t ram_bytes;
    uint8_t spad[SPAD_SIZE];
} FrKept;

/* Blended values from tick-1 to tick, exported at the swap of `tick` (when
 * the motion set pairs exactly those two) at phases 1/4, 1/2, 3/4. */
#define FR_BLENDS 3u
typedef struct FrBlendSet {
    int valid;
    uint32_t tick;
    uint32_t n[FR_BLENDS];
    PSXMotionValue v[FR_BLENDS][FR_MAX_VALUES];
} FrBlendSet;

static FrKept s_kept[FR_HIST];
static FrBlendSet s_blend[2];

static struct {
    PSXMotionSet* motion;
    uint32_t tick;
    int in_pass;            /* our pass is replaying the loop body */
    /* The tick whose OT the game drew into each DRAWENV's buffer. */
    int buf_valid[2];
    uint32_t buf_tick[2];
    /* This frame's passes: replay kept[base] with the blends of base+1. */
    const FrKept* base;
    const FrBlendSet* blend;
    /* Pass worker. */
    int worker;             /* -1 unavailable, 0 not started, 1 running */
    uint32_t workers;       /* helper slots */
    uint32_t late;          /* jobs out for the frame on screen (slots 0..late-1) */
    int worker_fail_run;    /* consecutive failed jobs */
    uint64_t worker_failed, worker_done;
    PSXModRenderPass late_pass[FR_MAX_WORKERS];
    struct FrLate { uint32_t draw_env, slot, tick; int done; } late_job[FR_MAX_WORKERS];
    struct CPUState* cpu;   /* the emulation thread's, for the VBlank hook */
    const PSXMotionValue* values;   /* inside a pass: the placed values */
    uint32_t nvalues;
    uint32_t vblank_disp_x, vblank_disp_y;   /* display at the last VBlank */
} s_fr;

/* The helper's job: the kept state is loaded; replay it with these values. */
typedef struct FrJob {
    uint32_t alpha_q16;
    uint32_t nvalues;
    PSXMotionValue values[FR_MAX_VALUES];
} FrJob;
static FrJob s_job;

static const FrKept* fr_kept(uint32_t tick) {
    const FrKept* k = &s_kept[tick % FR_HIST];
    return k->valid && k->tick == tick && k->ram ? k : NULL;
}

static void fr_restore_kept(struct CPUState* cpu, const FrKept* k) {
    memcpy(memory_get_ram_ptr(), k->ram, k->ram_bytes);
    memcpy(memory_get_scratchpad_ptr(), k->spad, SPAD_SIZE);
    memcpy(cpu->gpr, k->gpr, sizeof k->gpr);
    cpu->hi = k->hi;
    cpu->lo = k->lo;
    memcpy(cpu->gte_data, k->gte_data, sizeof k->gte_data);
    memcpy(cpu->gte_ctrl, k->gte_ctrl, sizeof k->gte_ctrl);
}

static void fr_take_draw_start(struct CPUState* cpu) {
    ++s_fr.tick;
    if (s_fr.motion) {
        /* This tick's camera as the draw finds it, which is what the replay
         * overwrites at the same point. The map is drawn before
         * func_800AEF94 rebuilds the view matrix, so at the swap the matrix
         * is one the map never used. */
        psx_motion_begin(s_fr.motion, s_fr.tick);
        psx_motion_track(s_fr.motion, PSX_MOTION_MATRIX, VS_VIEW_MATRIX, 1);
        psx_motion_track(s_fr.motion, PSX_MOTION_MATRIX, VS_CAM_ROTATION, 2);
        psx_motion_track(s_fr.motion, PSX_MOTION_VECTOR, VS_CAM_POSITION, 3);
        psx_motion_track(s_fr.motion, PSX_MOTION_VECTOR, VS_CAM_LOOKAT, 4);
        psx_motion_track(s_fr.motion, PSX_MOTION_VECTOR, VS_CAM_ANGLES, 5);
    }
    FrKept* k = &s_kept[s_fr.tick % FR_HIST];
    const uint32_t bytes = psx_ram_live_bytes();
    k->valid = 0;
    if (bytes != k->ram_bytes) {
        free(k->ram);
        k->ram = (uint8_t*)malloc(bytes);
        k->ram_bytes = k->ram ? bytes : 0;
    }
    if (!k->ram) return;
    memcpy(k->ram, memory_get_ram_ptr(), bytes);
    memcpy(k->spad, memory_get_scratchpad_ptr(), SPAD_SIZE);
    memcpy(k->gpr, cpu->gpr, sizeof k->gpr);
    k->hi = cpu->hi;
    k->lo = cpu->lo;
    memcpy(k->gte_data, cpu->gte_data, sizeof k->gte_data);
    memcpy(k->gte_ctrl, cpu->gte_ctrl, sizeof k->gte_ctrl);
    k->tick = s_fr.tick;
    k->valid = 1;
}

/* The blended values for frame phase p (Q16): the nearest exported quarter. */
static uint32_t fr_blend_index(uint32_t alpha_q16) {
    int i = (int)((alpha_q16 * 4u + 32768u) / 65536u) - 1;
    return i < 0 ? 0u : i >= (int)FR_BLENDS ? FR_BLENDS - 1u : (uint32_t)i;
}

/* Inside the sandbox, on the kept state: replay the loop body from the
 * scene draw start with the placed values. The kept state is that
 * iteration's own (frameBuf, OT and all), so its OT goes to the buffer it
 * was built for, with everything the tick's logic had queued (the battle
 * sphere, menus, the HUD's absolute packets). */
static int fr_replay(struct CPUState* cpu) {
    s_fr.in_pass = 1;
    /* The model/sequence load queue (CD reads, parsing), the sound sequencer
     * and pad vibration draw nothing; skip them (their device stores would be
     * dropped anyway, and the pass worker's helper has no CD session). */
    int ran = psx_mod_run_guest_span(cpu, FR_DRAW_START, FR_QUEUE_CALL);
    if (ran) ran = psx_mod_run_guest_span(cpu, FR_QUEUE_DONE, FR_SOUND_LOOP);
    if (ran) ran = psx_mod_run_guest_span(cpu, FR_SOUND_DONE, FR_SWAP_CALL);
    s_fr.in_pass = 0;
    return ran;
}

/* Draw the replayed OT into the DRAWENV whose clip is the shown rect. */
static void fr_submit(struct CPUState* cpu, uint32_t draw_env) {
    const uint32_t fb = psx_mod_read_word(VS_FRAME_BUF) & 1u;
    const uint32_t ot = psx_mod_read_word(VS_OT_TABLE + 4u * fb) + 4u * OT_SUBMIT_INDEX;
    psx_mod_call_guest(cpu, VS_DRAW_SYNC, FR_SWAP_CALL, 0, 0, 0, 0);
    psx_mod_call_guest(cpu, VS_PUT_DRAW_ENV, FR_SWAP_CALL, draw_env, 0, 0, 0);
    psx_mod_call_guest(cpu, VS_DRAW_OTAG, FR_SWAP_CALL, ot, 0, 0, 0);
    psx_mod_call_guest(cpu, VS_DRAW_SYNC, FR_SWAP_CALL, 0, 0, 0, 0);
}

static int fr_pass(struct CPUState* cpu, void* user, uint32_t alpha_q16) {
    const uint32_t b = fr_blend_index(alpha_q16);
    fr_restore_kept(cpu, s_fr.base);
    s_fr.values = s_fr.blend->v[b];
    s_fr.nvalues = s_fr.blend->n[b];
    const int ran = fr_replay(cpu);
    s_fr.values = NULL;
    s_fr.nvalues = 0;
    if (!ran) return 0;
    fr_submit(cpu, *(const uint32_t*)user);
    return 1;
}

/* Helper process: run the replay on the state the main process sent. */
static int fr_helper_job(struct CPUState* cpu, const void* user, uint32_t len) {
    const FrJob* job = (const FrJob*)user;
    if (len < offsetof(FrJob, values) || job->nvalues > FR_MAX_VALUES ||
        len < offsetof(FrJob, values) + job->nvalues * sizeof(PSXMotionValue))
        return 0;
    s_fr.values = job->values;
    s_fr.nvalues = job->nvalues;
    const int ran = fr_replay(cpu);
    s_fr.values = NULL;
    s_fr.nvalues = 0;
    return ran;
}

static const PSXMotionValue* fr_find_value(uint32_t kind, uint32_t addr) {
    uint32_t lo = 0, hi = s_fr.nvalues;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const PSXMotionValue* v = &s_fr.values[mid];
        if (v->addr < addr || (v->addr == addr && v->kind < kind)) lo = mid + 1;
        else hi = mid;
    }
    if (lo < s_fr.nvalues && s_fr.values[lo].addr == addr && s_fr.values[lo].kind == kind)
        return &s_fr.values[lo];
    return NULL;
}

/* Main process, inside a late pass: the kept state, the helper's result on
 * top, then its OT to the GPU. */
static int fr_late_pass(struct CPUState* cpu, void* user, uint32_t alpha_q16) {
    const struct FrLate* job = (const struct FrLate*)user;
    const FrKept* k = fr_kept(job->tick);
    (void)alpha_q16;
    if (!k) return 0;
    fr_restore_kept(cpu, k);
    if (!psx_mod_pass_worker_apply(job->slot)) return 0;
    fr_submit(cpu, job->draw_env);
    return 1;
}

/* Add the helpers' images to the frame they were for, earliest phase first
 * (its deadline comes first). A late pass is refused until the display has
 * flipped to that frame; before the final call a refused or unfinished job
 * stays out for the next try. */
static void fr_collect_late(struct CPUState* cpu, uint32_t wait_ms, int final) {
    const uint32_t n = s_fr.late;
    if (!n) return;
    for (uint32_t i = 0; i < n; i++) {
        struct FrLate* job = &s_fr.late_job[i];
        if (job->done) continue;
        /* The helpers run at once: after the first, the rest are about done. */
        if (!psx_mod_pass_worker_result(job->slot, i == 0 ? wait_ms : (wait_ms ? 2u : 0u))) {
            if (final) job->done = 1;
            continue;
        }
        if (!fr_kept(job->tick)) { job->done = 1; continue; }   /* its state is gone */
        if (psx_mod_render_pass_late(cpu, &s_fr.late_pass[i], fr_late_pass, job) || final)
            job->done = 1;
    }
    if (final) s_fr.late = 0;
}

/* Guest VBlank, just before its presents: the image due in this VBlank is
 * added now, not at the next scene draw start (after the next tick's logic,
 * which comes too late for the middle of the frame). The wait spends time
 * the presenter would otherwise sleep until its deadline. Traced at 60 fps
 * in the slot 1 cutscene: collected at the scene draw start, two of three
 * in-between images missed their present. */
static void fr_vblank(void) {
    {
        GpuDisplayInfo di;
        gpu_get_display_info(&di);
        s_fr.vblank_disp_x = di.display_x;
        s_fr.vblank_disp_y = di.display_y;
    }
    if (!s_fr.late || !s_fr.cpu || g_psx_render_pass_active) return;
    /* Up to 1 ms before the next present: a job that is not done by then
     * stays out for the next present, VBlank or the scene draw start. Also
     * the presenter's hook before each present (psx_mod_set_render_pass_
     * present_hook): above 60 fps one VBlank has more than one present. */
    const double slack = psx_mod_render_pass_present_slack_ms() - 1.0;
    fr_collect_late(s_fr.cpu, slack > 0.0 ? (uint32_t)slack : 0u, 0);
}

/* Scene draw start. In the real frame: keep the machine as the draw finds
 * it. In our replay: place what the draw code reads part of the way to the
 * next tick. */
static void fr_draw_start(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (!g_psx_render_pass_active) {
        s_fr.cpu = cpu;
        fr_collect_late(cpu, FR_WORKER_WAIT_MS, 1);
        fr_take_draw_start(cpu);
        return;
    }
    if (!s_fr.in_pass || !s_fr.values) return;
    for (uint32_t i = 0; i < s_fr.nvalues; i++) psx_motion_write_value(&s_fr.values[i]);
    /* The GTE already holds the view matrix here (traced: rotation and
     * translation equal vs_scratch.viewMatrix at every draw start), and the
     * map is drawn with it before anything reloads it: load the placed one. */
    for (uint32_t i = 0; i < 4; i++) cpu->gte_ctrl[i] = psx_mod_read_word(VS_VIEW_MATRIX + 4u * i);
    cpu->gte_ctrl[4] = (uint32_t)(int32_t)(int16_t)psx_mod_read_half(VS_VIEW_MATRIX + 16u);
    for (uint32_t i = 0; i < 3; i++) cpu->gte_ctrl[5 + i] = psx_mod_read_word(VS_VIEW_MATRIX + 20u + 4u * i);
}

/* func_800B3A68(actor, &actor->bones[0], clip, model) entry, inside
 * func_800B28A8: the actor's bones for this tick are computed (into
 * actor->bones and the draw copy D_800F3C58.bones) and not yet drawn. In the
 * real frame: record them. In our replay: place them part of the way to the
 * next tick. (At func_800B28A8's entry the bones still hold the last draw's
 * values, and it recomputes them.) The same check runs for objects and
 * weapons; only an actor passes its own bones. */
static void fr_actor_bones(struct CPUState* cpu, uint32_t address) {
    (void)address;
    const uint32_t actor = cpu->gpr[4];
    if ((actor & 0x1FFFFFFFu) >= 0x00200000u || (actor & 3u)) return;
    if (cpu->gpr[5] != actor + ACTOR_BONES) return;
    uint32_t n = psx_mod_read_byte(actor);
    if (n > ACTOR_MAX_BONES) n = ACTOR_MAX_BONES;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t bone = actor + ACTOR_BONES + 32u * i;
        if (s_fr.values) {
            if (s_fr.in_pass) {
                const PSXMotionValue* v = fr_find_value(PSX_MOTION_MATRIX, bone);
                if (v) {
                    PSXMotionValue d = *v;
                    psx_motion_write_value(v);
                    d.addr = VS_DRAW_BONES + 32u * i;
                    psx_motion_write_value(&d);
                }
            }
        } else if (!g_psx_render_pass_active && s_fr.motion) {
            psx_motion_track(s_fr.motion, PSX_MOTION_MATRIX, bone,
                             psx_motion_identity(actor, i, n));
        }
    }
}

/* Helpers for this session: PSX_PASS_WORKERS, else one per in-between image
 * the chosen rate needs (one at 60 fps, three at 120 and above). */
static uint32_t fr_worker_count(void) {
    const char* e = getenv("PSX_PASS_WORKERS");
    char value[16];
    if (e && e[0]) {
        const int v = atoi(e);
        return v < 1 ? 1u : v > (int)FR_MAX_WORKERS ? FR_MAX_WORKERS : (uint32_t)v;
    }
    if (psx_mod_option_value(FR_PACKAGE, FR_FEATURE, "rate", value, sizeof value) &&
        strcmp(value, "60") != 0)
        return 3u;
    return 1u;
}

/* Hand the replay of this frame to the helpers (fr_swap_call). 1 when the
 * worker is in use, so no in-process pass runs. */
static int fr_worker_frame(const PSXModRenderPassFrame* frame, uint32_t draw_env) {
    uint32_t phases[8];
    if (s_fr.worker == 0) {
        const char* e = getenv("PSX_PASS_WORKER");
#ifdef PSX_DISC_CODE
        /* The disc-code build's helper has no compiled overlays of its own
         * yet (its jobs run interpreted and fail): opt-in only. */
        const int want = e && e[0] == '1';
#else
        const int want = !(e && e[0] == '0');
#endif
        s_fr.workers = want ? psx_mod_pass_worker_start(fr_worker_count()) : 0;
        s_fr.worker = s_fr.workers ? 1 : -1;
        fprintf(stdout, "vagrantstory: render pass workers: %u\n", s_fr.workers);
        fflush(stdout);
    }
    if (s_fr.worker < 0) return 0;
    {
        PSXModPassWorkerStats st;
        psx_mod_pass_worker_get_stats(&st);
        if (st.failed_session) { s_fr.worker = -1; return 0; }
        /* Jobs that keep failing: replay in-process for the session. */
        if (st.failed > s_fr.worker_failed) {
            if (++s_fr.worker_fail_run >= 8) { s_fr.worker = -1; return 0; }
        } else if (st.done > s_fr.worker_done) {
            s_fr.worker_fail_run = 0;
        }
        s_fr.worker_failed = st.failed;
        s_fr.worker_done = st.done;
    }
    /* Free slots: a job still out (no scene draw start since) is for a frame
     * that is gone; one that is still running keeps its slot. */
    s_fr.late = 0;
    uint32_t slots[FR_MAX_WORKERS], nfree = 0, nready = 0;
    for (uint32_t i = 0; i < s_fr.workers; i++) {
        if (!psx_mod_pass_worker_ready(i)) continue;
        nready++;
        if (psx_mod_pass_worker_pending(i)) (void)psx_mod_pass_worker_result(i, 0);
        if (!psx_mod_pass_worker_pending(i)) slots[nfree++] = i;
    }
    if (!nready) return 0;   /* helpers starting: in-process */
    if (!nfree) return 1;    /* helpers busy: no passes */
    const uint32_t n = psx_mod_render_pass_plan(frame->period_vblanks,
                                                frame->shown_after_vblanks, phases, 8);
    if (n == 0) return 1;
    /* The phases the free helpers take: all of them, or an even spread. */
    const uint32_t take = n < nfree ? n : nfree;
    PSXModRenderPass rect;
    memset(&rect, 0, sizeof rect);
    rect.struct_size = sizeof rect;
    rect.x = frame->x; rect.y = frame->y; rect.w = frame->w; rect.h = frame->h;
    rect.alpha_q16 = phases[n / 2];
    if (!psx_mod_render_pass_open(&rect)) return 1;
    const FrKept* k = s_fr.base;
    PSXModPassWorkerState st;
    st.ram = k->ram;
    st.scratchpad = k->spad;
    st.gpr = k->gpr;
    st.hi = k->hi;
    st.lo = k->lo;
    st.gte_data = k->gte_data;
    st.gte_ctrl = k->gte_ctrl;
    for (uint32_t i = 0; i < take; i++) {
        const uint32_t alpha = take == n ? phases[i]
                                         : phases[(2u * i + 1u) * n / (2u * take)];
        const uint32_t b = fr_blend_index(alpha);
        s_job.alpha_q16 = alpha;
        s_job.nvalues = s_fr.blend->n[b];
        memcpy(s_job.values, s_fr.blend->v[b], s_job.nvalues * sizeof(PSXMotionValue));
        if (!psx_mod_pass_worker_submit(slots[i], &st, &s_job,
                                        (uint32_t)(offsetof(FrJob, values) +
                                                   s_job.nvalues * sizeof(PSXMotionValue))))
            continue;
        PSXModRenderPass* lp = &s_fr.late_pass[s_fr.late];
        *lp = rect;
        lp->alpha_q16 = alpha;
        s_fr.late_job[s_fr.late].draw_env = draw_env;
        s_fr.late_job[s_fr.late].slot = slots[i];
        s_fr.late_job[s_fr.late].tick = k->tick;
        s_fr.late_job[s_fr.late].done = 0;
        s_fr.late++;
    }
    return 1;
}

/* vs_scratch.camera.angles are 12-bit angles in s32s (4096 per turn) that
 * wrap: turning the camera past 0 goes 0 -> 4088. Tracked as plain
 * vectors, that blended the long way round, half a turn off (an in-between
 * frame facing away: the screen mostly black, the minimap turned; reported
 * at a wall in the field). Blend them along the shorter arc instead, from
 * the two ticks' draw-start values. */
static void fr_fix_angle_blends(FrBlendSet* bs) {
    const FrKept* a = fr_kept(bs->tick - 1u);
    const FrKept* c = fr_kept(bs->tick);
    const uint32_t off = VS_CAM_ANGLES - 0x1F800000u;
    if (!a || !c) return;
    for (uint32_t b = 0; b < FR_BLENDS; b++) {
        const double t = (b + 1) / 4.0;
        for (uint32_t i = 0; i < bs->n[b]; i++) {
            PSXMotionValue* v = &bs->v[b][i];
            if (v->addr != VS_CAM_ANGLES || v->kind != PSX_MOTION_VECTOR) continue;
            for (uint32_t k = 0; k < 3; k++) {
                int32_t p, q;
                memcpy(&p, a->spad + off + 4u * k, 4);
                memcpy(&q, c->spad + off + 4u * k, 4);
                int32_t d = q - p;
                if (d > 2048) d -= 4096;
                else if (d < -2048) d += 4096;
                int32_t r = p + (int32_t)(t * d + (d >= 0 ? 0.5 : -0.5));
                if (p >= 0 && p < 4096 && q >= 0 && q < 4096) r &= 4095;
                v->v[k] = r;
            }
        }
    }
}

static void fr_swap_call(struct CPUState* cpu, uint32_t address) {
    (void)address;
    if (g_psx_render_pass_active || !fr_kept(s_fr.tick)) return;
    const uint32_t fb = psx_mod_read_word(VS_FRAME_BUF) & 1u;
    if (s_fr.motion) {
        /* Pair this tick's values (camera at the draw start, bones at each
         * actor draw) with the previous tick's, and keep the blends between
         * them for the frame of the previous tick, whose in-between frames
         * may be opened now or at the next swap. */
        PSXMotionLimits limits = { 4096.0, 1.0 };
        PSXMotionStats stats;
        FrBlendSet* bs = &s_blend[s_fr.tick & 1u];
        (void)psx_motion_prepare(s_fr.motion, &limits, &stats);
        bs->valid = 0;
        for (uint32_t b = 0; b < FR_BLENDS; b++)
            bs->n[b] = psx_motion_export(s_fr.motion, (b + 1) / 4.0, bs->v[b], FR_MAX_VALUES);
        bs->tick = s_fr.tick;
        fr_fix_angle_blends(bs);
        bs->valid = 1;
    }
    /* The buffer the presenter will see as new next: the display's, when
     * the game already pointed it there since the last VBlank (the field),
     * else the other one, which the swap flips to (event scenes). Its
     * picture is the OT the game drew there at an earlier swap: replay that
     * tick, natively (its own frameBuf and OT), towards the next one. */
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    uint32_t shown = fb ^ 1u;
    int shown_event = 0;
    {
        const uint32_t d0 = VS_DRAW_ENV, d1 = VS_DRAW_ENV + DRAWENV_SIZE;
        const int on0 = psx_mod_read_half(d0) == di.display_x && psx_mod_read_half(d0 + 2) == di.display_y;
        const int on1 = psx_mod_read_half(d1) == di.display_x && psx_mod_read_half(d1 + 2) == di.display_y;
        if (on0 != on1) {
            const uint32_t on = on0 ? 0u : 1u;
            const int moved = di.display_x != s_fr.vblank_disp_x || di.display_y != s_fr.vblank_disp_y;
            shown = moved ? on : on ^ 1u;
            /* Event scenes (the swap flips to the frame, e.g. the 'Have you found Sydney?' scene):
             * their in-between frames still lose the widescreen sides and
             * the characters' rim light even when replayed from the frame's
             * own tick; hold their frames until that is understood. */
            if (!moved) shown_event = 1;
        }
    }
    const int have_base = s_fr.buf_valid[shown];
    const uint32_t base_tick = s_fr.buf_tick[shown];
    /* This swap draws this tick's OT into DRAWENV[frameBuf ^ 1] (traced: the
     * drawing area after every swap). */
    s_fr.buf_valid[fb ^ 1u] = 1;
    s_fr.buf_tick[fb ^ 1u] = s_fr.tick;
    if (shown_event || !have_base || base_tick >= s_fr.tick) return;
    s_fr.base = fr_kept(base_tick);
    s_fr.blend = &s_blend[(base_tick + 1u) & 1u];
    if (!s_fr.base || !s_fr.blend->valid || s_fr.blend->tick != base_tick + 1u) return;
    const uint32_t draw_env = VS_DRAW_ENV + DRAWENV_SIZE * shown;
    const uint16_t x = psx_mod_read_half(draw_env + 0), y = psx_mod_read_half(draw_env + 2);
    uint16_t w = 0, h = 0;
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t d = VS_DISP_ENV + DISPENV_SIZE * i;
        if (psx_mod_read_half(d + 0) == x && psx_mod_read_half(d + 2) == y) {
            w = psx_mod_read_half(d + 4);
            h = psx_mod_read_half(d + 6);
        }
    }
    if (w == 0 || h == 0) return;
    if (psx_mod_read_word(VS_GIM_CARD) != 0) return;
    PSXModRenderPassFrame frame;
    memset(&frame, 0, sizeof frame);
    frame.struct_size = sizeof frame;
    frame.period_vblanks = (psx_mod_read_word(VS_TICKSPEED) >= 4) ? 4u : 2u;
    frame.shown_after_vblanks = 1;
    frame.x = x; frame.y = y; frame.w = w; frame.h = h;
    if (fr_worker_frame(&frame, draw_env)) return;
    (void)psx_mod_render_pass_frame(cpu, &frame, fr_pass, (void*)&draw_env);
}

static void fr_activate(void) {
    if (!s_fr.motion) s_fr.motion = psx_motion_set_create(2048);
    if (s_fr.motion) psx_motion_invalidate(s_fr.motion);
    for (uint32_t i = 0; i < FR_HIST; i++) s_kept[i].valid = 0;
    s_blend[0].valid = s_blend[1].valid = 0;
    s_fr.buf_valid[0] = s_fr.buf_valid[1] = 0;
    const int ok = psx_mod_activate_render_pass_rate(FR_PACKAGE, FR_FEATURE, "rate",
                                                     PSX_MOD_RENDER_PASS_FLIP_PENDING);
    psx_mod_set_render_pass_present_hook(fr_vblank);
    fprintf(stdout, "vagrantstory: engine frame rate %s\n", ok ? "on" : "unavailable");
}

PSX_MOD_CONSTRUCTOR(vs_register_frame_rate_plugin) {
    const char* id = "vs.framerate";
    (void)psx_mod_register_activation_plugin(id, fr_activate);
    (void)psx_mod_register_vblank_plugin(id, fr_vblank);
    (void)psx_mod_pass_worker_set_job(fr_helper_job);
    (void)psx_mod_register_instruction_plugin(id, FR_SWAP_CALL, FR_SWAP_CALL_INSN,
                                              fr_swap_call);
    (void)psx_mod_register_instruction_plugin(id, FR_DRAW_START, FR_DRAW_START_INSN,
                                              fr_draw_start);
    (void)psx_mod_register_function_entry_plugin(id, VS_ACTOR_BONES, fr_actor_bones);
}
