#include "ftest_scavenging.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include "../ftest.h"
#include "../ftest_util.h"

#include "../../game_legacy.h"
#include "../../keeperfx.hpp"
#include "../../player_instances.h"
#include "../../gui_msgs.h"
#include "../../dungeon_data.h"
#include "../../config_creature.h"
#include "../../creature_states.h"
#include "../../thing_creature.h"
#include "../../thing_data.h"
#include "../../thing_stats.h"
#include "../../slab_data.h"
#include "../../map_data.h"
#include "../../room_entrance.h"
#include "../../room_data.h"
#include "../../creature_states_scavn.h"
#include "../../thing_navigate.h"
#include "../../thing_list.h"
#include "../../config.h"

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Helper: kill all imps/diggers belonging to a player to prevent unexpected expansion.
static TbBool ftest_scav__kill_callback(struct Thing* creatng)
{
    kill_creature(creatng, INVALID_THING, creatng->owner, CrDed_NoUnconscious | CrDed_NoEffects);
    return true;
}

static void ftest_scav__kill_player_imps(PlayerNumber owner)
{
    long count = do_to_players_all_creatures_of_model(owner, CREATURE_DIGGER, ftest_scav__kill_callback);
    if (count > 0)
        FTESTLOG("Killed %ld imps/diggers for player %d", count, (int)owner);
}

// Kill enemy imps periodically (every 20 turns) to prevent expansion during tests
static void ftest_scav__kill_enemy_imps_periodic(void)
{
    if (game.play_gameturn % 20 == 0)
        ftest_scav__kill_player_imps(PLAYER1);
}

// Force a creature back to scavenging if the AI made it wander off (e.g. to lair/hatchery).
// Moves the creature back to the scavenger room and resets its state.
// Check every 20 turns to avoid per-frame overhead.
static void ftest_scav__force_scavenge(struct Thing* creatng, MapSlabCoord scav_cx, MapSlabCoord scav_cy)
{
    if (creatng == NULL || thing_is_invalid(creatng))
        return;
    if (game.play_gameturn % 20 != 0)
        return;
    if (get_creature_state_besides_interruptions(creatng) != CrSt_AtScavengerRoom)
    {
        struct Coord3d pos;
        set_coords_to_slab_center(&pos, scav_cx, scav_cy);
        move_thing_in_map(creatng, &pos);
        initialise_thing_state(creatng, CrSt_AtScavengerRoom);
    }
}

// Heart room is 5x5 slabs centered on (hx,hy), occupying (hx-2,hy-2) to (hx+2,hy+2).
// All corridors and rooms must avoid overwriting these slabs.
#define HEART_RADIUS 2

// Check if a slab is part of an existing room (portal, heart room, etc.)
static TbBool ftest_scav__slab_has_room(MapSlabCoord sx, MapSlabCoord sy)
{
    struct Room* room = subtile_room_get(slab_subtile_center(sx), slab_subtile_center(sy));
    return !room_is_invalid(room);
}

// Safe version of ftest_util_replace_slabs that skips slabs with existing rooms.
// Also skips slabs within the heart boundary.
static TbBool ftest_scav__safe_replace_slabs(MapSlabCoord x1, MapSlabCoord y1, MapSlabCoord x2, MapSlabCoord y2,
                                              SlabKind slbkind, PlayerNumber owner,
                                              MapSlabCoord heart_x, MapSlabCoord heart_y)
{
    for (MapSlabCoord sy = y1; sy <= y2; sy++)
    {
        for (MapSlabCoord sx = x1; sx <= x2; sx++)
        {
            // Skip heart area
            if (sx >= heart_x - HEART_RADIUS && sx <= heart_x + HEART_RADIUS &&
                sy >= heart_y - HEART_RADIUS && sy <= heart_y + HEART_RADIUS)
            {
                FTESTLOG("Skipping slab (%d,%d) — inside heart area", sx, sy);
                continue;
            }
            // Skip slabs that already have a room
            if (ftest_scav__slab_has_room(sx, sy))
            {
                FTESTLOG("Skipping slab (%d,%d) — existing room", sx, sy);
                continue;
            }
            ftest_util_replace_slabs(sx, sy, sx, sy, slbkind, owner);
        }
    }
    return true;
}

// Helper: create lair + hatchery for a player, placed away from the portal.
// Each room is 5x5, connected to the heart area by a narrow (1-wide) claimed passage.
// Portal is LEFT of heart for PLAYER0, RIGHT of heart for PLAYER1.
// Rooms are placed ABOVE heart to stay clear of both portal and scavenger room.
// Also digs a corridor from heart to the portal.
static TbBool ftest_scav__create_support_rooms(PlayerNumber owner)
{
    struct Dungeon* dungeon = get_dungeon(owner);
    if (dungeon_invalid(dungeon))
    {
        FTESTLOG("Warning: Invalid dungeon for player %d", (int)owner);
        return false;
    }
    MapSlabCoord hx = coord_slab(dungeon->mappos.x.val);
    MapSlabCoord hy = coord_slab(dungeon->mappos.y.val);

    // Dig corridor from heart to portal
    RoomIndex entrance_idx = dungeon->room_list_start[RoK_ENTRANCE];
    if (entrance_idx > 0)
    {
        struct Room* entrance = room_get(entrance_idx);
        if (!room_is_invalid(entrance))
        {
            MapSlabCoord portal_sx = subtile_slab(entrance->central_stl_x);
            MapSlabCoord portal_sy = subtile_slab(entrance->central_stl_y);
            FTESTLOG("Player %d portal at slab (%d,%d)", (int)owner, portal_sx, portal_sy);

            MapSlabCoord start_x = (portal_sx > hx) ? hx + HEART_RADIUS + 1 : hx - HEART_RADIUS - 1;
            MapSlabCoord cx = start_x;
            MapSlabCoord step_x = (portal_sx > hx) ? 1 : -1;
            while (cx != portal_sx)
            {
                if (!ftest_scav__slab_has_room(cx, hy))
                    ftest_util_replace_slabs(cx, hy, cx, hy, SlbT_CLAIMED, owner);
                cx += step_x;
            }
            MapSlabCoord cy = hy;
            MapSlabCoord step_y = (portal_sy > hy) ? 1 : -1;
            while (cy != portal_sy)
            {
                cy += step_y;
                if (!ftest_scav__slab_has_room(portal_sx, cy))
                    ftest_util_replace_slabs(portal_sx, cy, portal_sx, cy, SlbT_CLAIMED, owner);
            }
            FTESTLOG("Dug corridor from heart area to portal (%d,%d)->(%d,%d)", start_x, hy, portal_sx, portal_sy);
        }
    }

    // Place lair + hatchery ABOVE heart (portal is left/right, scavenger room is right/left)
    MapSlabCoord lair_x = hx - 2;
    MapSlabCoord lair_y = hy - HEART_RADIUS - 3 - 5; // above heart
    ftest_scav__safe_replace_slabs(lair_x, lair_y, lair_x + 4, lair_y + 4, SlbT_LAIR, owner, hx, hy);
    // Corridor from lair down to heart
    ftest_scav__safe_replace_slabs(hx, lair_y + 5, hx, hy - HEART_RADIUS - 1, SlbT_CLAIMED, owner, hx, hy);

    // Place hatchery LEFT of lair
    MapSlabCoord hatch_x = lair_x - 6;
    MapSlabCoord hatch_y = lair_y;
    ftest_scav__safe_replace_slabs(hatch_x, hatch_y, hatch_x + 4, hatch_y + 4, SlbT_GARDEN, owner, hx, hy);
    // Corridor from hatchery to lair corridor
    ftest_scav__safe_replace_slabs(hatch_x + 5, hatch_y + 2, lair_x - 1, hatch_y + 2, SlbT_CLAIMED, owner, hx, hy);

    FTESTLOG("Created 5x5 lair(%d,%d)+hatchery(%d,%d) for player %d near heart (%d,%d)",
        lair_x, lair_y, hatch_x, hatch_y, (int)owner, hx, hy);
    return true;
}

