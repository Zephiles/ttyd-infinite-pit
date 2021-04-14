#include "randomizer.h"

#include "common_functions.h"
#include "common_types.h"
#include "common_ui.h"
#include "patch.h"
#include "randomizer_data.h"
#include "randomizer_patches.h"
#include "randomizer_state.h"

#include <gc/OSLink.h>
#include <ttyd/battle_actrecord.h>
#include <ttyd/battle_enemy_item.h>
#include <ttyd/battle_event_cmd.h>
#include <ttyd/battle_information.h>
#include <ttyd/battle_seq.h>
#include <ttyd/battle_unit.h>
#include <ttyd/cardmgr.h>
#include <ttyd/dispdrv.h>
#include <ttyd/event.h>
#include <ttyd/evtmgr.h>
#include <ttyd/item_data.h>
#include <ttyd/mario_pouch.h>
#include <ttyd/msgdrv.h>
#include <ttyd/npcdrv.h>
#include <ttyd/seqdrv.h>
#include <ttyd/seq_battle.h>
#include <ttyd/seq_title.h>
#include <ttyd/sound.h>
#include <ttyd/statuswindow.h>
#include <ttyd/swdrv.h>
#include <ttyd/system.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace mod::pit_randomizer {
    
Randomizer* g_Randomizer = nullptr;
    
namespace {

using ::gc::OSLink::OSModuleInfo;
using ::ttyd::battle_unit::BattleWorkUnit;
using ::ttyd::dispdrv::CameraId;
using ::ttyd::evtmgr::EvtEntry;
using ::ttyd::npcdrv::FbatBattleInformation;
using ::ttyd::npcdrv::NpcEntry;
using ::ttyd::seqdrv::SeqIndex;

namespace ItemType = ::ttyd::item_data::ItemType;

// Trampoline hooks for patching in custom logic to existing TTYD C functions.
void (*g_stg0_00_init_trampoline)(void) = nullptr;
void (*g_cardCopy2Main_trampoline)(int32_t) = nullptr;
bool (*g_OSLink_trampoline)(OSModuleInfo*, void*) = nullptr;
const char* (*g_msgSearch_trampoline)(const char*) = nullptr;
void (*g_seq_battleInit_trampoline)(void) = nullptr;
void (*g_fbatBattleMode_trampoline)(void) = nullptr;
void (*g_BtlActRec_JudgeRuleKeep_trampoline)(void) = nullptr;
void (*g__rule_disp_trampoline)(void) = nullptr;
void (*g_BattleInformationSetDropMaterial_trampoline)(FbatBattleInformation*) = nullptr;
int32_t (*g_btlevtcmd_GetItemRecoverParam_trampoline)(EvtEntry*, bool) = nullptr;
int32_t (*g_btlevtcmd_ConsumeItem_trampoline)(EvtEntry*, bool) = nullptr;
int32_t (*g_btlevtcmd_GetConsumeItem_trampoline)(EvtEntry*, bool) = nullptr;
void* (*g_BattleEnemyUseItemCheck_trampoline)(BattleWorkUnit*) = nullptr;
void (*g_seqSetSeq_trampoline)(SeqIndex, const char*, const char*) = nullptr;
void (*g_statusWinDisp_trampoline)(void) = nullptr;
void (*g_gaugeDisp_trampoline)(double, double, int32_t) = nullptr;

bool g_CueGameOver = false;

void DrawOptionsMenu() {
    g_Randomizer->menu_.Draw();
}

void DrawTitleScreenInfo() {
    const char* kTitleInfo = 
        "PM:TTYD Infinite Pit v1.41 r46 by jdaster64\n"
        "https://github.com/jdaster64/ttyd-infinite-pit\n"
        "Guide / Other mods: https://goo.gl/vjJjVd";
    DrawCenteredTextWindow(
        kTitleInfo, 0, -50, 0xFFu, true, 0xFFFFFFFFu, 0.7f, 0x000000E5u, 15, 10);
}

uint32_t secretCode_RtaTimer        = 034345566;
uint32_t secretCode_BonusOptions1   = 012651265;
uint32_t secretCode_BonusOptions2   = 043652131;
uint32_t secretCode_BonusOptions3   = 031313141;
uint32_t secretCode_UnlockFxBadges  = 026122146;

bool g_DrawRtaTimer = false;
void DrawRtaTimer() {
    // Print the current RTA timer and its position to the screen at all times.
    char buf[32];
    sprintf(buf, "%s", g_Randomizer->state_.GetCurrentTimeString());
    DrawText(buf, -260, -195, 0xFF, true, ~0U, 0.75f, /* center-left */ 3);
}

}
    
Randomizer::Randomizer() {}

