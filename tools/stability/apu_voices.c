/* Exercise the real VP and resampler with generated PCM, without a game. */
#include <assert.h>
#include "../../../xboxrecomp_fork/src/apu/apu_vp.c"

static uint8_t ram[1u << 20];
MCPXAPUState *g_state;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
__declspec(thread) uint32_t g_esp;
ptrdiff_t g_xbox_mem_offset;

uint8_t *mcpx_apu_ram_address(uint64_t address, uint32_t bytes)
{
    assert(address + bytes <= sizeof ram);
    return ram + address;
}
uint8_t *mcpx_apu_ds_address(uint64_t address, uint32_t bytes)
{
    return mcpx_apu_ram_address(address, bytes);
}
void mcpx_apu_ram_copy(uint64_t address, void *destination, uint32_t bytes)
{
    memcpy(destination, mcpx_apu_ram_address(address, bytes), bytes);
}

static void setup(MCPXAPUState *d, uint16_t v, unsigned length, int looping)
{
    memset(ram, 0, sizeof ram);
    d->regs[NV_PAPU_VPVADDR] = 0x1000;
    d->regs[NV_PAPU_FENADDR] = 0x20000;
    d->regs[NV_PAPU_VPSGEADDR] = 0x30000;
    d->regs[NV_PAPU_TVL2D] = v;
    d->regs[NV_PAPU_TVL3D] = d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    ds_stl(0x30000, 0x40000);
    voice_set_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                   NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE, 0xFFFF);
    voice_set_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE, 1);
    voice_set_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE, 1);
    voice_set_mask(d, v, NV_PAVS_VOICE_CFG_FMT, NV_PAVS_VOICE_CFG_FMT_LOOP, looping);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 1);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_EACUR, 5);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_EFCUR, 5);
    voice_set_mask(d, v, NV_PAVS_VOICE_CFG_ENVA, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL, 255);
    voice_set_mask(d, v, NV_PAVS_VOICE_CFG_ENVF, NV_PAVS_VOICE_CFG_ENVA_EA_SUSTAINLEVEL, 255);
    voice_set_mask(d, v, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO, length - 1);
    ds_stl(0x1000 + v * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_VOLA, 0xFFFFFFFF);
    ds_stl(0x1000 + v * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFF);
    ds_stl(0x1000 + v * NV_PAVS_SIZE + NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFF);
    voice_set_mask(d, v, NV_PAVS_VOICE_TAR_VOLA, NV_PAVS_VOICE_TAR_VOLA_VOLUME0, 0);
    voice_set_mask(d, v, NV_PAVS_VOICE_CFG_VBIN, NV_PAVS_VOICE_CFG_VBIN_V0BIN, 0);
    for (unsigned i = 0; i < length; i++) ((int16_t *)(ram + 0x40000))[i] = 8192;
    voice_reset_filters(d, v);
}

typedef struct {
    MCPXAPUState *d;
    uint16_t v;
    HANDLE entered, done;
} LockWorker;

static DWORD WINAPI lock_worker(LPVOID parameter)
{
    LockWorker *worker = parameter;
    SetEvent(worker->entered);
    voice_lock(worker->d, worker->v, true);
    SetEvent(worker->done);
    return 0;
}

static void test_lock_handoff(MCPXAPUState *d, uint16_t v)
{
    LockWorker workers[2] = {{0}};
    HANDLE threads[2];
    assert(mcpx_apu_voice_idle_begin(d, v));
    for (unsigned i = 0; i < 2; i++) {
        workers[i].d = d; workers[i].v = v + (uint16_t)i;
        workers[i].entered = CreateEventA(NULL, TRUE, FALSE, NULL);
        workers[i].done = CreateEventA(NULL, TRUE, FALSE, NULL);
        threads[i] = CreateThread(NULL, 0, lock_worker, &workers[i], 0, NULL);
        assert(threads[i]);
        assert(WaitForSingleObject(workers[i].entered, 2000) == WAIT_OBJECT_0);
    }
    assert(WaitForSingleObject(workers[0].done, 20) == WAIT_TIMEOUT);
    assert(WaitForSingleObject(workers[1].done, 2000) == WAIT_OBJECT_0);
    mcpx_apu_voice_idle_end(d, v);
    for (unsigned i = 0; i < 2; i++) {
        assert(WaitForSingleObject(workers[i].done, 2000) == WAIT_OBJECT_0);
        assert(WaitForSingleObject(threads[i], 2000) == WAIT_OBJECT_0);
        CloseHandle(threads[i]); CloseHandle(workers[i].entered); CloseHandle(workers[i].done);
        voice_lock(d, workers[i].v, false);
    }
    puts("PASS: voice updates wait for their own slice; other voices remain independent");
}