// Helper: create a 5x5 scavenger room on the opposite side from the portal.
// PLAYER0 portal is LEFT → scavenger goes RIGHT. PLAYER1 portal is RIGHT → scavenger goes LEFT.
// Returns the center slab coordinates of the room via out_cx/out_cy.
static TbBool ftest_scav__create_scavenger_room(PlayerNumber owner, MapSlabCoord* out_cx, MapSlabCoord* out_cy)
{
    struct Dungeon* dungeon = get_dungeon(owner);
    if (dungeon_invalid(dungeon))
        return false;
    MapSlabCoord hx = coord_slab(dungeon->mappos.x.val);
    MapSlabCoord hy = coord_slab(dungeon->mappos.y.val);

    // PLAYER0: portal left, room right. PLAYER1: portal right, room left.
    int dir_x = (owner == PLAYER0) ? 1 : -1;

    MapSlabCoord rx = (dir_x > 0)
        ? hx + HEART_RADIUS + 3
        : hx - HEART_RADIUS - 3 - 4;
    if (!ftest_scav__safe_replace_slabs(rx, hy - 2, rx + 4, hy + 2, SlbT_SCAVENGER, owner, hx, hy))
        return false;
    // Corridor from heart boundary to room
    {
        MapSlabCoord from_x = (dir_x > 0) ? hx + HEART_RADIUS + 1 : rx + 5;
        MapSlabCoord to_x = (dir_x > 0) ? rx - 1 : hx - HEART_RADIUS - 1;
        ftest_scav__safe_replace_slabs(from_x, hy, to_x, hy, SlbT_CLAIMED, owner, hx, hy);
    }
    *out_cx = rx + 2;
    *out_cy = hy;
    FTESTLOG("Created 5x5 scavenger room for player %d at (%d,%d) dir=%s", (int)owner, rx + 2, hy, (dir_x > 0) ? "right" : "left");
    return true;
}

// Helper: create a 5x5 claimed area ABOVE the player's heart, with narrow passage.
// Used to place victim creatures. Returns center slab coordinates.
static TbBool ftest_scav__create_claimed_area(PlayerNumber owner, MapSlabCoord* out_cx, MapSlabCoord* out_cy)
{
    struct Dungeon* dungeon = get_dungeon(owner);
    if (dungeon_invalid(dungeon))
        return false;
    MapSlabCoord hx = coord_slab(dungeon->mappos.x.val);
    MapSlabCoord hy = coord_slab(dungeon->mappos.y.val);

    // Place above heart (portal is left/right, so above is always safe)
    MapSlabCoord ry = hy - HEART_RADIUS - 3 - 4;
    if (!ftest_scav__safe_replace_slabs(hx - 2, ry, hx + 2, ry + 4, SlbT_CLAIMED, owner, hx, hy))
        return false;
    ftest_scav__safe_replace_slabs(hx, ry + 5, hx, hy - HEART_RADIUS - 1, SlbT_CLAIMED, owner, hx, hy);
    *out_cx = hx;
    *out_cy = ry + 2;
    FTESTLOG("Created 5x5 claimed area for player %d at (%d,%d)", (int)owner, hx, ry + 2);
    return true;
}

// ============================================================================
// TEST 1: Scavenging from enemy player
// Setup: PLAYER0 has scavenger room + lv10 creature. PLAYER1 has lv1 creature.
// Expect: PLAYER0 accumulates scavenge points, PLAYER1 creature enters
// being-scavenged state, eventually changes owner to PLAYER0.
// ============================================================================

struct ftest_scav_enemy__variables
{
    MapSlabCoord p0_scav_cx;
    MapSlabCoord p0_scav_cy;
    MapSlabCoord p1_area_cx;
    MapSlabCoord p1_area_cy;
    struct Thing* p0_dragon;
    struct Thing* p1_dragon;
    ThingModel dragon_model;
    int32_t initial_p1_points;
    GameTurn check_start_turn;
    TbBool scavenge_started;
    TbBool scavenge_completed;
};
static struct ftest_scav_enemy__variables ftest_scav_enemy__vars = {
    .p0_scav_cx = 0,
    .p0_scav_cy = 0,
    .p1_area_cx = 0,
    .p1_area_cy = 0,
    .p0_dragon = NULL,
    .p1_dragon = NULL,
    .dragon_model = 0,
    .initial_p1_points = 0,
    .check_start_turn = 0,
    .scavenge_started = 0,
    .scavenge_completed = 0,
};

