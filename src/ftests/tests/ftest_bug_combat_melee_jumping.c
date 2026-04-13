#include "ftest_bug_combat_melee_jumping.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"
#include "../ftest.h"
#include "../ftest_util.h"
#include "../../game_legacy.h"
#include "../../keeperfx.hpp"
#include "../../player_instances.h"
#include "../../config_creature.h"
#include "../../thing_list.h"
#include "../../creature_control.h"
#include "../../thing_creature.h"
#include "../../creature_states.h"
#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ftest_bug_combat_melee_jumping__variables
{
    MapSlabCoord slb_x_center;
    MapSlabCoord slb_y_center;
    ThingIndex enemy1_idx;
    ThingIndex enemy2_idx;
};

struct ftest_bug_combat_melee_jumping__variables ftest_bug_combat_melee_jumping__vars = {
    .slb_x_center = 42,
    .slb_y_center = 42,
    .enemy1_idx = 0,
    .enemy2_idx = 0
};

FTestActionResult ftest_bug_combat_melee_jumping_action001__setup(struct FTestActionArgs* const args);
FTestActionResult ftest_bug_combat_melee_jumping_action002__spawn_enemy2(struct FTestActionArgs* const args);
FTestActionResult ftest_bug_combat_melee_jumping_action003__check(struct FTestActionArgs* const args);
FTestActionResult ftest_bug_combat_melee_jumping_action004__wait(struct FTestActionArgs* const args);

TbBool ftest_bug_combat_melee_jumping_init()
{
    ftest_append_action(ftest_bug_combat_melee_jumping_action001__setup, 20, &ftest_bug_combat_melee_jumping__vars);
    ftest_append_action(ftest_bug_combat_melee_jumping_action002__spawn_enemy2, 100, &ftest_bug_combat_melee_jumping__vars);
    ftest_append_action(ftest_bug_combat_melee_jumping_action003__check, 100, &ftest_bug_combat_melee_jumping__vars);
    ftest_append_action(ftest_bug_combat_melee_jumping_action004__wait, 500, &ftest_bug_combat_melee_jumping__vars);
    return true;
}

FTestActionResult ftest_bug_combat_melee_jumping_action001__setup(struct FTestActionArgs* const args)
{
    struct ftest_bug_combat_melee_jumping__variables* const vars = args->data;
    ftest_util_reveal_map(PLAYER0);

    // Carve out a 5x5 arena in the earth
    ftest_util_replace_slabs(vars->slb_x_center - 2, vars->slb_y_center - 2, vars->slb_x_center + 2, vars->slb_y_center + 2, SlbT_CLAIMED, PLAYER0);
    
    struct Coord3d center_pos;
    set_coords_to_slab_center(&center_pos, vars->slb_x_center, vars->slb_y_center);

    // Spawn 1 Enemy Dragon (Level 10) in the center
    struct Thing* enemy1 = ftest_util_create_creature(center_pos.x.val, center_pos.y.val, PLAYER1, 10, creature_model_id("DRAGON"));
    vars->enemy1_idx = enemy1->index;

    // Spawn 25 Friendly Demon Spawns (Level 1) scattered around the Dragon
    for (int i = 0; i < 25; i++) {
        MapCoord ds_x = center_pos.x.val + (GAME_RANDOM(1024)) - 512;
        MapCoord ds_y = center_pos.y.val + (GAME_RANDOM(1024)) - 512;
        ftest_util_create_creature(ds_x, ds_y, PLAYER0, 1, creature_model_id("DEMONSPAWN"));
    }

    ftest_util_move_camera_to_slab(vars->slb_x_center, vars->slb_y_center, PLAYER0);
    return FTRs_Go_To_Next_Action; 
}

FTestActionResult ftest_bug_combat_melee_jumping_action002__spawn_enemy2(struct FTestActionArgs* const args)
{
    struct ftest_bug_combat_melee_jumping__variables* const vars = (struct ftest_bug_combat_melee_jumping__variables* const)args->data;
    
    struct Coord3d pos2;
    set_coords_to_slab_center(&pos2, vars->slb_x_center + 1, vars->slb_y_center + 1);
    
    struct Thing* enemy2 = ftest_util_create_creature(pos2.x.val, pos2.y.val, PLAYER1, 10, creature_model_id("DRAGON"));
    vars->enemy2_idx = enemy2->index;
    
    FTESTLOG("Spawned second enemy %s index %d at (%d, %d)", thing_model_name(enemy2), enemy2->index, pos2.x.stl.num, pos2.y.stl.num);
    
    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_bug_combat_melee_jumping_action003__check(struct FTestActionArgs* const args)
{
    struct ftest_bug_combat_melee_jumping__variables* const vars = (struct ftest_bug_combat_melee_jumping__variables* const)args->data;
    int enemy1_attackers = 0;
    int enemy2_attackers = 0;
    int waiting_count = 0;
    int total_friendlies = 0;

    const struct StructureList *slist = get_list_for_thing_class(TCls_Creature);
    long i = slist->index;
    while (i != 0) {
        struct Thing *thing = thing_get(i);
        i = thing->next_of_class;
        if (thing->owner == PLAYER0 && thing_is_creature(thing)) {
            total_friendlies++;
            struct CreatureControl* cctrl = creature_control_get_from_thing(thing);
            if (cctrl->combat_flags & CmbtF_Waiting) {
                waiting_count++;
            } else if (cctrl->combat_flags & CmbtF_Melee) {
                if (cctrl->combat.battle_enemy_idx == vars->enemy1_idx) enemy1_attackers++;
                if (cctrl->combat.battle_enemy_idx == vars->enemy2_idx) enemy2_attackers++;
            }
        }
    }

    FTESTLOG("--- COMBAT STATE REPORT ---");
    FTESTLOG("Total Friendlies: %d", total_friendlies);
    FTESTLOG("Attackers on Enemy 1 (idx %d): %d", vars->enemy1_idx, enemy1_attackers);
    FTESTLOG("Attackers on Enemy 2 (idx %d): %d", vars->enemy2_idx, enemy2_attackers);
    FTESTLOG("Waiting Friendlies: %d", waiting_count);
    FTESTLOG("---------------------------");

    if (waiting_count > 0 && enemy2_attackers == 0) {
        FTEST_FAIL_TEST("Waiting friendly units did not engage the second enemy!");
    } else if (enemy2_attackers > 0) {
        FTESTLOG("Success: Friendlies successfully engaged the new threat.");
    } else {
        FTESTLOG("No waiting units found, or all slots not filled yet.");
    }
    
    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_bug_combat_melee_jumping_action004__wait(struct FTestActionArgs* const args)
{
    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif
#endif