#define OVERLAP_SLICES 160
static void render_overlap(MCPXAPUState *d, int mode, unsigned impact_at,
                           float output[OVERLAP_SLICES][NUM_SAMPLES_PER_FRAME])
{
    const uint16_t shot = 64, impact = 65;
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    setup(d, shot, 4096, 0);
    memcpy(ram + 0x1000 + impact * NV_PAVS_SIZE, ram + 0x1000 + shot * NV_PAVS_SIZE, NV_PAVS_SIZE);
    voice_set_mask(d, impact, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);
    voice_set_mask(d, impact, NV_PAVS_VOICE_CUR_PSL_START, NV_PAVS_VOICE_CUR_PSL_START_BA, 4096);
    voice_set_mask(d, impact, NV_PAVS_VOICE_PAR_NEXT, NV_PAVS_VOICE_PAR_NEXT_EBO, 31);
    ds_stl(0x30008, 0x50000);
    for (unsigned i = 0; i < 32; i++) ((int16_t *)(ram + 0x50000))[i] = -4096;
    if (mode == 1)
        voice_set_mask(d, shot, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE, 0);
    for (unsigned frame = 0; frame < OVERLAP_SLICES; frame++) {
        if (frame == impact_at && mode != 0) {
            /* Start the impact immediately after the shot in the same list,
             * routing both into the same mixbin. Use the actual PIO start. */
            fe_method(d, NV1BA0_PIO_SET_ANTECEDENT_VOICE, shot);
            fe_method(d, NV1BA0_PIO_VOICE_ON, impact | (5u << 28) | (5u << 24));
        }
        memset(bins, 0, sizeof bins);
        mcpx_apu_vp_frame(d, bins);
        memcpy(output[frame], bins[0], sizeof output[frame]);
    }
}

static void test_shot_impact_overlap(MCPXAPUState *d)
{
    static float shot[OVERLAP_SLICES][NUM_SAMPLES_PER_FRAME];
    static float impact[OVERLAP_SLICES][NUM_SAMPLES_PER_FRAME];
    static float together[OVERLAP_SLICES][NUM_SAMPLES_PER_FRAME];
    const unsigned delays[] = {0, 1, 4, 24};
    for (unsigned delay = 0; delay < sizeof delays / sizeof *delays; delay++) {
        float impact_peak = 0;
        render_overlap(d, 0, delays[delay], shot);
        render_overlap(d, 1, delays[delay], impact);
        render_overlap(d, 2, delays[delay], together);
        for (unsigned frame = 0; frame < OVERLAP_SLICES; frame++)
            for (unsigned sample = 0; sample < NUM_SAMPLES_PER_FRAME; sample++) {
                impact_peak = fmaxf(impact_peak, fabsf(impact[frame][sample]));
                assert(fabsf(together[frame][sample] - shot[frame][sample] - impact[frame][sample]) < 0.00001f);
            }
        assert(impact_peak > 0);
        assert(together[delays[delay] + 12][0] > 0.2f);
    }
    puts("PASS: immediate and delayed impacts mix with the continuing shot; neither replaces the other");
}

static void test_stale_idle(MCPXAPUState *d)
{
    const uint16_t v = 64;
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    for (unsigned restart = 0; restart < 32; restart++) {
        setup(d, v, 1024, 1);
        d->regs[NV_PAPU_FECTL] = 0;
        d->regs[NV_PAPU_FETFORCE1] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
        voice_off(d, v);
        fe_method(d, SE2FE_IDLE_VOICE, v);
        assert(d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FETRAPREASON_REQUESTED);
        /* The title removes/restarts the old voice before the ISR catches up. */
        d->regs[NV_PAPU_TVL2D] = 0xFFFF;
        fe_method(d, NV1BA0_PIO_SET_ANTECEDENT_VOICE, 1u << 16);
        fe_method(d, NV1BA0_PIO_VOICE_ON, v | (5u << 28) | (5u << 24));
        int idle = mcpx_apu_voice_idle_begin(d, v);
        /* Emulate the ISR's idle dispatch: a stale trap would unlink the
         * restarted voice but leave its hardware ACTIVE bit set. */
        if (idle) {
            d->regs[NV_PAPU_TVL2D] = 0xFFFF;
            mcpx_apu_voice_idle_end(d, v);
        }
        d->regs[NV_PAPU_FECTL] = 0;  /* the caller acknowledges the trap */
        memset(bins, 0, sizeof bins); mcpx_apu_vp_frame(d, bins);
        assert(d->regs[NV_PAPU_TVL2D] == v);
        assert(voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO) > 0);
        float peak = 0;
        for (unsigned i = 0; i < NUM_SAMPLES_PER_FRAME; i++) peak = fmaxf(peak, fabsf(bins[0][i]));
        assert(peak > 0);
    }
    /* A genuinely idle voice must still be reported and retired normally. */
    voice_off(d, v); d->regs[NV_PAPU_FECTL] = 0;
    fe_method(d, SE2FE_IDLE_VOICE, v);
    assert(mcpx_apu_voice_idle_begin(d, v));
    mcpx_apu_voice_idle_end(d, v);
    voice_lock(d, v, true);
    assert(!mcpx_apu_voice_idle_begin(d, v));
    voice_lock(d, v, false);
    d->regs[NV_PAPU_FECTL] = 0;
    assert(!mcpx_apu_voice_idle_begin(NULL, v));
    assert(!mcpx_apu_voice_idle_begin(d, MCPX_HW_MAX_VOICES));
    puts("PASS: 32 stop/restart races preserve the new sound; genuine idle reports are retained");
}