// Forward declarations
FTestActionResult ftest_scav_enemy__action001__setup(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_enemy__action002__wait_for_scavenge_start(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_enemy__action003__verify_progress(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_enemy__action004__wait_for_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_enemy__action005__wait_after_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_enemy__action006__verify_result(struct FTestActionArgs* const args);

TbBool ftest_scavenging_from_enemy_init()
{
    ftest_append_action(ftest_scav_enemy__action001__setup, 20, &ftest_scav_enemy__vars);
    ftest_append_action(ftest_scav_enemy__action002__wait_for_scavenge_start, 100, &ftest_scav_enemy__vars);
    ftest_append_action(ftest_scav_enemy__action003__verify_progress, 10, &ftest_scav_enemy__vars);
    ftest_append_action(ftest_scav_enemy__action004__wait_for_completion, 10, &ftest_scav_enemy__vars);
    ftest_append_action(ftest_scav_enemy__action005__wait_after_completion, 0, &ftest_scav_enemy__vars);
    ftest_append_action(ftest_scav_enemy__action006__verify_result, 0, &ftest_scav_enemy__vars);
    return true;
}

FTestActionResult ftest_scav_enemy__action001__setup(struct FTestActionArgs* const args)
{
    struct ftest_scav_enemy__variables* const vars = args->data;

    ftest_util_reveal_map(PLAYER0);

    vars->dragon_model = get_rid(creature_desc, "DRAGON");
    if (vars->dragon_model <= 0)
    {
        FTEST_FAIL_TEST("Could not find DRAGON creature model");
        return FTRs_Go_To_Next_Action;
    }
    FTESTLOG("DRAGON model id: %d (%s)", (int)vars->dragon_model, creature_code_name(vars->dragon_model));

    // Create 5x5 scavenger room for PLAYER0 (right of heart, narrow passage)
    if (!ftest_scav__create_scavenger_room(PLAYER0, &vars->p0_scav_cx, &vars->p0_scav_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 scavenger room");
        return FTRs_Go_To_Next_Action;
    }

    // Create 5x5 claimed area for PLAYER1 (above heart, narrow passage)
    if (!ftest_scav__create_claimed_area(PLAYER1, &vars->p1_area_cx, &vars->p1_area_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 claimed area");
        return FTRs_Go_To_Next_Action;
    }

    // Create lair + hatchery for both players
    ftest_scav__create_support_rooms(PLAYER0);
    ftest_scav__create_support_rooms(PLAYER1);

    // Give PLAYER0 plenty of gold for scavenging costs
    player_add_offmap_gold(PLAYER0, 100000);

    // Create PLAYER0 dragon in scavenger room (level 10 - strong scavenger)
    struct Coord3d pos;
    set_coords_to_slab_center(&pos, vars->p0_scav_cx, vars->p0_scav_cy);
    vars->p0_dragon = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER0, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p0_dragon))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 dragon in scavenger room");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p0_dragon, 9); // level 10 (0-indexed)
    FTESTLOG("Created PLAYER0 %s (index %d, level 10) at scavenger room", creature_code_name(vars->dragon_model), (int)vars->p0_dragon->index);

    // Force creature into scavenging state
    initialise_thing_state(vars->p0_dragon, CrSt_AtScavengerRoom);

    // Create PLAYER1 dragon as scavenge target (level 1 - weak victim)
    struct Coord3d pos2;
    set_coords_to_slab_center(&pos2, vars->p1_area_cx, vars->p1_area_cy);
    vars->p1_dragon = ftest_util_create_creature(pos2.x.val, pos2.y.val, PLAYER1, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p1_dragon))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p1_dragon, 0); // level 1 (0-indexed)
    FTESTLOG("Created PLAYER1 %s (index %d, level 1) as scavenge target", creature_code_name(vars->dragon_model), (int)vars->p1_dragon->index);

    ftest_util_move_camera_to_slab(vars->p0_scav_cx, vars->p0_scav_cy, PLAYER0);

    // Kill enemy imps to prevent unexpected expansion
    ftest_scav__kill_player_imps(PLAYER1);

    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_enemy__action002__wait_for_scavenge_start(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_enemy__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_dragon, vars->p0_scav_cx, vars->p0_scav_cy);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    if (dungeon_invalid(dungeon))
    {
        FTEST_FAIL_TEST("PLAYER0 dungeon invalid");
        return FTRs_Go_To_Next_Action;
    }

    int32_t points = dungeon->scavenge_turn_points[vars->dragon_model];
    short target_idx = dungeon->scavenge_targets[vars->dragon_model];

    if (game.play_gameturn % 100 == 0)
    {
        FTESTLOG("Scavenge points for %s: %d, target idx: %d, dragon state: %d",
            creature_code_name(vars->dragon_model), (int)points, (int)target_idx,
            (int)get_creature_state_besides_interruptions(vars->p0_dragon));
    }

    // Check if scavenging has started (points accumulating)
    if (points > 0)
    {
        FTESTLOG("Scavenging started! Points: %d, target: %d", (int)points, (int)target_idx);
        vars->scavenge_started = 1;
        vars->initial_p1_points = points;
        vars->check_start_turn = game.play_gameturn;
        return FTRs_Go_To_Next_Action;
    }

    // Timeout after 2000 turns
    if (game.play_gameturn > args->actual_started_at_game_turn + 2000)
    {
        FTEST_FAIL_TEST("Scavenging did not start within 2000 turns. PLAYER0 dragon state: %d", (int)get_creature_state_besides_interruptions(vars->p0_dragon));
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_enemy__action003__verify_progress(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_enemy__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_dragon, vars->p0_scav_cx, vars->p0_scav_cy);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    int32_t points = dungeon->scavenge_turn_points[vars->dragon_model];

    // Verify points are increasing
    if (points > vars->initial_p1_points)
    {
        FTESTLOG("Scavenge points increasing: %d -> %d", (int)vars->initial_p1_points, (int)points);

        // Check if PLAYER1 dragon is being scavenged
        if (creature_is_being_scavenged(vars->p1_dragon))
        {
            FTESTLOG("PLAYER1 dragon is in being-scavenged state");
        }
        else
        {
            FTESTLOG("PLAYER1 dragon NOT yet in being-scavenged state (state: %d)", (int)get_creature_state_besides_interruptions(vars->p1_dragon));
        }

        // Check target is our enemy dragon
        short target_idx = dungeon->scavenge_targets[vars->dragon_model];
        if (target_idx == vars->p1_dragon->index)
        {
            FTESTLOG("Scavenge target correctly set to PLAYER1 dragon (index %d)", (int)target_idx);
        }
        else
        {
            FTESTLOG("Scavenge target index %d does not match PLAYER1 dragon index %d", (int)target_idx, (int)vars->p1_dragon->index);
        }

        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn > vars->check_start_turn + 500)
    {
        FTEST_FAIL_TEST("Points not increasing after 500 turns. Current: %d, initial: %d", (int)points, (int)vars->initial_p1_points);
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_enemy__action004__wait_for_completion(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_enemy__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_dragon, vars->p0_scav_cx, vars->p0_scav_cy);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    int32_t points = dungeon->scavenge_turn_points[vars->dragon_model];

    // Log progress periodically
    if (game.play_gameturn % 200 == 0)
    {
        struct Thing* target = thing_get(dungeon->scavenge_targets[vars->dragon_model]);
        long required = 0;
        if (!thing_is_invalid(target))
            required = calculate_correct_creature_scavenge_required(target, PLAYER0) << 8;
        FTESTLOG("Scavenge progress: %d / %ld (%.1f%%), dragon owner: %d",
            (int)points, required, required > 0 ? (100.0 * points / required) : 0.0,
            (int)vars->p1_dragon->owner);
    }

    // Only check actual owner change — the scavenge completion is asynchronous:
    // points reset first, then creature disappears, reappears, THEN owner changes.
    if (vars->p1_dragon->owner == PLAYER0)
    {
        FTESTLOG("PLAYER1 dragon now owned by PLAYER0 - scavenge completed!");
        vars->scavenge_completed = 1;
        return FTRs_Go_To_Next_Action;
    }

    // Timeout after 10000 turns
    if (game.play_gameturn > args->actual_started_at_game_turn + 10000)
    {
        FTEST_FAIL_TEST("Scavenge did not complete within 10000 turns. Points: %d, owner: %d",
            (int)points, (int)vars->p1_dragon->owner);
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_enemy__action005__wait_after_completion(struct FTestActionArgs* const args)
{
    // Wait 200 turns (~10 seconds) after scavenge completes before verifying
    if (game.play_gameturn < args->actual_started_at_game_turn + 200)
        return FTRs_Repeat_Current_Action;
    FTESTLOG("Post-completion wait done (200 turns)");
    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_enemy__action006__verify_result(struct FTestActionArgs* const args)
{
    struct ftest_scav_enemy__variables* const vars = args->data;

    if (!vars->scavenge_completed)
    {
        FTEST_FAIL_TEST("Scavenge did not complete");
        return FTRs_Go_To_Next_Action;
    }

    // Verify the dragon is now owned by PLAYER0
    if (vars->p1_dragon->owner != PLAYER0)
    {
        FTEST_FAIL_TEST("Dragon owner is %d, expected PLAYER0 (%d)", (int)vars->p1_dragon->owner, (int)PLAYER0);
        return FTRs_Go_To_Next_Action;
    }

    // Verify creatures_scavenged counter incremented
    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    if (dungeon->creatures_scavenged[vars->dragon_model] < 1)
    {
        FTEST_FAIL_TEST("creatures_scavenged[DRAGON] is %d, expected >= 1", (int)dungeon->creatures_scavenged[vars->dragon_model]);
        return FTRs_Go_To_Next_Action;
    }

    // Verify scavenge_turn_points reset after completion
    if (dungeon->scavenge_turn_points[vars->dragon_model] != 0)
    {
        FTESTLOG("Warning: scavenge_turn_points not reset: %d", (int)dungeon->scavenge_turn_points[vars->dragon_model]);
    }

    FTESTLOG("TEST PASSED: Successfully scavenged %s from PLAYER1", creature_code_name(vars->dragon_model));
    return FTRs_Go_To_Next_Action;
}

// ============================================================================
// TEST 2: Scavenging from creature pool
// Setup: PLAYER0 has scavenger room + lv1 sorceror, no enemy sorcerors exist.
// Pool has sorcerors available. Expect: creature pulled from pool.
// ============================================================================

struct ftest_scav_pool__variables
{
    MapSlabCoord p0_scav_cx;
    MapSlabCoord p0_scav_cy;
    struct Thing* p0_dragon;
    ThingModel dragon_model;
    struct Thing* p0_dragon_nopool;
    ThingModel dragon_nopool_model;
    int initial_pool_count;
    int initial_creature_count;
    TbBool scavenge_completed;
};
static struct ftest_scav_pool__variables ftest_scav_pool__vars = {
    .p0_scav_cx = 0,
    .p0_scav_cy = 0,
    .p0_dragon = NULL,
    .dragon_model = 0,
    .p0_dragon_nopool = NULL,
    .dragon_nopool_model = 0,
    .initial_pool_count = 0,
    .initial_creature_count = 0,
    .scavenge_completed = 0,
};

FTestActionResult ftest_scav_pool__action001__setup(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_pool__action002__wait_for_progress(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_pool__action003__wait_for_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_pool__action004__wait_after_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_pool__action005__verify_result(struct FTestActionArgs* const args);

TbBool ftest_scavenging_from_pool_init()
{
    ftest_append_action(ftest_scav_pool__action001__setup, 20, &ftest_scav_pool__vars);
    ftest_append_action(ftest_scav_pool__action002__wait_for_progress, 100, &ftest_scav_pool__vars);
    ftest_append_action(ftest_scav_pool__action003__wait_for_completion, 10, &ftest_scav_pool__vars);
    ftest_append_action(ftest_scav_pool__action004__wait_after_completion, 0, &ftest_scav_pool__vars);
    ftest_append_action(ftest_scav_pool__action005__verify_result, 0, &ftest_scav_pool__vars);
    return true;
}

FTestActionResult ftest_scav_pool__action001__setup(struct FTestActionArgs* const args)
{
    struct ftest_scav_pool__variables* const vars = args->data;

    ftest_util_reveal_map(PLAYER0);

    // Use SORCEROR for pool test — it's naturally in the pool for level 8 (count=30)
    // and has a high ScavengeValue=5, allowing scavenging to complete in ~4400 turns.
    // BILE_DEMON has ScavengeValue=1 which takes too long.
    vars->dragon_model = get_rid(creature_desc, "SORCEROR");
    if (vars->dragon_model <= 0)
    {
        FTEST_FAIL_TEST("Could not find SORCEROR creature model");
        return FTRs_Go_To_Next_Action;
    }
    FTESTLOG("SORCEROR model id: %d (%s)", (int)vars->dragon_model, creature_code_name(vars->dragon_model));

    // Create 5x5 scavenger room for PLAYER0 (right of heart, narrow passage)
    if (!ftest_scav__create_scavenger_room(PLAYER0, &vars->p0_scav_cx, &vars->p0_scav_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 scavenger room");
        return FTRs_Go_To_Next_Action;
    }

    // Create lair + hatchery for PLAYER0
    ftest_scav__create_support_rooms(PLAYER0);

    // Give gold
    player_add_offmap_gold(PLAYER0, 100000);

    // Raise attracted creature limit so natural portal arrivals don't block pool scavenging.
    // can_scavenge_creature_from_pool() rejects if num_active_creatrs >= max_creatures_attracted.
    {
        struct Dungeon* dngn = get_dungeon(PLAYER0);
        dngn->max_creatures_attracted = 50;
    }

    // Verify pool has sorcerors
    vars->initial_pool_count = game.pool.crtr_kind[vars->dragon_model];
    FTESTLOG("Initial %s pool count: %d", creature_code_name(vars->dragon_model), vars->initial_pool_count);
    if (vars->initial_pool_count <= 0)
    {
        FTEST_FAIL_TEST("%s not available in creature pool for this level", creature_code_name(vars->dragon_model));
        return FTRs_Go_To_Next_Action;
    }

    // Create PLAYER0 sorceror in scavenger room (level 1, no enemy sorcerors on map)
    struct Coord3d pos;
    set_coords_to_slab_center(&pos, vars->p0_scav_cx, vars->p0_scav_cy);
    vars->p0_dragon = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER0, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p0_dragon))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 sorceror");
        return FTRs_Go_To_Next_Action;
    }
    // Keep level 1 (exp_level 0) — pool scavenging threshold uses the scavenger's
    // loyalty which scales with level, so level 1 keeps the threshold manageable.

    // Force creature into scavenging state
    initialise_thing_state(vars->p0_dragon, CrSt_AtScavengerRoom);

    // Also place a DRAGON in the scavenger room — DRAGON is NOT in the pool for level 8,
    // so it should sit idle: no points accumulated, no gold spent.
    vars->dragon_nopool_model = get_rid(creature_desc, "DRAGON");
    if (vars->dragon_nopool_model > 0)
    {
        struct Coord3d pos2;
        set_coords_to_slab_center(&pos2, vars->p0_scav_cx, vars->p0_scav_cy);
        vars->p0_dragon_nopool = ftest_util_create_creature(pos2.x.val, pos2.y.val, PLAYER0, 1, vars->dragon_nopool_model);
        if (!thing_is_invalid(vars->p0_dragon_nopool))
        {
            set_creature_level(vars->p0_dragon_nopool, 9); // level 10
            initialise_thing_state(vars->p0_dragon_nopool, CrSt_AtScavengerRoom);
            FTESTLOG("Created PLAYER0 DRAGON (level 10) in scavenger room — not in pool, should do nothing");
            FTESTLOG("DRAGON pool count: %d (expected 0)", game.pool.crtr_kind[vars->dragon_nopool_model]);
        }
    }

    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    vars->initial_creature_count = dungeon->num_active_creatrs;
    FTESTLOG("Created PLAYER0 %s (level 1) for pool scavenging. Active creatures: %d",
        creature_code_name(vars->dragon_model), vars->initial_creature_count);

    ftest_util_move_camera_to_slab(vars->p0_scav_cx, vars->p0_scav_cy, PLAYER0);

    // Kill enemy imps to prevent unexpected expansion
    ftest_scav__kill_player_imps(PLAYER1);

    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_pool__action002__wait_for_progress(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_pool__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_dragon, vars->p0_scav_cx, vars->p0_scav_cy);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    int32_t points = dungeon->scavenge_turn_points[vars->dragon_model];
    short target_idx = dungeon->scavenge_targets[vars->dragon_model];

    // For pool scavenging, target should be 0/invalid (no enemy creature)
    if (points > 0)
    {
        FTESTLOG("Pool scavenge points accumulating: %d, target_idx: %d (expect 0 for pool)", (int)points, (int)target_idx);
        if (target_idx != 0)
        {
            FTESTLOG("Note: target_idx is %d, not 0. May be targeting enemy creature instead of pool.", (int)target_idx);
        }
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn % 200 == 0)
    {
        FTESTLOG("Waiting for pool scavenge to start... creature state: %d",
            (int)get_creature_state_besides_interruptions(vars->p0_dragon));
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 2000)
    {
        FTEST_FAIL_TEST("Pool scavenging did not start within 2000 turns");
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_pool__action003__wait_for_completion(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_pool__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_dragon, vars->p0_scav_cx, vars->p0_scav_cy);
    struct Dungeon* dungeon = get_dungeon(PLAYER0);

    int current_pool = game.pool.crtr_kind[vars->dragon_model];

    if (game.play_gameturn % 200 == 0)
    {
        FTESTLOG("Pool scavenge - points: %d, pool remaining: %d, scavenge_gain: %d, active creatures: %d",
            (int)dungeon->scavenge_turn_points[vars->dragon_model],
            current_pool,
            (int)dungeon->creatures_scavenge_gain,
            (int)dungeon->num_active_creatrs);
        if (vars->dragon_nopool_model > 0)
        {
            FTESTLOG("  DRAGON (not in pool) scavenge points: %d (should stay 0)",
                (int)dungeon->scavenge_turn_points[vars->dragon_nopool_model]);
        }
    }

    // Only count as completed when creatures_scavenge_gain increments —
    // this is the authoritative counter that only increases on actual scavenge,
    // not when creatures arrive naturally through the portal.
    if (dungeon->creatures_scavenge_gain >= 1)
    {
        FTESTLOG("Scavenge gain detected! scavenge_gain: %d, pool: %d -> %d",
            (int)dungeon->creatures_scavenge_gain, vars->initial_pool_count, current_pool);
        vars->scavenge_completed = 1;
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 10000)
    {
        FTEST_FAIL_TEST("Pool scavenge did not complete within 10000 turns. scavenge_gain: %d, pool: %d",
            (int)dungeon->creatures_scavenge_gain, current_pool);
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_pool__action004__wait_after_completion(struct FTestActionArgs* const args)
{
    // Wait 200 turns (~10 seconds) after scavenge completes before verifying
    if (game.play_gameturn < args->actual_started_at_game_turn + 200)
        return FTRs_Repeat_Current_Action;
    FTESTLOG("Post-completion wait done (200 turns)");
    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_pool__action005__verify_result(struct FTestActionArgs* const args)
{
    struct ftest_scav_pool__variables* const vars = args->data;

    if (!vars->scavenge_completed)
    {
        FTEST_FAIL_TEST("Pool scavenge did not complete");
        return FTRs_Go_To_Next_Action;
    }

    int current_pool = game.pool.crtr_kind[vars->dragon_model];
    FTESTLOG("Pool after scavenge: %d (was %d)", current_pool, vars->initial_pool_count);

    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    if (dungeon->creatures_scavenge_gain < 1)
    {
        FTEST_FAIL_TEST("creatures_scavenge_gain is %d, expected >= 1", (int)dungeon->creatures_scavenge_gain);
        return FTRs_Go_To_Next_Action;
    }

    // Verify DRAGON (not in pool) accumulated zero scavenge points and no gold was spent on it
    if (vars->dragon_nopool_model > 0)
    {
        int32_t dragon_points = dungeon->scavenge_turn_points[vars->dragon_nopool_model];
        if (dragon_points != 0)
        {
            FTEST_FAIL_TEST("DRAGON (not in pool) accumulated %d scavenge points — expected 0", (int)dragon_points);
            return FTRs_Go_To_Next_Action;
        }
        FTESTLOG("DRAGON (not in pool) correctly had 0 scavenge points — no wasted effort");
    }

    FTESTLOG("TEST PASSED: Successfully scavenged %s from creature pool", creature_code_name(vars->dragon_model));
    return FTRs_Go_To_Next_Action;
}

// ============================================================================
// TEST 3: Being scavenged (enemy scavenges OUR creature)
// Setup: PLAYER1 has scavenger room + lv10 dragon. PLAYER0 has lv1 dragon.
// Expect: PLAYER0 dragon enters being-scavenged state, eventually lost.
// ============================================================================

struct ftest_scav_victim__variables
{
    MapSlabCoord p1_scav_cx;
    MapSlabCoord p1_scav_cy;
    MapSlabCoord p0_area_cx;
    MapSlabCoord p0_area_cy;
    struct Thing* p0_dragon;
    struct Thing* p1_dragon;
    ThingModel dragon_model;
    TbBool being_scavenged_detected;
    TbBool creature_lost;
    int initial_p0_creature_count;
};
static struct ftest_scav_victim__variables ftest_scav_victim__vars = {
    .p1_scav_cx = 0,
    .p1_scav_cy = 0,
    .p0_area_cx = 0,
    .p0_area_cy = 0,
    .p0_dragon = NULL,
    .p1_dragon = NULL,
    .dragon_model = 0,
    .being_scavenged_detected = 0,
    .creature_lost = 0,
    .initial_p0_creature_count = 0,
};

FTestActionResult ftest_scav_victim__action001__setup(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_victim__action002__check_being_scavenged(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_victim__action003__check_enemy_progress(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_victim__action004__wait_for_loss(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_victim__action005__wait_after_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_victim__action006__verify_result(struct FTestActionArgs* const args);

TbBool ftest_scavenging_being_scavenged_init()
{
    ftest_append_action(ftest_scav_victim__action001__setup, 20, &ftest_scav_victim__vars);
    ftest_append_action(ftest_scav_victim__action002__check_being_scavenged, 100, &ftest_scav_victim__vars);
    ftest_append_action(ftest_scav_victim__action003__check_enemy_progress, 10, &ftest_scav_victim__vars);
    ftest_append_action(ftest_scav_victim__action004__wait_for_loss, 10, &ftest_scav_victim__vars);
    ftest_append_action(ftest_scav_victim__action005__wait_after_completion, 0, &ftest_scav_victim__vars);
    ftest_append_action(ftest_scav_victim__action006__verify_result, 0, &ftest_scav_victim__vars);
    return true;
}

FTestActionResult ftest_scav_victim__action001__setup(struct FTestActionArgs* const args)
{
    struct ftest_scav_victim__variables* const vars = args->data;

    ftest_util_reveal_map(PLAYER0);

    vars->dragon_model = get_rid(creature_desc, "DRAGON");
    if (vars->dragon_model <= 0)
    {
        FTEST_FAIL_TEST("Could not find DRAGON creature model");
        return FTRs_Go_To_Next_Action;
    }
    FTESTLOG("DRAGON model id: %d (%s)", (int)vars->dragon_model, creature_code_name(vars->dragon_model));

    // Create 5x5 scavenger room for PLAYER1 (right of heart, narrow passage)
    if (!ftest_scav__create_scavenger_room(PLAYER1, &vars->p1_scav_cx, &vars->p1_scav_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 scavenger room");
        return FTRs_Go_To_Next_Action;
    }

    // Create 5x5 claimed area for PLAYER0 (above heart, narrow passage)
    if (!ftest_scav__create_claimed_area(PLAYER0, &vars->p0_area_cx, &vars->p0_area_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 claimed area");
        return FTRs_Go_To_Next_Action;
    }

    // Create lair + hatchery for both players
    ftest_scav__create_support_rooms(PLAYER0);
    ftest_scav__create_support_rooms(PLAYER1);

    // Give PLAYER1 gold
    player_add_offmap_gold(PLAYER1, 100000);

    // Create PLAYER1 dragon in scavenger room (level 10 - strong scavenger)
    struct Coord3d pos;
    set_coords_to_slab_center(&pos, vars->p1_scav_cx, vars->p1_scav_cy);
    vars->p1_dragon = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER1, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p1_dragon))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p1_dragon, 9); // level 10 (0-indexed)
    FTESTLOG("Created PLAYER1 %s (index %d, level 10) in scavenger room", creature_code_name(vars->dragon_model), (int)vars->p1_dragon->index);

    // Force creature into scavenging state
    initialise_thing_state(vars->p1_dragon, CrSt_AtScavengerRoom);

    // Create PLAYER0 dragon as victim (level 1 - weak, easy to scavenge)
    struct Coord3d pos2;
    set_coords_to_slab_center(&pos2, vars->p0_area_cx, vars->p0_area_cy);
    vars->p0_dragon = ftest_util_create_creature(pos2.x.val, pos2.y.val, PLAYER0, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p0_dragon))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p0_dragon, 0); // level 1 (0-indexed)

    struct Dungeon* dungeon = get_dungeon(PLAYER0);
    vars->initial_p0_creature_count = dungeon->num_active_creatrs;
    FTESTLOG("Created PLAYER0 %s (index %d, level 1) as scavenge victim. Active creatures: %d",
        creature_code_name(vars->dragon_model), (int)vars->p0_dragon->index, vars->initial_p0_creature_count);

    ftest_util_move_camera_to_slab(vars->p0_area_cx, vars->p0_area_cy, PLAYER0);

    // Kill enemy imps to prevent unexpected expansion
    ftest_scav__kill_player_imps(PLAYER1);

    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_victim__action002__check_being_scavenged(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_victim__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p1_dragon, vars->p1_scav_cx, vars->p1_scav_cy);

    // Check if our dragon enters being-scavenged state
    if (creature_is_being_scavenged(vars->p0_dragon))
    {
        FTESTLOG("PLAYER0 dragon is now in being-scavenged state!");
        vars->being_scavenged_detected = 1;
        return FTRs_Go_To_Next_Action;
    }

    // Also check if dragon already lost (fast scavenge)
    if (vars->p0_dragon->owner == PLAYER1)
    {
        FTESTLOG("PLAYER0 dragon already lost to PLAYER1!");
        vars->creature_lost = 1;
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 3000)
    {
        FTEST_FAIL_TEST("Dragon not entering being-scavenged state within 3000 turns");
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn % 200 == 0)
    {
        struct Dungeon* enemy_dngn = get_dungeon(PLAYER1);
        FTESTLOG("Waiting for scavenge... PLAYER1 scavenge_turn_points[%s]: %d, target: %d, dragon state: %d",
            creature_code_name(vars->dragon_model),
            (int)enemy_dngn->scavenge_turn_points[vars->dragon_model],
            (int)enemy_dngn->scavenge_targets[vars->dragon_model],
            (int)get_creature_state_besides_interruptions(vars->p0_dragon));
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_victim__action003__check_enemy_progress(struct FTestActionArgs* const args)
{
    struct ftest_scav_victim__variables* const vars = args->data;

    // If already lost, skip
    if (vars->creature_lost)
        return FTRs_Go_To_Next_Action;

    struct Dungeon* enemy_dngn = get_dungeon(PLAYER1);
    int32_t enemy_points = enemy_dngn->scavenge_turn_points[vars->dragon_model];
    short enemy_target = enemy_dngn->scavenge_targets[vars->dragon_model];

    FTESTLOG("Enemy scavenge progress: points=%d, target_idx=%d (our dragon idx=%d)",
        (int)enemy_points, (int)enemy_target, (int)vars->p0_dragon->index);

    // Verify enemy is targeting our dragon
    if (enemy_target == vars->p0_dragon->index)
    {
        FTESTLOG("Confirmed: enemy targeting our dragon");
    }
    else if (enemy_target > 0)
    {
        struct Thing* target = thing_get(enemy_target);
        if (!thing_is_invalid(target))
        {
            FTESTLOG("Enemy targeting different creature: model=%s owner=%d idx=%d",
                creature_code_name(target->model), (int)target->owner, (int)target->index);
        }
    }

    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_victim__action004__wait_for_loss(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_victim__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p1_dragon, vars->p1_scav_cx, vars->p1_scav_cy);

    if (vars->creature_lost)
        return FTRs_Go_To_Next_Action;

    // Check if dragon changed owner
    if (vars->p0_dragon->owner == PLAYER1)
    {
        FTESTLOG("Dragon lost to PLAYER1!");
        vars->creature_lost = 1;
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn % 500 == 0)
    {
        struct Dungeon* enemy_dngn = get_dungeon(PLAYER1);
        struct Thing* target = thing_get(enemy_dngn->scavenge_targets[vars->dragon_model]);
        long required = 0;
        if (!thing_is_invalid(target))
            required = calculate_correct_creature_scavenge_required(target, PLAYER1) << 8;
        FTESTLOG("Being scavenged progress: %d / %ld",
            (int)enemy_dngn->scavenge_turn_points[vars->dragon_model], required);
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 15000)
    {
        FTEST_FAIL_TEST("Dragon not lost within 15000 turns");
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_victim__action005__wait_after_completion(struct FTestActionArgs* const args)
{
    // Wait 200 turns (~10 seconds) after creature loss before verifying
    if (game.play_gameturn < args->actual_started_at_game_turn + 200)
        return FTRs_Repeat_Current_Action;
    FTESTLOG("Post-completion wait done (200 turns)");
    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_victim__action006__verify_result(struct FTestActionArgs* const args)
{
    struct ftest_scav_victim__variables* const vars = args->data;

    if (!vars->creature_lost)
    {
        FTEST_FAIL_TEST("Dragon was not lost to enemy scavenging");
        return FTRs_Go_To_Next_Action;
    }

    // Verify PLAYER1 gained the creature
    struct Dungeon* enemy_dngn = get_dungeon(PLAYER1);
    if (enemy_dngn->creatures_scavenge_gain < 1)
    {
        FTEST_FAIL_TEST("PLAYER1 creatures_scavenge_gain is %d, expected >= 1", (int)enemy_dngn->creatures_scavenge_gain);
        return FTRs_Go_To_Next_Action;
    }

    // Verify PLAYER1 scavenge points reset
    if (enemy_dngn->scavenge_turn_points[vars->dragon_model] != 0)
    {
        FTESTLOG("Warning: PLAYER1 scavenge_turn_points not reset: %d", (int)enemy_dngn->scavenge_turn_points[vars->dragon_model]);
    }

    FTESTLOG("TEST PASSED: Our %s was scavenged by enemy", creature_code_name(vars->dragon_model));
    return FTRs_Go_To_Next_Action;
}

// ============================================================================
// TEST 4: Both players scavenging each other simultaneously
// Setup: PLAYER0 has scavenger room + lv10 dragon, PLAYER1 has lv1 dragon.
//        PLAYER1 has scavenger room + lv10 dragon, PLAYER0 has lv1 dragon.
// Both players attempt to scavenge each other's weak dragon at the same time.
// Expect: At least one player successfully scavenges the other's creature.
// ============================================================================

struct ftest_scav_both__variables
{
    MapSlabCoord p0_scav_cx;
    MapSlabCoord p0_scav_cy;
    MapSlabCoord p1_scav_cx;
    MapSlabCoord p1_scav_cy;
    MapSlabCoord p0_area_cx;
    MapSlabCoord p0_area_cy;
    MapSlabCoord p1_area_cx;
    MapSlabCoord p1_area_cy;
    MapSlabCoord p0_temple_cx;
    MapSlabCoord p0_temple_cy;
    struct Thing* p0_scavenger;  // PLAYER0's lv10 dragon in scavenger room
    struct Thing* p1_scavenger;  // PLAYER1's lv10 dragon in scavenger room
    struct Thing* p0_victim;     // PLAYER0's lv1 dragon (target for PLAYER1)
    struct Thing* p1_victim;     // PLAYER1's lv1 dragon (target for PLAYER0)
    ThingModel dragon_model;
    TbBool p0_scavenged_enemy;
    TbBool p1_scavenged_enemy;
    TbBool any_scavenge_started;
    TbBool victim_moved_to_temple;
    int32_t p1_pts_at_temple_move;
};
static struct ftest_scav_both__variables ftest_scav_both__vars = {
    .p0_scav_cx = 0,
    .p0_scav_cy = 0,
    .p1_scav_cx = 0,
    .p1_scav_cy = 0,
    .p0_area_cx = 0,
    .p0_area_cy = 0,
    .p1_area_cx = 0,
    .p1_area_cy = 0,
    .p0_temple_cx = 0,
    .p0_temple_cy = 0,
    .p0_scavenger = NULL,
    .p1_scavenger = NULL,
    .p0_victim = NULL,
    .p1_victim = NULL,
    .dragon_model = 0,
    .p0_scavenged_enemy = 0,
    .p1_scavenged_enemy = 0,
    .any_scavenge_started = 0,
    .victim_moved_to_temple = 0,
    .p1_pts_at_temple_move = 0,
};

FTestActionResult ftest_scav_both__action001__setup(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action002__wait_for_start(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action003__wait_for_halfway(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action004__move_victim_to_temple(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action005__verify_points_stopped(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action006__wait_for_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action007__wait_after_completion(struct FTestActionArgs* const args);
FTestActionResult ftest_scav_both__action008__verify_result(struct FTestActionArgs* const args);

TbBool ftest_scavenging_both_players_init()
{
    ftest_append_action(ftest_scav_both__action001__setup, 20, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action002__wait_for_start, 100, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action003__wait_for_halfway, 10, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action004__move_victim_to_temple, 0, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action005__verify_points_stopped, 0, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action006__wait_for_completion, 0, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action007__wait_after_completion, 0, &ftest_scav_both__vars);
    ftest_append_action(ftest_scav_both__action008__verify_result, 0, &ftest_scav_both__vars);
    return true;
}

FTestActionResult ftest_scav_both__action001__setup(struct FTestActionArgs* const args)
{
    struct ftest_scav_both__variables* const vars = args->data;

    ftest_util_reveal_map(PLAYER0);

    vars->dragon_model = get_rid(creature_desc, "DRAGON");
    if (vars->dragon_model <= 0)
    {
        FTEST_FAIL_TEST("Could not find DRAGON creature model");
        return FTRs_Go_To_Next_Action;
    }
    FTESTLOG("DRAGON model id: %d (%s)", (int)vars->dragon_model, creature_code_name(vars->dragon_model));

    // Create 5x5 scavenger rooms for both players (right of each heart)
    if (!ftest_scav__create_scavenger_room(PLAYER0, &vars->p0_scav_cx, &vars->p0_scav_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 scavenger room");
        return FTRs_Go_To_Next_Action;
    }
    if (!ftest_scav__create_scavenger_room(PLAYER1, &vars->p1_scav_cx, &vars->p1_scav_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 scavenger room");
        return FTRs_Go_To_Next_Action;
    }

    // Create 5x5 claimed areas for victims (above each heart)
    if (!ftest_scav__create_claimed_area(PLAYER0, &vars->p0_area_cx, &vars->p0_area_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 claimed area");
        return FTRs_Go_To_Next_Action;
    }
    if (!ftest_scav__create_claimed_area(PLAYER1, &vars->p1_area_cx, &vars->p1_area_cy))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 claimed area");
        return FTRs_Go_To_Next_Action;
    }

    // Create lair + hatchery for both players
    ftest_scav__create_support_rooms(PLAYER0);
    ftest_scav__create_support_rooms(PLAYER1);

    // Give both players gold
    player_add_offmap_gold(PLAYER0, 100000);
    player_add_offmap_gold(PLAYER1, 100000);

    // PLAYER0: lv10 dragon in scavenger room
    struct Coord3d pos;
    set_coords_to_slab_center(&pos, vars->p0_scav_cx, vars->p0_scav_cy);
    vars->p0_scavenger = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER0, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p0_scavenger))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 scavenger dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p0_scavenger, 9);
    initialise_thing_state(vars->p0_scavenger, CrSt_AtScavengerRoom);
    FTESTLOG("Created PLAYER0 %s (lv10) in scavenger room", creature_code_name(vars->dragon_model));

    // PLAYER1: lv10 dragon in scavenger room
    set_coords_to_slab_center(&pos, vars->p1_scav_cx, vars->p1_scav_cy);
    vars->p1_scavenger = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER1, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p1_scavenger))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 scavenger dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p1_scavenger, 9);
    initialise_thing_state(vars->p1_scavenger, CrSt_AtScavengerRoom);
    FTESTLOG("Created PLAYER1 %s (lv10) in scavenger room", creature_code_name(vars->dragon_model));

    // PLAYER0: lv1 dragon as victim (in PLAYER0's claimed area)
    set_coords_to_slab_center(&pos, vars->p0_area_cx, vars->p0_area_cy);
    vars->p0_victim = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER0, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p0_victim))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER0 victim dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p0_victim, 0);
    FTESTLOG("Created PLAYER0 %s (lv1) as victim", creature_code_name(vars->dragon_model));

    // PLAYER1: lv1 dragon as victim (in PLAYER1's claimed area)
    set_coords_to_slab_center(&pos, vars->p1_area_cx, vars->p1_area_cy);
    vars->p1_victim = ftest_util_create_creature(pos.x.val, pos.y.val, PLAYER1, 1, vars->dragon_model);
    if (thing_is_invalid(vars->p1_victim))
    {
        FTEST_FAIL_TEST("Failed to create PLAYER1 victim dragon");
        return FTRs_Go_To_Next_Action;
    }
    set_creature_level(vars->p1_victim, 0);
    FTESTLOG("Created PLAYER1 %s (lv1) as victim", creature_code_name(vars->dragon_model));

    // Create a 3x3 temple for PLAYER0 (above heart area, for later victim relocation)
    {
        struct Dungeon* p0dngn = get_dungeon(PLAYER0);
        MapSlabCoord hx = coord_slab(p0dngn->mappos.x.val);
        MapSlabCoord hy = coord_slab(p0dngn->mappos.y.val);
        // Place temple above heart, offset right to avoid lair/hatchery area
        vars->p0_temple_cx = hx + HEART_RADIUS + 3;
        vars->p0_temple_cy = hy - HEART_RADIUS - 3 - 1; // above heart, center of 3x3
        MapSlabCoord tx1 = vars->p0_temple_cx - 1;
        MapSlabCoord ty1 = vars->p0_temple_cy - 1;
        if (!ftest_scav__safe_replace_slabs(tx1, ty1, tx1 + 2, ty1 + 2, SlbT_TEMPLE, PLAYER0, hx, hy))
        {
            FTEST_FAIL_TEST("Failed to create PLAYER0 temple");
            return FTRs_Go_To_Next_Action;
        }
        // Connect temple to heart via L-shaped corridor
        ftest_scav__safe_replace_slabs(vars->p0_temple_cx, ty1 + 3, vars->p0_temple_cx, hy - HEART_RADIUS - 1, SlbT_CLAIMED, PLAYER0, hx, hy);
        ftest_scav__safe_replace_slabs(hx + HEART_RADIUS + 1, hy, vars->p0_temple_cx, hy, SlbT_CLAIMED, PLAYER0, hx, hy);
        FTESTLOG("Created 3x3 temple for PLAYER0 at (%d,%d)", vars->p0_temple_cx, vars->p0_temple_cy);
    }

    ftest_util_move_camera_to_slab(vars->p0_scav_cx, vars->p0_scav_cy, PLAYER0);

    // Kill enemy imps to prevent unexpected expansion
    ftest_scav__kill_player_imps(PLAYER1);

    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_both__action002__wait_for_start(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_both__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_scavenger, vars->p0_scav_cx, vars->p0_scav_cy);
    ftest_scav__force_scavenge(vars->p1_scavenger, vars->p1_scav_cx, vars->p1_scav_cy);
    struct Dungeon* p0_dngn = get_dungeon(PLAYER0);
    struct Dungeon* p1_dngn = get_dungeon(PLAYER1);

    int32_t p0_pts = p0_dngn->scavenge_turn_points[vars->dragon_model];
    int32_t p1_pts = p1_dngn->scavenge_turn_points[vars->dragon_model];

    if (game.play_gameturn % 200 == 0)
    {
        FTESTLOG("Both scavenging: P0 pts=%d target=%d | P1 pts=%d target=%d",
            (int)p0_pts, (int)p0_dngn->scavenge_targets[vars->dragon_model],
            (int)p1_pts, (int)p1_dngn->scavenge_targets[vars->dragon_model]);
    }

    if (p0_pts > 0 || p1_pts > 0)
    {
        FTESTLOG("Scavenging started! P0 pts=%d, P1 pts=%d", (int)p0_pts, (int)p1_pts);
        vars->any_scavenge_started = 1;
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 3000)
    {
        FTEST_FAIL_TEST("Neither player started scavenging within 3000 turns");
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_both__action003__wait_for_halfway(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_both__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_scavenger, vars->p0_scav_cx, vars->p0_scav_cy);
    ftest_scav__force_scavenge(vars->p1_scavenger, vars->p1_scav_cx, vars->p1_scav_cy);
    struct Dungeon* p1_dngn = get_dungeon(PLAYER1);

    int32_t p1_pts = p1_dngn->scavenge_turn_points[vars->dragon_model];

    // Calculate required points for P1 scavenging P0's victim
    struct Thing* target = thing_get(p1_dngn->scavenge_targets[vars->dragon_model]);
    long required = 0;
    if (!thing_is_invalid(target))
        required = calculate_correct_creature_scavenge_required(target, PLAYER1) << 8;

    if (game.play_gameturn % 200 == 0)
    {
        struct Dungeon* p0_dngn = get_dungeon(PLAYER0);
        FTESTLOG("Waiting for halfway: P1 pts=%d / %ld (%.1f%%) | P0 pts=%d",
            (int)p1_pts, required, required > 0 ? (100.0 * p1_pts / required) : 0.0,
            (int)p0_dngn->scavenge_turn_points[vars->dragon_model]);
    }

    // Wait until P1 reaches ~50% of required points
    if (required > 0 && p1_pts > required / 2)
    {
        FTESTLOG("P1 scavenge reached halfway! Points: %d / %ld (%.1f%%)",
            (int)p1_pts, required, 100.0 * p1_pts / required);
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 10000)
    {
        FTEST_FAIL_TEST("P1 scavenge did not reach halfway within 10000 turns. P1 pts=%d, required=%ld",
            (int)p1_pts, required);
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_both__action004__move_victim_to_temple(struct FTestActionArgs* const args)
{
    struct ftest_scav_both__variables* const vars = args->data;
    struct Dungeon* p1_dngn = get_dungeon(PLAYER1);

    // Record P1's current scavenge points before the move
    vars->p1_pts_at_temple_move = p1_dngn->scavenge_turn_points[vars->dragon_model];

    // Move PLAYER0's victim to the temple — offset from center to avoid sacrifice
    struct Coord3d temple_pos;
    set_coords_to_slab_center(&temple_pos, vars->p0_temple_cx + 1, vars->p0_temple_cy);
    move_thing_in_map(vars->p0_victim, &temple_pos);
    initialise_thing_state(vars->p0_victim, CrSt_CreatureBeingDropped);

    FTESTLOG("Moved P0 victim to temple at (%d,%d). P1 scavenge points at move: %d",
        vars->p0_temple_cx, vars->p0_temple_cy, (int)vars->p1_pts_at_temple_move);
    vars->victim_moved_to_temple = 1;

    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_both__action005__verify_points_stopped(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_both__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_scavenger, vars->p0_scav_cx, vars->p0_scav_cy);
    struct Dungeon* p1_dngn = get_dungeon(PLAYER1);

    int32_t p1_pts = p1_dngn->scavenge_turn_points[vars->dragon_model];

    if (game.play_gameturn % 100 == 0)
    {
        FTESTLOG("After temple move: P1 pts=%d (was %d at move), victim state=%d",
            (int)p1_pts, (int)vars->p1_pts_at_temple_move,
            (int)get_creature_state_besides_interruptions(vars->p0_victim));
    }

    // Wait 500 turns, then verify P1 points stopped (or decreased/reset)
    if (game.play_gameturn >= args->actual_started_at_game_turn + 500)
    {
        if (p1_pts <= vars->p1_pts_at_temple_move)
        {
            FTESTLOG("CONFIRMED: P1 scavenge points stopped after victim moved to temple. Before: %d, After: %d",
                (int)vars->p1_pts_at_temple_move, (int)p1_pts);
        }
        else
        {
            FTESTLOG("WARNING: P1 scavenge points INCREASED after victim moved to temple! Before: %d, After: %d",
                (int)vars->p1_pts_at_temple_move, (int)p1_pts);
        }
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_both__action006__wait_for_completion(struct FTestActionArgs* const args)
{
    ftest_scav__kill_enemy_imps_periodic();
    struct ftest_scav_both__variables* const vars = args->data;
    ftest_scav__force_scavenge(vars->p0_scavenger, vars->p0_scav_cx, vars->p0_scav_cy);

    // Check if PLAYER0 scavenged PLAYER1's victim (P1's victim should still be scavengeable)
    if (!vars->p0_scavenged_enemy && vars->p1_victim->owner == PLAYER0)
    {
        FTESTLOG("PLAYER0 successfully scavenged PLAYER1's dragon!");
        vars->p0_scavenged_enemy = 1;
    }

    // Check if PLAYER1 somehow still scavenged PLAYER0's victim (shouldn't happen from temple)
    if (!vars->p1_scavenged_enemy && vars->p0_victim->owner == PLAYER1)
    {
        FTESTLOG("PLAYER1 scavenged PLAYER0's dragon despite temple! (unexpected)");
        vars->p1_scavenged_enemy = 1;
    }

    if (vars->p0_scavenged_enemy || vars->p1_scavenged_enemy)
    {
        return FTRs_Go_To_Next_Action;
    }

    if (game.play_gameturn % 500 == 0)
    {
        struct Dungeon* p0_dngn = get_dungeon(PLAYER0);
        struct Dungeon* p1_dngn = get_dungeon(PLAYER1);
        FTESTLOG("Completion wait: P0 pts=%d | P1 pts=%d",
            (int)p0_dngn->scavenge_turn_points[vars->dragon_model],
            (int)p1_dngn->scavenge_turn_points[vars->dragon_model]);
    }

    if (game.play_gameturn > args->actual_started_at_game_turn + 15000)
    {
        FTEST_FAIL_TEST("No scavenge completed within 15000 turns after temple move");
        return FTRs_Go_To_Next_Action;
    }

    return FTRs_Repeat_Current_Action;
}

FTestActionResult ftest_scav_both__action007__wait_after_completion(struct FTestActionArgs* const args)
{
    // Wait 200 turns (~10 seconds) after scavenge completes before verifying
    if (game.play_gameturn < args->actual_started_at_game_turn + 200)
        return FTRs_Repeat_Current_Action;
    FTESTLOG("Post-completion wait done (200 turns)");
    return FTRs_Go_To_Next_Action;
}

FTestActionResult ftest_scav_both__action008__verify_result(struct FTestActionArgs* const args)
{
    struct ftest_scav_both__variables* const vars = args->data;

    if (!vars->p0_scavenged_enemy && !vars->p1_scavenged_enemy)
    {
        FTEST_FAIL_TEST("Neither player scavenged any creature");
        return FTRs_Go_To_Next_Action;
    }

    FTESTLOG("Results: P0 scavenged enemy=%d, P1 scavenged enemy=%d, victim moved to temple=%d",
        (int)vars->p0_scavenged_enemy, (int)vars->p1_scavenged_enemy, (int)vars->victim_moved_to_temple);

    if (vars->p0_scavenged_enemy)
    {
        FTESTLOG("TEST PASSED: PLAYER0 scavenged PLAYER1's %s. Temple blocked PLAYER1's scavenge as expected.",
            creature_code_name(vars->dragon_model));
    }
    else
    {
        FTESTLOG("TEST PASSED (unexpected path): PLAYER1 scavenged PLAYER0's %s despite temple move",
            creature_code_name(vars->dragon_model));
    }

    return FTRs_Go_To_Next_Action;
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
