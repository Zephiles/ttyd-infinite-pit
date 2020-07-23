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
#include <ttyd/battle_unit.h>
#include <ttyd/dispdrv.h>
#include <ttyd/evtmgr.h>
#include <ttyd/msgdrv.h>
#include <ttyd/npcdrv.h>
#include <ttyd/seqdrv.h>
#include <ttyd/seq_title.h>
#include <ttyd/system.h>

#include <cstdint>
#include <cstdio>

namespace mod::pit_randomizer {
    
Randomizer* g_Randomizer = nullptr;
    
namespace  {

using ::gc::OSLink::OSModuleInfo;
using ::ttyd::battle_unit::BattleWorkUnit;
using ::ttyd::dispdrv::CameraId;
using ::ttyd::evtmgr::EvtEntry;
using ::ttyd::npcdrv::NpcEntry;

// Trampoline hooks for patching in custom logic to existing TTYD C functions.
bool (*g_OSLink_trampoline)(OSModuleInfo*, void*) = nullptr;
const char* (*g_msgSearch_trampoline)(const char*) = nullptr;
void (*g_npcSetupBattleInfo_trampoline)(NpcEntry*, void*) = nullptr;
void (*g_BtlActRec_JudgeRuleKeep_trampoline)(void) = nullptr;
int32_t (*g_btlevtcmd_ConsumeItem_trampoline)(EvtEntry*, bool) = nullptr;
int32_t (*g_btlevtcmd_GetConsumeItem_trampoline)(EvtEntry*, bool) = nullptr;
void* (*g_BattleEnemyUseItemCheck_trampoline)(BattleWorkUnit*) = nullptr;

void DrawTitleScreenInfo() {
    const char* kTitleInfo =
        "Pit of Infinite Trials v0.00 by jdaster64\nPUT GITHUB LINK HERE";
    DrawCenteredTextWindow(
        kTitleInfo, 0, -50, 0xFFu, 0xFFFFFFFFu, 0.75f, 0x000000E5u, 15, 10);
}

// TODO: REMOVE, for TESTING ONLY.
void DrawDebuggingFunctions() {
    uint32_t& enemyTypeToTest = g_Randomizer->state_.debug_[0];
    
    // D-Pad Up or Down to change the type of enemy to test.
    if (ttyd::system::keyGetButtonTrg(0) & ButtonId::DPAD_UP) {
        ++enemyTypeToTest;
    } else if (ttyd::system::keyGetButtonTrg(0) & ButtonId::DPAD_RIGHT) {
        enemyTypeToTest += 10;
    } else if (ttyd::system::keyGetButtonTrg(0) & ButtonId::DPAD_DOWN) {
        --enemyTypeToTest;
    } else if (ttyd::system::keyGetButtonTrg(0) & ButtonId::DPAD_LEFT) {
        enemyTypeToTest -= 10;
    }
    if (enemyTypeToTest > 105) enemyTypeToTest = 105;
    if (enemyTypeToTest < 1) enemyTypeToTest = 1;
    
    // Print the current enemy type to the screen at all times.
    char buf[16];
    sprintf(buf, "%d", enemyTypeToTest);
    DrawCenteredTextWindow(
        buf, -200, -150, 0xFFu, 0xFFFFFFFFu, 0.75f, 0x000000E5u, 15, 10);
}

}
    
Randomizer::Randomizer() {}

void Randomizer::Init() {
    g_Randomizer = this;
    state_.InitializeRandomizerState(/* new_save = */ true);
    
    // Hook functions with custom logic.
    
    g_OSLink_trampoline = patch::hookFunction(
        gc::OSLink::OSLink, [](OSModuleInfo* new_module, void* bss) {
            bool result = g_OSLink_trampoline(new_module, bss);
            if (new_module != nullptr && result) {
                OnModuleLoaded(new_module);
            }
            return result;
        });
        
    g_msgSearch_trampoline = patch::hookFunction(
        ttyd::msgdrv::msgSearch, [](const char* msg_key) {
            const char* replacement = GetReplacementMessage(msg_key);
            if (replacement) return replacement;
            return g_msgSearch_trampoline(msg_key);
        });
        
    g_npcSetupBattleInfo_trampoline = patch::hookFunction(
        ttyd::npcdrv::npcSetupBattleInfo, [](NpcEntry* npc, void* battleInfo) {
            g_npcSetupBattleInfo_trampoline(npc, battleInfo);
            SetBattleCondition(&npc->battleInfo);
        });
        
    g_BtlActRec_JudgeRuleKeep_trampoline = patch::hookFunction(
        ttyd::battle_actrecord::BtlActRec_JudgeRuleKeep, []() {
            g_BtlActRec_JudgeRuleKeep_trampoline();
            CheckBattleCondition();
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
        
    ApplyItemAndAttackPatches();
    ApplyMiscPatches();
}

void Randomizer::Update() {}

void Randomizer::Draw() {
    if (CheckSeq(ttyd::seqdrv::SeqIndex::kTitle)) {
        const uint32_t curtain_state = *reinterpret_cast<uint32_t*>(
            reinterpret_cast<uintptr_t>(ttyd::seq_title::seqTitleWorkPointer2)
            + 0x8);
        if (curtain_state >= 2 && curtain_state < 12) {
            // Curtain is not fully down; draw title screen info.
            RegisterDrawCallback(DrawTitleScreenInfo, CameraId::k2d);
        }
    }
    // TODO: REMOVE, for TESTING ONLY.
    if (InMainGameModes()) {
        RegisterDrawCallback(DrawDebuggingFunctions, CameraId::k2d);
    }
}

}