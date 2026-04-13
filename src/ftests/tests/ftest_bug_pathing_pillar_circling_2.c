/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file ftest_bug_pathing_pillar_circling_2.c
 *     Functional test catching imps indefinitely circling pillars.
 */
/******************************************************************************/
#include "../../pre_inc.h"
#include "../ftest.h"
#include "../ftest_util.h"
#include "ftest_bug_pathing_pillar_circling_2.h"

#include "../../game_legacy.h"
#include "../../keeperfx.hpp"
#include "../../map_data.h"
#include "../../map_blocks.h"
#include "../../player_instances.h"
#include "../../thing_list.h"
#include "../../thing_creature.h"
#include "../../creature_control.h"
#include "../../creature_states.h"
#include "../../config_creature.h"
#include "../../post_inc.h"

struct ftest_bug_pathing_pillar_circling_2__variables {
    int stuck_timer[THINGS_COUNT];
};

struct ftest_bug_pathing_pillar_circling_2__variables ftest_bug_pathing_pillar_circling_2__vars = {0};

static FTestActionResult ftest_bug_pathing_pillar_circling_2_action001__setup_map(struct FTestActionArgs* const args) {
    ftest_util_reveal_map(PLAYER0);

    // Clear test area to rock to establish a blank canvas.
    // We only clear the required area to avoid destroying the existing Dungeon Heart.
    ftest_util_replace_slabs(2, 2, 35, 52, SlbT_ROCK, PLAYER0);
    
    // Create 1 large 8x8 Treasury
    ftest_util_replace_slabs(25, 5, 32, 12, SlbT_TREASURE, PLAYER0);
    
    // Central claimed path connecting the Treasury to all Workshops
    ftest_util_replace_slabs(15, 3, 24, 49, SlbT_CLAIMED, PLAYER0);

    // Create Workshops sizes 2x2 up to 6x6
    int start_y = 5;
    for (int size = 2; size <= 6; size++) {
        int start_x = 5;
        
        // Encase the entire workshop interior + 1 exterior layer in Gems
        ftest_util_replace_slabs(start_x - 1, start_y - 1, start_x + size, start_y + size, SlbT_GEMS, PLAYER0);

        // Fill interior with Workshop slabs
        ftest_util_replace_slabs(start_x, start_y, start_x + size - 1, start_y + size - 1, SlbT_WORKSHOP, PLAYER0);

        // Tag the exterior gems to be excavated
        for (int gy = start_y - 1; gy <= start_y + size; gy++) {
            for (int gx = start_x - 1; gx <= start_x + size; gx++) {
                if (gx == start_x - 1 || gx == start_x + size || gy == start_y - 1 || gy == start_y + size) {
                    game_action(PLAYER0, GA_MarkDig, 0, slab_subtile_center(gx), slab_subtile_center(gy), 1, 1);
                }
            }
        }
        
        // Connect this workshop to the main path
        ftest_util_replace_slabs(start_x + size, start_y + size/2, 14, start_y + size/2, SlbT_CLAIMED, PLAYER0);
        
        // Spawn exactly 20 Imps inside the current Workshop
        for (int i = 0; i < 20; i++) {
            struct Coord3d pos;
            set_coords_to_slab_center(&pos, start_x + size/2, start_y + size/2);
            struct Thing *imp = create_creature(&pos, get_players_special_digger_model(PLAYER0), PLAYER0);
            if (!thing_is_invalid(imp)) {
                FTESTLOG("Spawned Imp %d (Name: %s)", (int)imp->index, creature_own_name(imp));
            }
        }
        
        start_y += size + 3; // Shift Y down to leave space for the next size room
    }
    
    ftest_util_move_camera_to_slab(15, 20, PLAYER0);
    return FTRs_Go_To_Next_Action;
}

static FTestActionResult ftest_bug_pathing_pillar_circling_2_action002__monitor(struct FTestActionArgs* const args) {
    struct ftest_bug_pathing_pillar_circling_2__variables *v = args->data;
    
    if (game.play_gameturn > 3000) {
        return FTRs_Go_To_Next_Action; // 3000 turns without getting stuck = pass
    }

    for (long i = 1; i < THINGS_COUNT; i++) {
        struct Thing *thing = thing_get(i);
        if (thing_is_invalid(thing) || !thing_is_creature(thing)) continue;
        
        // Only print debug info periodically to avoid massive log flooding
        if ((i == 71 || i == 75) && (game.play_gameturn % 50 == 0)) {
            struct CreatureControl *cctrl = creature_control_get_from_thing(thing);
            FTESTLOG("Imp %ld (%s) State: %d | Timer: %d | Pos: %d,%d (Stl: %d,%d) | Target: %d,%d (Stl: %d,%d) | Angle: %d | Vel: %d,%d | ColBlk: %d,%d",
                i, creature_own_name(thing), thing->active_state, v->stuck_timer[i],
                thing->mappos.x.val, thing->mappos.y.val,
                thing->mappos.x.stl.num, thing->mappos.y.stl.num,
                cctrl->moveto_pos.x.val, cctrl->moveto_pos.y.val,
                cctrl->moveto_pos.x.stl.num, cctrl->moveto_pos.y.stl.num,
                thing->move_angle_xy,
                thing->velocity.x.val, thing->velocity.y.val,
                stl_num_decode_x(cctrl->navi.first_colliding_block),
                stl_num_decode_y(cctrl->navi.first_colliding_block));
        }

        if (thing->active_state == CrSt_MoveToPosition) {
            v->stuck_timer[i]++;
            if (v->stuck_timer[i] == 100) {
                FTESTLOG("Warning: Imp %ld (Name: %s) has been in MoveToPosition for 100 turns...", i, creature_own_name(thing));
            }
            if (v->stuck_timer[i] > 1000) {
                FTEST_FAIL_TEST("Imp %ld (Name: %s) is stuck indefinitely circling/pathing! (active_state == MoveToPosition for 1000 turns)", i, creature_own_name(thing));
                return FTRs_Go_To_Next_Action;
            }
        } else {
            // Decay the timer instead of instantly resetting. This allows the stuck 
            // detection to survive brief 1-2 turn state interruptions when bumping/recalculating.
            if (v->stuck_timer[i] > 0) v->stuck_timer[i] -= 2;
            if (v->stuck_timer[i] < 0) v->stuck_timer[i] = 0;
        }
    }
    return FTRs_Repeat_Current_Action;
}

TbBool ftest_bug_pathing_pillar_circling_2_init() {
    memset(&ftest_bug_pathing_pillar_circling_2__vars, 0, sizeof(struct ftest_bug_pathing_pillar_circling_2__variables));
    ftest_append_action(ftest_bug_pathing_pillar_circling_2_action001__setup_map, 10, &ftest_bug_pathing_pillar_circling_2__vars);
    ftest_append_action(ftest_bug_pathing_pillar_circling_2_action002__monitor, 10, &ftest_bug_pathing_pillar_circling_2__vars);
    return true;
}