void Randomizer::Init() {
    g_Randomizer = this;
    
    // Hook functions with custom logic.
    
    g_stg0_00_init_trampoline = patch::hookFunction(
        ttyd::event::stg0_00_init, []() {
            // Replaces existing logic, includes loading the randomizer state.
            OnFileLoad(/* new_file = */ true);
            ApplySettingBasedPatches();
        });
        
    g_cardCopy2Main_trampoline = patch::hookFunction(
        ttyd::cardmgr::cardCopy2Main, [](int32_t save_file_number) {
            g_cardCopy2Main_trampoline(save_file_number);
            OnFileLoad(/* new_file = */ false);
            // If invalid randomizer file loaded, give the player a Game Over.
            if (!g_Randomizer->state_.Load(/* new_save = */ false)) {
                g_CueGameOver = true;
            }
            ApplySettingBasedPatches();
        });
    
    g_OSLink_trampoline = patch::hookFunction(
        gc::OSLink::OSLink, [](OSModuleInfo* new_module, void* bss) {
            bool result = g_OSLink_trampoline(new_module, bss);
            if (new_module != nullptr && result) {
                OnModuleLoaded(new_module);
            }
            return result;
        });

    g_seq_battleInit_trampoline = patch::hookFunction(
        ttyd::seq_battle::seq_battleInit, []() {
            // Copy information from parent npc before battle, if applicable.
            CopyChildBattleInfo(/* to_child = */ true);
            g_seq_battleInit_trampoline();
        });

    g_fbatBattleMode_trampoline = patch::hookFunction(
        ttyd::npcdrv::fbatBattleMode, []() {
            bool post_battle_state = ttyd::npcdrv::fbatGetPointer()->state == 4;
            g_fbatBattleMode_trampoline();
            // Copy information back to parent npc after battle, if applicable.
            if (post_battle_state) CopyChildBattleInfo(/* to_child = */ false);
        });
    
    g_seqSetSeq_trampoline = patch::hookFunction(
        ttyd::seqdrv::seqSetSeq, 
        [](SeqIndex seq, const char* mapName, const char* beroName) {
            OnEnterExitBattle(/* is_start = */ seq == SeqIndex::kBattle);
            // Check for failed file load.
            if (g_CueGameOver) {
                seq = SeqIndex::kGameOver;
                mapName = reinterpret_cast<const char*>(1);
                beroName = 0;
                g_CueGameOver = false;
            } else if (
                seq == SeqIndex::kMapChange && !strcmp(mapName, "aaa_00") && 
                !strcmp(beroName, "prologue")) {
                // If loading a new file, load the player into the pre-Pit room.
                mapName = "tik_06";
                beroName = "e_bero";
            }
            g_seqSetSeq_trampoline(seq, mapName, beroName);
        });
        
    g_msgSearch_trampoline = patch::hookFunction(
        ttyd::msgdrv::msgSearch, [](const char* msg_key) {
            const char* replacement = GetReplacementMessage(msg_key);
            if (replacement) return replacement;
            return g_msgSearch_trampoline(msg_key);
        });
        
    g_BtlActRec_JudgeRuleKeep_trampoline = patch::hookFunction(
        ttyd::battle_actrecord::BtlActRec_JudgeRuleKeep, []() {
            g_BtlActRec_JudgeRuleKeep_trampoline();
            CheckBattleCondition();
        });
        
    g__rule_disp_trampoline = patch::hookFunction(
        ttyd::battle_seq::_rule_disp, []() {
            // Replaces the original logic completely.
            DisplayBattleCondition();
        });
        
    g_BattleInformationSetDropMaterial_trampoline = patch::hookFunction(
        ttyd::battle_information::BattleInformationSetDropMaterial,
        [](FbatBattleInformation* fbat_info) {
            // Replaces the original logic completely.
            GetDropMaterials(fbat_info);
        });
        
    g_btlevtcmd_GetItemRecoverParam_trampoline = patch::hookFunction(
        ttyd::battle_event_cmd::btlevtcmd_GetItemRecoverParam,
        [](EvtEntry* evt, bool isFirstCall) {
            g_btlevtcmd_GetItemRecoverParam_trampoline(evt, isFirstCall);
            // Run custom behavior to replace the recovery params in some cases.
            return GetAlteredItemRestorationParams(evt, isFirstCall);
        });
        
    g_btlevtcmd_ConsumeItem_trampoline = patch::hookFunction(
        ttyd::battle_event_cmd::btlevtcmd_ConsumeItem,
        [](EvtEntry* evt, bool isFirstCall) {
            EnemyConsumeItem(evt);
            return g_btlevtcmd_ConsumeItem_trampoline(evt, isFirstCall);
        });
        
    g_btlevtcmd_GetConsumeItem_trampoline = patch::hookFunction(
        ttyd::battle_event_cmd::btlevtcmd_GetConsumeItem,
        [](EvtEntry* evt, bool isFirstCall) {
            if (GetEnemyConsumeItem(evt)) return 2;
            return g_btlevtcmd_GetConsumeItem_trampoline(evt, isFirstCall);
        });
        
    g_BattleEnemyUseItemCheck_trampoline = patch::hookFunction(
        ttyd::battle_enemy_item::BattleEnemyUseItemCheck,
        [](BattleWorkUnit* unit) {
            void* evt_code = g_BattleEnemyUseItemCheck_trampoline(unit);
            if (!evt_code) {
                evt_code = EnemyUseAdditionalItemsCheck(unit);
            }
            return evt_code;
        });
        
    g_statusWinDisp_trampoline = patch::hookFunction(
        ttyd::statuswindow::statusWinDisp, []() {
            g_statusWinDisp_trampoline();
            DisplayStarPowerNumber();
        });
        
    g_gaugeDisp_trampoline = patch::hookFunction(
        ttyd::statuswindow::gaugeDisp, [](double x, double y, int32_t sp) {
            // Replaces the original logic completely.
            DisplayStarPowerOrbs(x, y, sp);
        });
        
    ApplyEnemyStatChangePatches();
    ApplyWeaponLevelSelectionPatches();
    ApplyItemAndAttackPatches();
    ApplyPlayerStatTrackingPatches();
    ApplyMiscPatches();
    
    // Initialize the menu.
    menu_.Init();
}

