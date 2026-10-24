// Stadium game registration for the Switch runner.
//
// The body of psrnx_register_stadium() is src/main/main.cpp lines 2158-2282 (GameEntry, overlay/RSP hook
// registration, audio use-after-free voice layouts) copied verbatim, so the Switch build registers exactly what the
// PC runner does. When upstream main.cpp changes, diff this block against it.
#include <cstdint>
#include <cstdio>
#include <string>

#include "librecomp/audio_uaf_protect.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "recomp.h"
#include "ultramodern/ultramodern.hpp"

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();
namespace pokestadium { void register_overlays(); }
namespace pokestadium::rsp { void register_pre_task_hooks(); }

// Returns the game id for recomp::select_rom / recomp::start_game.
std::u8string psrnx_register_stadium() {
    recomp::GameEntry game{};
    game.rom_hash               = 0x6E46EACF8F27011DULL;  // XXH3_64bits of baserom.z64 (US v1.0)
                                                         // computed by tools/compute_rom_hash.exe
    game.internal_name          = "POKEMON STADIUM";
    game.game_id                = u8"pokestadium.us.1.0";
    game.mod_game_id            = "pokestadium";
    game.save_type              = recomp::SaveType::Flashram;  // Pokemon Stadium uses 1Mbit FlashRAM
    game.is_enabled             = true;
    game.has_compressed_code    = false;
    game.entrypoint_address     = get_entrypoint_address();
    game.entrypoint             = recomp_entrypoint;

    // Activate Stadium's dead-code expansion-pak path before Util_InitMainPools
    // runs. Util_InitMainPools reads gExpansionRAMStart (vaddr 0x80068B90)
    // to choose POOL_END_4MB vs POOL_END_6MB. The 6MB path is only taken
    // when gExpansionRAMStart > 0 AND osMemSize > 0x600000. Our runtime
    // reports osMemSize=8MB correctly, but gExpansionRAMStart lives in
    // BSS and has NO writer anywhere in the recompiled binary â€” on real
    // hardware some hardware-detect path (RDP probe / silicon side-effect)
    // sets it; in our HLE harness it stays zeroed and the 6MB code path is
    // unreachable. Stadium's working set marginally exceeds the 3MB usable
    // cap of the 4MB path, so the title-screen pokemon-models loader hits
    // an alloc failure without this. Forcing the value at on_init_callback
    // is the proper layer for this kind of "missing-from-HLE hardware-detect
    // side-effect" â€” it fires once after rdram is wired up but before the
    // game thread reads the global. Previously implemented as a hook on
    // Util_InitMainPools entry; that worked but required a recompile-time
    // hook for a one-time runtime-init fact. See TEMP_PATCHES.md
    // 'force-expansion-ram' entry (retired by this change).
    game.on_init_callback = [](uint8_t* rdram, recomp_context* /*ctx*/) {
        // gExpansionRAMStart at kseg0 0x80068B90 -> physical 0x00068B90.
        // Write u32 = 1 with XOR-3 byte order to match recompiled MEM_W.
        constexpr uint32_t paddr = 0x00068B90u;
        rdram[(paddr + 0) ^ 3] = 0;
        rdram[(paddr + 1) ^ 3] = 0;
        rdram[(paddr + 2) ^ 3] = 0;
        rdram[(paddr + 3) ^ 3] = 1;
        std::fprintf(stderr,
            "[PSR] on_init: forced gExpansionRAMStart=1 "
            "(POOL_END_6MB path active)\n");
        std::fflush(stderr);
    };

    recomp::register_game(game);
    std::fprintf(stderr, "[PSR] game registered\n"); std::fflush(stderr);

    // Feed the generated section/overlay table into librecomp so its
    // func_map knows about every section beyond patches. Without this,
    // any LOOKUP_FUNC at runtime fails with "Failed to find function
    // at 0x...". Defined in src/main/register_overlays.cpp.
    pokestadium::register_overlays();
    std::fprintf(stderr, "[PSR] overlays registered\n"); std::fflush(stderr);

    // Register RSP pre-task hooks. Stadium's aspMain (the standard
    // libultra audio ucode, stripped variant at ROM 0x68020) skips
    // the command-load setup that Zelda's variant does inline; we
    // replicate rspboot's residue (DMEM[0x2B0] = first chunk, $29 =
    // 0x2B0, $30 = chunk_size, dma regs harmless) so the dispatch
    // loop reads real commands instead of dispatch-table residue.
    // Defined in src/main/rsp_aspmain_hook.cpp.
    pokestadium::rsp::register_pre_task_hooks();
    std::fprintf(stderr, "[PSR] rsp pre-task hooks registered\n"); std::fflush(stderr);

    // Register Stadium's libnaudio voice-list layout so the generic
    // audio-UAF protector in librecomp can silence voices whose backing
    // wavetable memory is about to be freed by main_pool_pop_state.
    //
    // Walk strategy: pAllocList + pLameList in N_ALSynth catch every
    // voice the synth might still pull from on the next audio frame
    // regardless of bus routing â€” a previous auxBus->sources walk
    // missed voices that were allocated but routed elsewhere.
    //
    // Offsets verified against disasm/src/libnaudio/n_synthInternals.h
    // (N_PVoice: dc_table=0x20, em_motion=0x84) and recompiled
    // n_alSynNew at 0x800491C0 (N_ALSynth: pAllocList=0x0C,
    // pLameList=0x14). n_syn lives at vaddr 0x80078584. The offsets
    // are libnaudio standards; future libnaudio games can reuse this
    // registration shape and only swap n_syn_var_vaddr.
    {
        librecomp::audio_uaf::VoiceLayout layout{};
        layout.n_syn_var_vaddr                = 0x80078584u;
        layout.alloc_list_offset              = 0x0Cu;  // N_ALSynth.pAllocList
        layout.lame_list_offset               = 0x14u;  // N_ALSynth.pLameList
        layout.voice_em_motion_offset         = 0x84u;  // N_PVoice.em_motion
        layout.voice_dc_table_offset          = 0x20u;  // N_PVoice.dc_table
        layout.voice_em_motion_stopped_value  = 0u;     // AL_STOPPED
        layout.max_voice_count                = 64u;
        // Diagnostic-only offsets for the always-on voice-event ring.
        // Standard libnaudio N_PVoice ABI (n_synthInternals.h): the
        // ADPCM load-filter fields. Used to capture predictor carry /
        // first-decode flag / sample position at each key-on so the
        // music-rate click can be correlated against voice events.
        layout.voice_dc_state_offset          = 0x0Cu;  // N_PVoice.dc_state (ADPCM_STATE*)
        layout.voice_dc_sample_offset         = 0x30u;  // N_PVoice.dc_sample
        layout.voice_dc_lastsam_offset        = 0x34u;  // N_PVoice.dc_lastsam
        layout.voice_dc_first_offset          = 0x38u;  // N_PVoice.dc_first
        librecomp::audio_uaf::register_voice_layout(layout);
    }

    // Stadium-side secondary voice table â€” array-style high-level synth
    // voices at D_800FC7D0 (count at 0x800FC7CC, array ptr at 0x800FC7D0).
    // The wavetable-pointer-ARRAY base lives at voice.unk_090 + 0x2C
    // (verified against the audio_diag hook capture: ctx->r11 = $t3 =
    // MEM_W($s0, 0x2C) where $s0 = arg0->unk_090). For Stadium's UAF
    // source (the 1 MiB SoundBank pool buffer freed at fragment36
    // cleanup), the array base AND its wavetable entries all live in
    // the same buffer, so range-checking the array base correctly
    // catches the freed-bank case. Silence by zeroing voice.unk_038 â€”
    // Stadium's func_8003AD58 skips voices whose unk_038 is null.
    // Routing this through audio_uaf_protect retires the per-scene
    // func_8004FF20 hook (was pkmnstadium_audio_stop_voices) and makes
    // coverage range-checked + global across all pool pops.
    {
        librecomp::audio_uaf::SecondaryVoiceTableLayout sec{};
        sec.count_vaddr           = 0x800FC7CCu;
        sec.array_ptr_vaddr       = 0x800FC7D0u;
        sec.voice_size            = 0x150u;
        sec.max_voice_count       = 64u;
        sec.chain_step_count      = 2u;
        sec.chain_offsets[0]      = 0x90u;  // voice.unk_090 -> inner struct ptr
        sec.chain_offsets[1]      = 0x2Cu;  // .unk_2C       -> wavetable-array base
        sec.silence_field_offset  = 0x38u;  // voice.unk_038
        sec.silence_value         = 0u;
        librecomp::audio_uaf::register_secondary_voice_table(sec);
    }

    return game.game_id;
}