int main(void)
{
    const uint16_t v = 64;
    float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {{0}};
    MCPXAPUState *d = calloc(1, sizeof *d);
    assert(d);
    g_state = d; mcpx_apu_vp_init(d); mcpx_apu_vp_reset(d);
    test_lock_handoff(d, v);
    setup(d, v, 1024, 1);
    voice_lock(d, v, true);
    mcpx_apu_vp_frame(d, bins);
    if (voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO) != 0) {
        puts("FAIL: the mixer consumed samples while the game held the voice lock");
        return 1;
    }
    voice_lock(d, v, false);
    mcpx_apu_vp_frame(d, bins);
    assert(voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO) != 0);
    puts("PASS: locked voices wait; unlocked voices resume");

    setup(d, v, 1024, 1);
    for (unsigned n = 1; n <= 16; n++) {
        uint16_t other = v + (uint16_t)n;
        memcpy(ram + 0x1000 + other * NV_PAVS_SIZE, ram + 0x1000 + v * NV_PAVS_SIZE, NV_PAVS_SIZE);
        voice_set_mask(d, other - 1, NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE, other);
        voice_reset_filters(d, other);
    }
    voice_lock(d, v, true);
    mcpx_apu_vp_frame(d, bins);
    assert(!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
    for (unsigned n = 1; n <= 16; n++)
        assert(voice_get_mask(d, v + (uint16_t)n, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
    voice_lock(d, v, false);
    mcpx_apu_vp_frame(d, bins);
    assert(voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
    puts("PASS: a voice resumes correctly while sixteen other sounds play");

    const unsigned lengths[] = {1, 2, 32, 33, 95, 1024};
    const int pitches[] = {-4096, 0, 4096};
    for (unsigned length = 0; length < sizeof lengths / sizeof *lengths; length++)
        for (unsigned pitch = 0; pitch < sizeof pitches / sizeof *pitches; pitch++) {
            setup(d, v, lengths[length], 0);
            voice_set_mask(d, v, NV_PAVS_VOICE_TAR_PITCH_LINK,
                           NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH, (uint16_t)pitches[pitch]);
            float peak = 0;
            unsigned frames;
            for (frames = 0; frames < 256; frames++) {
                memset(bins, 0, sizeof bins);
                mcpx_apu_vp_frame(d, bins);
                for (unsigned i = 0; i < NUM_SAMPLES_PER_FRAME; i++) peak = fmaxf(peak, fabsf(bins[0][i]));
                if (!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE)) break;
            }
            unsigned expected_frames = (unsigned)(lengths[length] * pow(2.0, -pitches[pitch] / 4096.0));
            if ((expected_frames && peak == 0) || frames >= 256) {
                fprintf(stderr, "FAIL: %u source samples at pitch %d: peak %.6f, %u slices\n",
                        lengths[length], pitches[pitch], peak, frames);
                return 1;
            }
        }
    puts("PASS: short and long one-shots reach the mix and complete at half/normal/double pitch");

    setup(d, v, 33, 0);
    float samples[32][2];
    assert(voice_get_samples(d, v, samples, 32) == 32);
    assert(!d->vp.filters[v].resample_source_ended);
    assert(voice_get_samples(d, v, samples, 32) == 1);
    assert(d->vp.filters[v].resample_source_ended);
    puts("PASS: the inclusive final source sample is preserved");

    setup(d, v, 1024, 1);
    fe_method(d, NV1BA0_PIO_VOICE_PAUSE, v | NV1BA0_PIO_VOICE_PAUSE_ACTION);
    mcpx_apu_vp_frame(d, bins);
    assert(!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
    fe_method(d, NV1BA0_PIO_VOICE_PAUSE, v);
    mcpx_apu_vp_frame(d, bins);
    assert(voice_get_mask(d, v, NV_PAVS_VOICE_PAR_OFFSET, NV_PAVS_VOICE_PAR_OFFSET_CBO));
    fe_method(d, NV1BA0_PIO_VOICE_OFF, v);
    assert(!voice_get_mask(d, v, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE));
    puts("PASS: loops, pause/resume, explicit stop, and resampler restart remain functional");
    test_shot_impact_overlap(d);
    test_stale_idle(d);
    for (unsigned voice = 0; voice < MCPX_HW_MAX_VOICES; voice++)
        if (d->vp.filters[voice].resampler) src_delete(d->vp.filters[voice].resampler);
    mcpx_apu_vp_finalize(d); free(d);
    return 0;
}