void Randomizer::Update() {
    menu_.Update();
    
    // Process cheat codes.
    static uint32_t code_history = 0;
    int32_t code = 0;
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::A) code = 1;
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::B) code = 2;
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::L) code = 3;
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::R) code = 4;
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::X) code = 5;
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::Y) code = 6;
    if (code) code_history = (code_history << 3) | code;
    if ((code_history & 0xFFFFFF) == secretCode_RtaTimer) {
        code_history = 0;
        g_DrawRtaTimer = true;
        ttyd::sound::SoundEfxPlayEx(0x265, 0, 0x64, 0x40);
    }
    if ((code_history & 0xFFFFFF) == secretCode_BonusOptions1) {
        code_history = 0;
        menu_.SetMenuPageVisibility(5, true);
        ttyd::sound::SoundEfxPlayEx(0x265, 0, 0x64, 0x40);
    }
    if ((code_history & 0xFFFFFF) == secretCode_BonusOptions2) {
        code_history = 0;
        menu_.SetMenuPageVisibility(6, true);
        ttyd::sound::SoundEfxPlayEx(0x265, 0, 0x64, 0x40);
    }
    if ((code_history & 0xFFFFFF) == secretCode_BonusOptions3) {
        code_history = 0;
        menu_.SetMenuPageVisibility(7, true);
        ttyd::sound::SoundEfxPlayEx(0x265, 0, 0x64, 0x40);
    }
    if ((code_history & 0xFFFFFF) == secretCode_UnlockFxBadges) {
        code_history = 0;
        // Check Journal for whether the FX badges were already unlocked.
        bool has_fx_badges = ttyd::swdrv::swGet(
            ItemType::ATTACK_FX_R - ItemType::POWER_JUMP + 0x80);
        if (!has_fx_badges && ttyd::mario_pouch::pouchGetHaveBadgeCnt() < 196) {
            ttyd::mario_pouch::pouchGetItem(ItemType::ATTACK_FX_P);
            ttyd::mario_pouch::pouchGetItem(ItemType::ATTACK_FX_G);
            ttyd::mario_pouch::pouchGetItem(ItemType::ATTACK_FX_B);
            ttyd::mario_pouch::pouchGetItem(ItemType::ATTACK_FX_Y);
            ttyd::mario_pouch::pouchGetItem(ItemType::ATTACK_FX_R);
            ttyd::sound::SoundEfxPlayEx(0x265, 0, 0x64, 0x40);
        }
    }
}

void Randomizer::Draw() {
    // Draw title screen info.
    if (CheckSeq(ttyd::seqdrv::SeqIndex::kTitle)) {
        const uint32_t curtain_state = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uintptr_t>(ttyd::seq_title::seqTitleWorkPointer2)
            + 0x8);
        if (curtain_state >= 2 && curtain_state < 12) {
            // Curtain is not fully down.
            RegisterDrawCallback(DrawTitleScreenInfo, CameraId::k2d);
        }
    }
    
    // Draw options menu.
    RegisterDrawCallback(DrawOptionsMenu, CameraId::k2d);
    
    // Draw RTA timer overlay, if enabled.
    if (InMainGameModes() && g_DrawRtaTimer) {
        RegisterDrawCallback(DrawRtaTimer, CameraId::kDebug3d);
    }
}

}