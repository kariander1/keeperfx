#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char TbBool;

TbBool ftest_scavenging_from_enemy_init();
TbBool ftest_scavenging_from_pool_init();
TbBool ftest_scavenging_being_scavenged_init();
TbBool ftest_scavenging_both_players_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
