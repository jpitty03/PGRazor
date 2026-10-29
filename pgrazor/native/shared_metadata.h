#pragma once
#include "runtime_metadata.h"
#include <stddef.h>
#include <stdint.h>

// Feature 56 / API 2. Complete compiled port of diagnostics/quick-close.js prepare(module,api,domain).
//
// Compiles the full required core (7 classes, 6 fields, 7 methods, CatchSuccess enum literal value,
// boxed System.Boolean size/align ABI check, and pointerHandleAbi GC-handle code fingerprint) plus
// the 127 optional classes and all 21 optional subfeature descriptor bodies (catchCanvas, catchDiscovery,
// catchScene, details, directPresentation, transfer, spin, spinCamera, items, itemsLookup, rocket,
// battles, berries, incubator, buddy, gifts, spawn, vgp, vgpMarker, mapPoi, mapPoiExtra) directly in
// typed C++ with zero DELEGATED slots and no JS/VM interpreter.

namespace pgr_shared_meta {

static_assert(sizeof(void *) == 8, "shared_metadata native ABI requires LP64");

struct ClassSpec { const char *key, *name_space, *name, *canonical; };
struct FieldSpec { uint32_t owner; const char *name; uint64_t offset; uint32_t kind; const char *type_name; uint32_t value_size; };
struct MethodSpec {
  const char *key; uint32_t owner; const char *name, *return_type;
  uint32_t param_count; const char *param0, *param1;
  uint32_t is_static; uint64_t code_rva; const unsigned char *prologue; uint32_t prologue_len;
};
struct OptionalSpec { uint32_t id; const char *key; };

// ---------------------------------------------------------------------------------------------------
// Required-core descriptor tables (7 classes, 6 fields, 7 methods).
// ---------------------------------------------------------------------------------------------------
inline constexpr ClassSpec kClasses[] = {
  {"GUI",    "Niantic.Holoholo.NUI",       "GuiController",                     "Niantic.Holoholo.NUI.GuiController"},                       // 0
  {"SGUI",   "Niantic.Holoholo.Encounter", "EncounterSummaryGuiController",     "Niantic.Holoholo.Encounter.EncounterSummaryGuiController"}, // 1
  {"SSTATE", "Niantic.Holoholo.Encounter", "EncounterSummaryState",             "Niantic.Holoholo.Encounter.EncounterSummaryState"},        // 2
  {"RESP",   "Holoholo.Rpc",               "CatchPokemonOutProto",              "Holoholo.Rpc.CatchPokemonOutProto"},                        // 3
  {"OBJ",    "UnityEngine",                "Object",                            "UnityEngine.Object"},                                       // 4
  {"SYNC",   "UnityEngine",                "UnitySynchronizationContext",       "UnityEngine.UnitySynchronizationContext"},                  // 5
  {"BOOL",   "System",                     "Boolean",                           "System.Boolean"},                                           // 6
};
inline constexpr size_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);

inline constexpr FieldSpec kFields[] = {
  {3u, "CapturedPokemonId", 0x20ULL, PGR_MD_FIELD_VALUE,    "System.UInt64", 8u}, // 0  RESP.CapturedPokemonId
  {0u, "currentState",      0x70ULL, PGR_MD_FIELD_ENUM_I32, nullptr,         4u}, // 1  GUI.currentState
  {1u, "OnDismissed",       0x2a8ULL,PGR_MD_FIELD_REF,      nullptr,         0u}, // 2  SGUI.OnDismissed
  {2u, "dscx",              0xc0ULL, PGR_MD_FIELD_REF,      nullptr,         0u}, // 3  SSTATE.dscx
  {2u, "dscy",              0xc8ULL, PGR_MD_FIELD_REF,      nullptr,         0u}, // 4  SSTATE.dscy
  {3u, "Status",            0x10ULL, PGR_MD_FIELD_ENUM_I32, nullptr,         4u}, // 5  RESP.Status
};
inline constexpr size_t kFieldCount = sizeof(kFields) / sizeof(kFields[0]);
inline constexpr uint32_t kStatusFieldIndex = 5u;

inline constexpr unsigned char kExitState[] = {
  0xfe,0x67,0xbc,0xa9,0xf8,0x5f,0x01,0xa9,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9,
  0xd4,0x1c,0x02,0x90,0xf3,0x03,0x00,0xaa,0x88,0x6e,0x5f,0x39,0x28,0x04,0x00,0x37};
inline constexpr unsigned char kDismiss[] = {
  0xfe,0x0f,0x1d,0xf8,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,0xb4,0x6a,0x02,0xf0,
  0xf3,0x03,0x00,0xaa,0x88,0xea,0x51,0x39,0x68,0x02,0x00,0x36,0xb4,0x6a,0x02,0xf0};
inline constexpr unsigned char kInitialize[] = {
  0xfe,0x6f,0xbb,0xa9,0xfa,0x67,0x01,0xa9,0xf8,0x5f,0x02,0xa9,0xf6,0x57,0x03,0xa9,
  0xf4,0x4f,0x04,0xa9,0xd5,0x1c,0x02,0x90,0xf4,0x03,0x01,0xaa,0xf3,0x03,0x00,0xaa};
inline constexpr unsigned char kSetScores[] = {
  0xff,0x43,0x05,0xd1,0xef,0x3b,0x0b,0x6d,0xed,0x33,0x0c,0x6d,0xeb,0x2b,0x0d,0x6d,
  0xe9,0x23,0x0e,0x6d,0xfd,0x7b,0x0f,0xa9,0xfc,0x6f,0x10,0xa9,0xfa,0x67,0x11,0xa9};
inline constexpr unsigned char kHandleOpenComplete[] = {
  0xfe,0x67,0xbc,0xa9,0xf8,0x5f,0x01,0xa9,0xf6,0x57,0x02,0xa9,0xf4,0x4f,0x03,0xa9,
  0x14,0x1d,0x02,0xf0,0xf3,0x03,0x00,0xaa,0x88,0x36,0x61,0x39,0x08,0x01,0x00,0x37};
inline constexpr unsigned char kOpImplicit[] = {
  0xfe,0x0f,0x1d,0xf8,0xf6,0x57,0x01,0xa9,0xf4,0x4f,0x02,0xa9,0xf4,0xb3,0x00,0x90,
  0xf3,0x03,0x00,0xaa,0x88,0x2e,0x67,0x39,0x88,0x02,0x00,0x36,0xf4,0x88,0x00,0xd0};

inline constexpr MethodSpec kMethods[] = {
  {"exitState",          2u,"ExitState",          "System.Void",   0u,nullptr,                            nullptr,            0u,0x0ad3724cULL,kExitState,         sizeof(kExitState)},          // 0
  {"dismiss",            0u,"Dismiss",             "System.Void",   0u,nullptr,                            nullptr,            0u,0x0a375344ULL,kDismiss,           sizeof(kDismiss)},            // 1
  {"initialize",         2u,"Initialize",          "System.Void",   1u,"Holoholo.Rpc.CatchPokemonOutProto",nullptr,            0u,0x0ad37690ULL,kInitialize,        sizeof(kInitialize)},         // 2
  {"setScores",          1u,"SetScores",           "System.Void",   2u,"Holoholo.Rpc.CatchPokemonOutProto","Holoholo.Rpc.Item",0u,0x0ad2eb84ULL,kSetScores,         sizeof(kSetScores)},          // 3
  {"handleOpenComplete", 1u,"HandleOpenComplete",  "System.Void",   0u,nullptr,                            nullptr,            0u,0x0ad2c7fcULL,kHandleOpenComplete,sizeof(kHandleOpenComplete)}, // 4
  {"alive",              4u,"op_Implicit",         "System.Boolean",1u,"UnityEngine.Object",                nullptr,            1u,0x0da6bba0ULL,kOpImplicit,        sizeof(kOpImplicit)},         // 5
  {"execute",            5u,"ExecuteTasks",        "System.Void",   0u,nullptr,                            nullptr,            1u,0x0ULL,       nullptr,            0u},                          // 6
};
inline constexpr size_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);

inline constexpr unsigned char kDebugPrologue[] = {
  0x08,0x10,0x05,0x90, 0x08,0x45,0x47,0xf9, 0x48,0x01,0x00,0xb4, 0x09,0x10,0x05,0x90,
  0x29,0x49,0x47,0xf9, 0xe9,0x00,0x00,0xb4, 0x0a,0x04,0x40,0xf9, 0x1f,0x01,0x0a,0xeb,
  0xe8,0x03,0x1f,0x2a, 0x28,0x01,0x00,0x54, 0x3f,0x01,0x0a,0xeb, 0xe3,0x00,0x00,0x54,
  0xa1,0x00,0x00,0xb4, 0x08,0x04,0x40,0xf9, 0x3f,0x08,0x00,0xb9, 0x3f,0x08,0x00,0xf9,
  0x28,0x00,0x00,0xf9, 0x28,0x00,0x80,0x52, 0xe0,0x03,0x08,0x2a, 0xc0,0x03,0x5f,0xd6};
inline constexpr uint32_t kDebugPrologueLen = sizeof(kDebugPrologue);

// ---------------------------------------------------------------------------------------------------
// All 21 optional subfeature blocks in quick-close.js prepare() (zero DELEGATED slots).
// ---------------------------------------------------------------------------------------------------
inline constexpr OptionalSpec kOptional[] = {
  { 0, "catchCanvas"},
  { 1, "catchDiscovery"},
  { 2, "catchScene"},
  { 3, "details"},
  { 4, "directPresentation"},
  { 5, "transfer"},
  { 6, "spin"},
  { 7, "spinCamera"},
  { 8, "items"},
  { 9, "itemsLookup"},
  {10, "rocket"},
  {11, "battles"},
  {12, "berries"},
  {13, "incubator"},
  {14, "buddy"},
  {15, "gifts"},
  {16, "spawn"},
  {17, "vgp"},
  {18, "vgpMarker"},
  {19, "mapPoi"},
  {20, "mapPoiExtra"},
};
inline constexpr size_t kOptionalCount = sizeof(kOptional) / sizeof(kOptional[0]);

enum OptionalId {
  OPT_CATCH_CANVAS = 0, OPT_CATCH_DISCOVERY = 1, OPT_CATCH_SCENE = 2, OPT_DETAILS = 3,
  OPT_DIRECT_PRESENTATION = 4, OPT_TRANSFER = 5, OPT_SPIN = 6, OPT_SPIN_CAMERA = 7,
  OPT_ITEMS = 8, OPT_ITEMS_LOOKUP = 9, OPT_ROCKET = 10, OPT_BATTLES = 11, OPT_BERRIES = 12,
  OPT_INCUBATOR = 13, OPT_BUDDY = 14, OPT_GIFTS = 15, OPT_SPAWN = 16, OPT_VGP = 17,
  OPT_VGP_MARKER = 18, OPT_MAP_POI = 19, OPT_MAP_POI_EXTRA = 20
};

// ---------------------------------------------------------------------------------------------------
// All 127 optional classes scanned by quick-close.js prepare() (line 256).
// ---------------------------------------------------------------------------------------------------
inline constexpr ClassSpec kOptClasses[] = {
  {"STRING",              "System",                                      "String",                                    "System.String"},                                    // 0
  {"SINGLE",              "System",                                      "Single",                                    "System.Single"},                                    // 1
  {"GUILAYER",            "Niantic.Holoholo.NUI",                        "GuiLayerService",                           "Niantic.Holoholo.NUI.GuiLayerService"},             // 2
  {"PKINFO_SERVICE",      "Niantic.Holoholo.UI.Pokemon",                 "PokemonInfoGuiService",                     "Niantic.Holoholo.UI.Pokemon.PokemonInfoGuiService"},// 3
  {"GAMESTATE",           "Niantic.Zeppelin.GameStates",                 "GameState",                                 "Niantic.Zeppelin.GameStates.GameState"},            // 4
  {"VGP_CAMERA",          "UnityEngine",                                 "Camera",                                    "UnityEngine.Camera"},                               // 5
  {"VGP_SCREEN",          "UnityEngine",                                 "Screen",                                    "UnityEngine.Screen"},                               // 6
  {"VGP_WILD",            "Niantic.Holoholo.Map",                        "WildMapPokemon",                            "Niantic.Holoholo.Map.WildMapPokemon"},              // 7
  {"VGP_POKEMON",         "Niantic.Holoholo.Map",                        "MapPokemon",                                "Niantic.Holoholo.Map.MapPokemon"},                  // 8
  {"VGP_STOP",            "Niantic.Holoholo.Pokestop",                   "MapPokestop",                               "Niantic.Holoholo.Pokestop.MapPokestop"},            // 9
  {"VGP_PLACE",           "Niantic.Holoholo.Map",                        "IMapPlace",                                 "Niantic.Holoholo.Map.IMapPlace"},                   // 10
  {"VGP_PLAYER",          "Niantic.Holoholo",                            "PlayerService",                             "Niantic.Holoholo.PlayerService"},                   // 11
  {"VGP_PLAYER_CONTRACT", "Niantic.Holoholo",                            "IPlayerService",                            "Niantic.Holoholo.IPlayerService"},                  // 12
  {"VGP_BAG_CONTRACT",    "Niantic.Holoholo",                            "IPokemonBag",                               "Niantic.Holoholo.IPokemonBag"},                     // 13
  {"VGP_ITEM_CONTRACT",   "Niantic.Holoholo",                            "IItemBag",                                  "Niantic.Holoholo.IItemBag"},                        // 14
  {"VGP_RPC",             "Niantic.Holoholo.Rpc",                        "RpcHandler",                                "Niantic.Holoholo.Rpc.RpcHandler"},                  // 15
  {"VGP_REQUEST",         "Holoholo.Rpc",                                "FortSearchProto",                           "Holoholo.Rpc.FortSearchProto"},                     // 16
  {"VGP_FORT",            "Holoholo.Rpc",                                "PokemonFortProto",                          "Holoholo.Rpc.PokemonFortProto"},                    // 17
  {"VGP_METHOD",          "Holoholo.Rpc",                                "Method",                                    "Holoholo.Rpc.Method"},                              // 18
  {"VGP_TYPE",            "System",                                      "Type",                                      "System.Type"},                                      // 19
  {"VGP_NAMEPLATE",       "Niantic.Holoholo.Encounter",                  "EncounterNameplate",                        "Niantic.Holoholo.Encounter.EncounterNameplate"},    // 20
  {"IENUMERABLE",         "System.Collections",                          "IEnumerable",                               "System.Collections.IEnumerable"},                   // 21
  {"INC_SERVICE",         "Niantic.Holoholo.Items",                      "IncubatorService",                          "Niantic.Holoholo.Items.IncubatorService"},          // 22
  {"INC_CONTRACT",        "Niantic.Holoholo.Items",                      "IIncubatorService",                         "Niantic.Holoholo.Items.IIncubatorService"},         // 23
  {"INC_PROTO",           "Holoholo.Rpc",                                "EggIncubatorProto",                         "Holoholo.Rpc.EggIncubatorProto"},                   // 24
  {"MAP_GYM",             "Niantic.Holoholo.Gym",                        "MapGym",                                    "Niantic.Holoholo.Gym.MapGym"},                      // 25
  {"RAID_INFO",           "Holoholo.Rpc",                                "RaidInfoProto",                             "Holoholo.Rpc.RaidInfoProto"},                       // 26
  {"GYM_DISPLAY",         "Holoholo.Rpc",                                "GymDisplayProto",                           "Holoholo.Rpc.GymDisplayProto"},                     // 27
  {"PK",                  "Holoholo.Rpc",                                "PokemonProto",                              "Holoholo.Rpc.PokemonProto"},                        // 28
  {"CHAR_SIZE",           "Holoholo.Rpc",                                "CharacterSize",                             "Holoholo.Rpc.CharacterSize"},                       // 29
  {"IENUMERATOR",         "System.Collections",                          "IEnumerator",                               "System.Collections.IEnumerator"},                   // 30
  {"IDISPOSABLE",         "System",                                      "IDisposable",                               "System.IDisposable"},                               // 31
  {"GIFT_BOX_PROTO",      "Holoholo.Rpc",                                "GiftBoxProto",                              "Holoholo.Rpc.GiftBoxProto"},                        // 32
  {"GIFT_OPEN_SEQ",       "Niantic.Holoholo.Social.Gifting.UI.OpenGiftGuiController", "ddh",                          "Niantic.Holoholo.Social.Gifting.UI.OpenGiftGuiController.ddh"}, // 33
  {"OPEN_GIFT_OUT",       "Holoholo.Rpc",                                "OpenGiftOutProto",                          "Holoholo.Rpc.OpenGiftOutProto"},                    // 34
  {"SEND_GIFT_OUT",       "Holoholo.Rpc",                                "SendGiftOutProto",                          "Holoholo.Rpc.SendGiftOutProto"},                    // 35
  {"BUDDY_7757",          "Niantic.Holoholo.Buddy",                      "BuddyObservedDataExt",                      "Niantic.Holoholo.Buddy.BuddyObservedDataExt"},      // 36
  {"BUDDY_7768",          "Niantic.Holoholo.Buddy",                      "BuddyRpcService",                           "Niantic.Holoholo.Buddy.BuddyRpcService"},           // 37
  {"BUDDY_7769",          "Niantic.Holoholo.Buddy",                      "BuddyStatsService",                         "Niantic.Holoholo.Buddy.BuddyStatsService"},         // 38
  {"BUDDY_8109",          "Niantic.Holoholo.Buddy",                      "BuddyPokemonService",                       "Niantic.Holoholo.Buddy.BuddyPokemonService"},       // 39
  {"IBUDDY_STATS",        "Niantic.Holoholo.Buddy",                      "IBuddyStatsService",                        "Niantic.Holoholo.Buddy.IBuddyStatsService"},        // 40
  {"BUDDY_OBS",           "Holoholo.Rpc",                                "BuddyObservedData",                         "Holoholo.Rpc.BuddyObservedData"},                   // 41
  {"BUDDY_FEED_OUT",      "Holoholo.Rpc",                                "BuddyFeedingOutProto",                      "Holoholo.Rpc.BuddyFeedingOutProto"},                // 42
  {"BUDDY_PK_PROTO",      "Holoholo.Rpc",                                "BuddyPokemonProto",                         "Holoholo.Rpc.BuddyPokemonProto"},                   // 43
  {"GIFT_5916",           "Niantic.Holoholo.Social.UI",                  "FriendProfileGuiController",                "Niantic.Holoholo.Social.UI.FriendProfileGuiController"}, // 44
  {"GIFT_5936",           "Niantic.Holoholo.Social.UI",                  "FriendProfileInfoPanel",                    "Niantic.Holoholo.Social.UI.FriendProfileInfoPanel"},     // 45
  {"GIFT_6149",           "Niantic.Holoholo.Social.Gifting",             "GiftingRpcService",                         "Niantic.Holoholo.Social.Gifting.GiftingRpcService"},     // 46
  {"GIFT_6154",           "Niantic.Holoholo.Social.Gifting",             "GiftingService",                            "Niantic.Holoholo.Social.Gifting.GiftingService"},        // 47
  {"GIFT_6164",           "Niantic.Holoholo.Social.Gifting",             "StickerService",                            "Niantic.Holoholo.Social.Gifting.StickerService"},        // 48
  {"GIFT_6176",           "Niantic.Holoholo.Social.Gifting.UI",          "StickerSelectionItemWidget",                "Niantic.Holoholo.Social.Gifting.UI.StickerSelectionItemWidget"}, // 49
  {"GIFT_6195",           "Niantic.Holoholo.Social.Gifting.UI",          "StickerSheetGuiController",                 "Niantic.Holoholo.Social.Gifting.UI.StickerSheetGuiController"},  // 50
  {"GIFT_6211",           "Niantic.Holoholo.Social.Gifting.UI",          "GiftItemView2Widget",                       "Niantic.Holoholo.Social.Gifting.UI.GiftItemView2Widget"},        // 51
  {"GIFT_6240",           "Niantic.Holoholo.Social.Gifting.UI",          "OpenGiftGuiController",                     "Niantic.Holoholo.Social.Gifting.UI.OpenGiftGuiController"},      // 52
  {"GIFT_6258",           "Niantic.Holoholo.Social.Gifting.UI",          "SendGiftGuiController",                     "Niantic.Holoholo.Social.Gifting.UI.SendGiftGuiController"},      // 53
  {"GIFT_6268",           "Niantic.Holoholo.Social.Gifting.UI",          "ViewGiftsGuiController",                    "Niantic.Holoholo.Social.Gifting.UI.ViewGiftsGuiController"},     // 54
  {"FRIEND_DETAILS",      "Niantic.Platform.Internal.Protos",            "FriendDetailsProto",                        "Niantic.Platform.Internal.Protos.FriendDetailsProto"},   // 55
  {"PLAYER_SUMMARY",      "Niantic.Platform.Internal.Protos",            "PlayerSummaryProto",                        "Niantic.Platform.Internal.Protos.PlayerSummaryProto"},   // 56
  {"GIFT_BOX_DETAILS",    "Holoholo.Rpc",                                "GiftBoxDetailsProto",                       "Holoholo.Rpc.GiftBoxDetailsProto"},                      // 57
  {"STICKER_PROTO",       "Holoholo.Rpc",                                "StickerProto",                              "Holoholo.Rpc.StickerProto"},                             // 58
  {"POKEBALL",            "Niantic.Holoholo.Encounter",                  "Pokeball",                                  "Niantic.Holoholo.Encounter.Pokeball"},                   // 59
  {"POKEBALL_THROW",      "Niantic.Holoholo.Encounter",                  "PokeballThrow",                             "Niantic.Holoholo.Encounter.PokeballThrow"},              // 60
  {"SPAWN",               "Niantic.Holoholo.Map",                        "MapEntityService",                          "Niantic.Holoholo.Map.MapEntityService"},                 // 61
  {"BERRY",               "Niantic.Holoholo.Encounter",                  "EncounterInteractionState",                 "Niantic.Holoholo.Encounter.EncounterInteractionState"},  // 62
  {"BERRYGUI",            "Niantic.Holoholo.Encounter",                  "EncounterItemUseGuiController",             "Niantic.Holoholo.Encounter.EncounterItemUseGuiController"}, // 63
  {"FEEDGUI",             "Niantic.Holoholo.UI.Items",                   "FeedItemGuiBase",                           "Niantic.Holoholo.UI.Items.FeedItemGuiBase"},             // 64
  {"BATTLE_9182",         "Niantic.Holoholo.Battle",                     "BattleGestureHandler",                      "Niantic.Holoholo.Battle.BattleGestureHandler"},          // 65
  {"BATTLE_9284",         "Niantic.Holoholo.Battle",                     "EnemyAlertIndicator",                       "Niantic.Holoholo.Battle.EnemyAlertIndicator"},           // 66
  {"BATTLE_9308",         "Niantic.Holoholo.Battle",                     "ChargeAttackDisplay",                       "Niantic.Holoholo.Battle.ChargeAttackDisplay"},           // 67
  {"BATTLE_9322",         "Niantic.Holoholo.Battle",                     "SwapUI",                                    "Niantic.Holoholo.Battle.SwapUI"},                        // 68
  {"BATTLE_10591",        "Niantic.Holoholo.Gym",                        "GymBattleState",                            "Niantic.Holoholo.Gym.GymBattleState"},                   // 69
  {"BATTLE_31244",        "Niantic.Holoholo.Bread",                      "BreadBattleState",                          "Niantic.Holoholo.Bread.BreadBattleState"},               // 70
  {"BATTLE_31263",        "Niantic.Holoholo.Bread",                      "TelegraphIndicator",                        "Niantic.Holoholo.Bread.TelegraphIndicator"},             // 71
  {"BATTLE_31280",        "Niantic.Holoholo.Bread",                      "BreadAbilityWidget",                        "Niantic.Holoholo.Bread.BreadAbilityWidget"},             // 72
  {"BATTLE_31285",        "Niantic.Holoholo.Bread",                      "BreadBattleGestureHandler",                 "Niantic.Holoholo.Bread.BreadBattleGestureHandler"},      // 73
  {"BATTLE_31337",        "Niantic.Holoholo.Bread",                      "BreadSwapUI",                               "Niantic.Holoholo.Bread.BreadSwapUI"},                    // 74
  {"BATTLE_31499",        "Niantic.Holoholo.VNext",                      "RaidBattleGestureHandler",                  "Niantic.Holoholo.VNext.RaidBattleGestureHandler"},       // 75
  {"BATTLE_31514",        "Niantic.Holoholo.VNext",                      "RaidBattleVNextState",                      "Niantic.Holoholo.VNext.RaidBattleVNextState"},           // 76
  {"BATTLE_31569",        "Niantic.Holoholo.VNext",                      "BattleDirector",                            "Niantic.Holoholo.VNext.BattleDirector"},                 // 77
  {"BATTLE_31623",        "Niantic.Holoholo.VNext",                      "EnemyAlertIndicatorVNext",                  "Niantic.Holoholo.VNext.EnemyAlertIndicatorVNext"},       // 78
  {"BATTLE_31639",        "Niantic.Holoholo.VNext",                      "SwapUIVNext",                               "Niantic.Holoholo.VNext.SwapUIVNext"},                    // 79
  {"BATTLE_44226",        "Niantic.Holoholo.VNext",                      "BattleState",                               "Niantic.Holoholo.VNext.BattleState"},                    // 80
  {"BEHAVIOUR",           "UnityEngine",                                 "Behaviour",                                 "UnityEngine.Behaviour"},                                 // 81
  {"BUTTON",              "UnityEngine.UI",                              "Button",                                    "UnityEngine.UI.Button"},                                 // 82
  {"RMINI",               "Niantic.Holoholo.Combat.InputChallenge",      "ParticleTapInputChallenge",                 "Niantic.Holoholo.Combat.InputChallenge.ParticleTapInputChallenge"}, // 83
  {"RMINIBASE",           "Niantic.Holoholo.Combat.InputChallenge",      "CombatInputChallenge",                      "Niantic.Holoholo.Combat.InputChallenge.CombatInputChallenge"},      // 84
  {"COMPONENT",           "UnityEngine",                                 "Component",                                 "UnityEngine.Component"},                                 // 85
  {"TRANSFORM",           "UnityEngine",                                 "Transform",                                 "UnityEngine.Transform"},                                 // 86
  {"GAMEOBJECT",          "UnityEngine",                                 "GameObject",                                "UnityEngine.GameObject"},                                // 87
  {"VECTOR3",             "UnityEngine",                                 "Vector3",                                   "UnityEngine.Vector3"},                                   // 88
  {"RVUI",                "Niantic.Holoholo.Combat.UI",                  "CombatVNextGuiController",                  "Niantic.Holoholo.Combat.UI.CombatVNextGuiController"},   // 89
  {"RVGEST",              "Niantic.Holoholo.VNext",                      "CombatVNextGestureHandler",                 "Niantic.Holoholo.VNext.CombatVNextGestureHandler"},      // 90
  {"RVBASE",              "Niantic.Holoholo.VNext",                      "BattleGestureHandlerVNext",                 "Niantic.Holoholo.VNext.BattleGestureHandlerVNext"},      // 91
  {"RVCHARGE",            "Niantic.Holoholo.VNext",                      "ChargeAttackDisplayVNext",                  "Niantic.Holoholo.VNext.ChargeAttackDisplayVNext"},       // 92
  {"ROCKET",              "Niantic.Holoholo.Combat.Contexts",            "InvasionCombatState",                       "Niantic.Holoholo.Combat.Contexts.InvasionCombatState"},  // 93
  {"RUI",                 "Niantic.Holoholo.Combat.UI",                  "CombatUI",                                  "Niantic.Holoholo.Combat.UI.CombatUI"},                   // 94
  {"RDIR",                "Niantic.Holoholo.Combat",                     "CombatDirectorV2",                          "Niantic.Holoholo.Combat.CombatDirectorV2"},              // 95
  {"RCHARGE",             "Niantic.Holoholo.Combat.UI",                  "CombatChargeAttackDisplay",                 "Niantic.Holoholo.Combat.UI.CombatChargeAttackDisplay"},  // 96
  {"RSHIELD",             "Niantic.Holoholo.Combat.InputChallenge",      "ShieldModalGuiController",                  "Niantic.Holoholo.Combat.InputChallenge.ShieldModalGuiController"}, // 97
  {"SELECTABLE",          "UnityEngine.UI",                              "Selectable",                                "UnityEngine.UI.Selectable"},                             // 98
  {"ITEM_GUI_SERVICE",    "Niantic.Holoholo.UI.Items",                   "ItemInventoryGuiService",                   "Niantic.Holoholo.UI.Items.ItemInventoryGuiService"},     // 99
  {"IITEM_GUI_SERVICE",   "Niantic.Holoholo.UI.Items",                   "IItemInventoryGuiService",                  "Niantic.Holoholo.UI.Items.IItemInventoryGuiService"},    // 100
  {"MAP_SCENE_CTX",       "Niantic.Holoholo",                            "MapSceneContext",                           "Niantic.Holoholo.MapSceneContext"},                      // 101
  {"SCENE_CTX",           "Zenject",                                     "SceneContext",                              "Zenject.SceneContext"},                                  // 102
  {"DICONTAINER",         "Zenject",                                     "DiContainer",                               "Zenject.DiContainer"},                                   // 103
  {"IITEM_SERVICE",       "Niantic.Holoholo.UI.Items",                   "IItemInventoryService",                     "Niantic.Holoholo.UI.Items.IItemInventoryService"},       // 104
  {"IBAG",                "Niantic.Holoholo.Internal",                   "ItemBagImpl",                               "Niantic.Holoholo.Internal.ItemBagImpl"},                 // 105
  {"ISERVICE",            "Niantic.Holoholo.UI.Items",                   "ItemInventoryService",                      "Niantic.Holoholo.UI.Items.ItemInventoryService"},        // 106
  {"IRESPONSE",           "Holoholo.Rpc",                                "RecycleItemOutProto",                       "Holoholo.Rpc.RecycleItemOutProto"},                      // 107
  {"ITEM",                "Holoholo.Rpc",                                "Item",                                      "Holoholo.Rpc.Item"},                                     // 108
  {"APPROACH",            "Niantic.Holoholo.Pokestop",                   "PokestopApproachState",                     "Niantic.Holoholo.Pokestop.PokestopApproachState"},       // 109
  {"RESOLVE",             "Niantic.Holoholo.Pokestop",                   "PokestopResolveState",                      "Niantic.Holoholo.Pokestop.PokestopResolveState"},        // 110
  {"SPLINE",              "Niantic.Holoholo.CameraRig",                  "SplineCameraController",                    "Niantic.Holoholo.CameraRig.SplineCameraController"},     // 111
  {"SPINSTATE",           "Niantic.Holoholo.Pokestop",                   "PokestopSpinnerInteractionState",           "Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionState"}, // 112
  {"SPINGUI",             "Niantic.Holoholo.Pokestop",                   "PokestopSpinnerInteractionGuiController",   "Niantic.Holoholo.Pokestop.PokestopSpinnerInteractionGuiController"}, // 113
  {"GESTURE",             "Niantic.Holoholo.GameInput",                  "ItemSpinnerGestureHandler",                 "Niantic.Holoholo.GameInput.ItemSpinnerGestureHandler"},  // 114
  {"SPINNER",             "Niantic.Holoholo.Map",                        "PoiItemSpinner",                            "Niantic.Holoholo.Map.PoiItemSpinner"},                   // 115
  {"SPINBASE",            "Niantic.Holoholo.Map",                        "BasePoiSpinner",                            "Niantic.Holoholo.Map.BasePoiSpinner"},                   // 116
  {"SPINRESULT",          "Holoholo.Rpc",                                "FortSearchOutProto",                        "Holoholo.Rpc.FortSearchOutProto"},                       // 117
  {"RELEASE",             "Holoholo.Rpc",                                "ReleasePokemonOutProto",                    "Holoholo.Rpc.ReleasePokemonOutProto"},                   // 118
  {"BAG",                 "Niantic.Holoholo.Internal",                   "PokemonBagImpl",                            "Niantic.Holoholo.Internal.PokemonBagImpl"},              // 119
  {"DISPLAY",             "Holoholo.Rpc",                                "PokemonDisplayProto",                       "Holoholo.Rpc.PokemonDisplayProto"},                      // 120
  {"DGUI",                "Niantic.Holoholo.UI.Pokemon",                 "PokemonInfoGuiController",                  "Niantic.Holoholo.UI.Pokemon.PokemonInfoGuiController"},  // 121
  {"DATA",                "Niantic.Holoholo.UI.Pokemon",                 "PokemonInfoData",                           "Niantic.Holoholo.UI.Pokemon.PokemonInfoData"},           // 122
  {"VIEW",                "Niantic.Holoholo.Encounter",                  "EncounterViewPokemonState",                 "Niantic.Holoholo.Encounter.EncounterViewPokemonState"},  // 123
  {"INT",                 "System",                                      "Int32",                                     "System.Int32"},                                          // 124
  {"DELEGATE",            "System",                                      "Delegate",                                  "System.Delegate"},                                       // 125
  {"ACTION",              "System",                                      "Action",                                    "System.Action"},                                         // 126
};
inline constexpr size_t kOptClassCount = sizeof(kOptClasses) / sizeof(kOptClasses[0]);
static_assert(kOptClassCount == 127, "127 optional classes");

enum OptClassId {
  OC_STRING = 0, OC_SINGLE, OC_GUILAYER, OC_PKINFO_SERVICE, OC_GAMESTATE,
  OC_VGP_CAMERA, OC_VGP_SCREEN, OC_VGP_WILD, OC_VGP_POKEMON, OC_VGP_STOP,
  OC_VGP_PLACE, OC_VGP_PLAYER, OC_VGP_PLAYER_CONTRACT, OC_VGP_BAG_CONTRACT, OC_VGP_ITEM_CONTRACT,
  OC_VGP_RPC, OC_VGP_REQUEST, OC_VGP_FORT, OC_VGP_METHOD, OC_VGP_TYPE,
  OC_VGP_NAMEPLATE, OC_IENUMERABLE, OC_INC_SERVICE, OC_INC_CONTRACT, OC_INC_PROTO,
  OC_MAP_GYM, OC_RAID_INFO, OC_GYM_DISPLAY, OC_PK, OC_CHAR_SIZE,
  OC_IENUMERATOR, OC_IDISPOSABLE, OC_GIFT_BOX_PROTO, OC_GIFT_OPEN_SEQ, OC_OPEN_GIFT_OUT,
  OC_SEND_GIFT_OUT, OC_BUDDY_7757, OC_BUDDY_7768, OC_BUDDY_7769, OC_BUDDY_8109,
  OC_IBUDDY_STATS, OC_BUDDY_OBS, OC_BUDDY_FEED_OUT, OC_BUDDY_PK_PROTO,
  OC_GIFT_5916, OC_GIFT_5936, OC_GIFT_6149, OC_GIFT_6154, OC_GIFT_6164,
  OC_GIFT_6176, OC_GIFT_6195, OC_GIFT_6211, OC_GIFT_6240, OC_GIFT_6258, OC_GIFT_6268,
  OC_FRIEND_DETAILS, OC_PLAYER_SUMMARY, OC_GIFT_BOX_DETAILS, OC_STICKER_PROTO,
  OC_POKEBALL, OC_POKEBALL_THROW, OC_SPAWN, OC_BERRY, OC_BERRYGUI, OC_FEEDGUI,
  OC_BATTLE_9182, OC_BATTLE_9284, OC_BATTLE_9308, OC_BATTLE_9322, OC_BATTLE_10591,
  OC_BATTLE_31244, OC_BATTLE_31263, OC_BATTLE_31280, OC_BATTLE_31285, OC_BATTLE_31337,
  OC_BATTLE_31499, OC_BATTLE_31514, OC_BATTLE_31569, OC_BATTLE_31623, OC_BATTLE_31639, OC_BATTLE_44226,
  OC_BEHAVIOUR, OC_BUTTON, OC_RMINI, OC_RMINIBASE, OC_COMPONENT, OC_TRANSFORM, OC_GAMEOBJECT, OC_VECTOR3,
  OC_RVUI, OC_RVGEST, OC_RVBASE, OC_RVCHARGE, OC_ROCKET, OC_RUI, OC_RDIR, OC_RCHARGE, OC_RSHIELD, OC_SELECTABLE,
  OC_ITEM_GUI_SERVICE, OC_IITEM_GUI_SERVICE, OC_MAP_SCENE_CTX, OC_SCENE_CTX, OC_DICONTAINER, OC_IITEM_SERVICE,
  OC_IBAG, OC_ISERVICE, OC_IRESPONSE, OC_ITEM, OC_APPROACH, OC_RESOLVE, OC_SPLINE,
  OC_SPINSTATE, OC_SPINGUI, OC_GESTURE, OC_SPINNER, OC_SPINBASE, OC_SPINRESULT,
  OC_RELEASE, OC_BAG, OC_DISPLAY, OC_DGUI, OC_DATA, OC_VIEW, OC_INT, OC_DELEGATE, OC_ACTION
};

// ---------------------------------------------------------------------------------------------------
// Opaque handle and integer slots for all 21 compiled optional subfeatures.
// ---------------------------------------------------------------------------------------------------
enum OptHandle {
  // 0. catchCanvas (2)
  H_CC_METHOD = 0, H_CC_CODE,
  // 1. catchDiscovery (4)
  H_CD_CLASS, H_CD_LAYER, H_CD_TOP, H_CD_TOP_CODE,
  // 2. catchScene (3)
  H_CS_FIND, H_CS_TYPE_OBJ, H_CS_ARRAY_LEN,
  // 3. details (22)
  H_DET_DELEGATE_TARGET, H_DET_VIEW_RESP, H_DET_DATA, H_DET_CALLBACK, H_DET_ON_DISMISSED,
  H_DET_LIST, H_DET_INDEX, H_DET_POST, H_DET_ID, H_DET_LIST_CLASS,
  H_DET_PRESENT, H_DET_PRESENT_CODE, H_DET_EXIT, H_DET_EXIT_CODE,
  H_DET_INIT, H_DET_INIT_CODE, H_DET_OPEN, H_DET_OPEN_CODE,
  H_DET_CLICK, H_DET_CLICK_CODE, H_DET_COUNT, H_DET_ITEM,
  // 4. directPresentation (5)
  H_DP_ACTIVE, H_DP_SERVICE, H_DP_SHOW, H_DP_SHOW_CODE, H_DP_CALLBACK,
  // 5. transfer (32)
  H_TR_RESULT_STATUS, H_TR_BAG, H_TR_GM, H_TR_SPECIES, H_TR_ATTACK, H_TR_DEFENSE, H_TR_STAMINA,
  H_TR_FAVORITE, H_TR_EGG, H_TR_DISPLAY, H_TR_EVENTS, H_TR_LIMITED, H_TR_COSTUME, H_TR_ORIG_COSTUME,
  H_TR_BACKGROUND, H_TR_ART, H_TR_ART_ASSET, H_TR_CB_POKEMON, H_TR_CB_BAG,
  H_TR_EVENT_CLASS, H_TR_CB_CLASS,
  H_TR_RESULT_M, H_TR_RESULT_CODE, H_TR_ERROR_M, H_TR_ERROR_CODE,
  H_TR_SIZE_M, H_TR_RARE_M, H_TR_SHINY_M, H_TR_EVENT_COUNT_M, H_TR_GET_M,
  H_TR_RELEASE_M, H_TR_RELEASE_CODE,
  // 6. spin (27)
  H_SPIN_SPINSTATE, H_SPIN_SPINGUI, H_SPIN_GESTURE, H_SPIN_SPINNER, H_SPIN_SPINBASE, H_SPIN_SPINRESULT,
  H_SPIN_GUI_F, H_SPIN_GESTURE_F, H_SPIN_SPINNER_F, H_SPIN_RESULT_F, H_SPIN_RESULT_ENUM,
  H_SPIN_ENTER, H_SPIN_ENTER_CODE, H_SPIN_EXIT, H_SPIN_EXIT_CODE, H_SPIN_RESULT_M, H_SPIN_RESULT_CODE,
  H_SPIN_CLOSE, H_SPIN_CLOSE_CODE, H_SPIN_SPIN_M, H_SPIN_SPIN_CODE, H_SPIN_REQUEST, H_SPIN_REQUEST_CODE,
  H_SPIN_CAN, H_SPIN_CAN_CODE, H_SPIN_ACTIVE, H_SPIN_ACTIVE_CODE,
  // 7. spinCamera (17)
  H_SC_ITER, H_SC_THIS_F, H_SC_STATE_F, H_SC_MOVE, H_SC_MOVE_CODE,
  H_SC_APPROACH, H_SC_APP_CAM, H_SC_APP_ENTER, H_SC_APP_ENTER_CODE, H_SC_APP_EXIT, H_SC_APP_EXIT_CODE,
  H_SC_RESOLVE, H_SC_RES_CAM, H_SC_RES_ENTER, H_SC_RES_ENTER_CODE, H_SC_RES_EXIT, H_SC_RES_EXIT_CODE,
  // 8. items (29)
  H_IT_DK, H_IT_CB,
  H_IT_DEL_METHOD, H_IT_DEL_TARGET, H_IT_BAG, H_IT_ID, H_IT_COUNT_F, H_IT_RECYCLABLE,
  H_IT_CB_BAG, H_IT_PREDICTION, H_IT_PROTO_M, H_IT_PROTO_ID, H_IT_STATUS,
  H_IT_LIST_M, H_IT_LIST_CODE, H_IT_INIT_M, H_IT_INIT_CODE, H_IT_COUNT_M, H_IT_GET_M,
  H_IT_EXPIRING_M, H_IT_EXPIRING_CODE, H_IT_QUANTITY_M, H_IT_QUANTITY_CODE,
  H_IT_RECYCLE_M, H_IT_RECYCLE_CODE, H_IT_RESULT_M, H_IT_RESULT_CODE, H_IT_ERROR_M, H_IT_ERROR_CODE,
  // 9. itemsLookup (18)
  H_IL_MAP, H_IL_CONTRACT, H_IL_GUI, H_IL_GUI_CONTRACT, H_IL_SINGLETON, H_IL_INSTANCE,
  H_IL_USABLE_M, H_IL_USABLE_CODE, H_IL_SHOW_M, H_IL_SHOW_CODE, H_IL_FUNC_TYPE,
  H_IL_REFLECT_EXP, H_IL_INIT_CLASS_EXP, H_IL_CREATE_DEL_M, H_IL_TYPE_OBJ_EXP,
  H_IL_FIND_M, H_IL_CONTAINER_M, H_IL_RESOLVE_M,
  // 10. rocket (69)
  H_RK_VN_GESTURE, H_RK_VN_ENABLED, H_RK_VN_TAP, H_RK_VN_BUTTON,
  H_RK_VN_UPDATE_M, H_RK_VN_UPDATE_CODE, H_RK_VN_CLOSE_M, H_RK_VN_CLOSE_CODE,
  H_RK_VN_BLOCKED_M, H_RK_VN_BLOCKED_CODE, H_RK_VN_FAST_M, H_RK_VN_FAST_CODE,
  H_RK_VN_OBSERVE_M, H_RK_VN_OBSERVE_CODE, H_RK_VN_CHARGE_M, H_RK_VN_CHARGE_CODE,
  H_RK_VN_CHARGED_M, H_RK_VN_CHARGED_CODE, H_RK_VN_VISIBLE_M, H_RK_VN_VISIBLE_CODE,
  H_RK_MN_COLLIDER, H_RK_MN_RUNNING,
  H_RK_MN_UPDATE_M, H_RK_MN_UPDATE_CODE, H_RK_MN_EXIT_M, H_RK_MN_EXIT_CODE,
  H_RK_MN_HIDE_M, H_RK_MN_HIDE_CODE, H_RK_MN_DESTROY_M, H_RK_MN_DESTROY_CODE,
  H_RK_MN_TRANSFORM_M, H_RK_MN_GAMEOBJ_M, H_RK_MN_SCALE_M, H_RK_MN_SETSCALE_M,
  H_RK_MN_ACTIVE_M, H_RK_MN_SETACTIVE_M,
  H_RK_FIRST, H_RK_SECOND, H_RK_CHARGED, H_RK_INPUT, H_RK_SHIELDS, H_RK_BUTTON,
  H_RK_ENTER_M, H_RK_ENTER_CODE, H_RK_EXIT_M, H_RK_EXIT_CODE,
  H_RK_UPDATE_M, H_RK_UPDATE_CODE, H_RK_UI_M, H_RK_UI_CODE,
  H_RK_CLOSE_M, H_RK_CLOSE_CODE, H_RK_DIRECTOR_M, H_RK_DIRECTOR_CODE,
  H_RK_ACTIVE_M, H_RK_ACTIVE_CODE, H_RK_FAST_M, H_RK_FAST_CODE,
  H_RK_CHARGE_M, H_RK_CHARGE_CODE, H_RK_VISIBLE_M, H_RK_VISIBLE_CODE,
  H_RK_PROMPT_M, H_RK_PROMPT_CODE, H_RK_SHIELD_M, H_RK_SHIELD_CODE,
  H_RK_SHIELD_CLOSE_M, H_RK_SHIELD_CLOSE_CODE, H_RK_INTERACTABLE_M,
  // 11. battles (102)
  H_BT_SC0_CLASS, H_BT_SC0_ENTER_M, H_BT_SC0_ENTER_CODE, H_BT_SC0_EXIT_M, H_BT_SC0_EXIT_CODE,
  H_BT_SC1_CLASS, H_BT_SC1_ENTER_M, H_BT_SC1_ENTER_CODE, H_BT_SC1_EXIT_M, H_BT_SC1_EXIT_CODE,
  H_BT_SC2_CLASS, H_BT_SC2_ENTER_M, H_BT_SC2_ENTER_CODE, H_BT_SC2_EXIT_M, H_BT_SC2_EXIT_CODE,
  H_BT_SW0_CLASS, H_BT_SW0_OPEN_M, H_BT_SW0_OPEN_CODE, H_BT_SW0_CL0_M, H_BT_SW0_CL0_CODE, H_BT_SW0_CL1_M, H_BT_SW0_CL1_CODE, H_BT_SW0_SHOW_M, H_BT_SW0_SHOW_CODE,
  H_BT_SW1_CLASS, H_BT_SW1_OPEN_M, H_BT_SW1_OPEN_CODE, H_BT_SW1_CL0_M, H_BT_SW1_CL0_CODE, H_BT_SW1_CL1_M, H_BT_SW1_CL1_CODE, H_BT_SW1_SHOW_M, H_BT_SW1_SHOW_CODE,
  H_BT_SW2_CLASS, H_BT_SW2_OPEN_M, H_BT_SW2_OPEN_CODE, H_BT_SW2_CL0_M, H_BT_SW2_CL0_CODE, H_BT_SW2_CL1_M, H_BT_SW2_CL1_CODE, H_BT_SW2_SHOW_M, H_BT_SW2_SHOW_CODE,
  H_BT_GYM_ITER, H_BT_GYM_WARN_STATE,
  H_BT_F_MAX_GESTURE, H_BT_F_DIRECTOR, H_BT_F_BATTLE_STATUS, H_BT_F_LEG_FLICK, H_BT_F_LEG_TAP, H_BT_F_LEG_BUTTON, H_BT_F_MAX_BUTTON, H_BT_F_MAX_TYPE,
  H_BT_M_CUR_STATE, H_BT_M_CUR_STATE_CODE, H_BT_M_RAID_FAST_ACT, H_BT_M_RAID_FAST_ACT_CODE,
  H_BT_M_DESTROY, H_BT_M_DESTROY_CODE, H_BT_M_DODGE, H_BT_M_DODGE_CODE,
  H_BT_M_GYM_WARN, H_BT_M_GYM_WARN_CODE, H_BT_M_RAID_WARN, H_BT_M_RAID_WARN_CODE,
  H_BT_M_MAX_WARN, H_BT_M_MAX_WARN_CODE, H_BT_M_LEG_DODGE, H_BT_M_LEG_DODGE_CODE,
  H_BT_M_ENABLE, H_BT_M_ENABLE_CODE, H_BT_M_DISABLE, H_BT_M_DISABLE_CODE,
  H_BT_M_BUTTONS, H_BT_M_BUTTONS_CODE, H_BT_M_RAID_FAST, H_BT_M_RAID_FAST_CODE,
  H_BT_M_MAX_FAST, H_BT_M_MAX_FAST_CODE, H_BT_M_LEG_ENABLE, H_BT_M_LEG_ENABLE_CODE,
  H_BT_M_LEG_DISABLE, H_BT_M_LEG_DISABLE_CODE, H_BT_M_LEG_BUTTONS, H_BT_M_LEG_BUTTONS_CODE,
  H_BT_M_LEG_FAST, H_BT_M_LEG_FAST_CODE, H_BT_M_LEG_INIT, H_BT_M_LEG_INIT_CODE,
  H_BT_M_LEG_CHARGED, H_BT_M_LEG_CHARGED_CODE, H_BT_M_LEG_CHARGE, H_BT_M_LEG_CHARGE_CODE,
  H_BT_M_MAX_REFRESH, H_BT_M_MAX_REFRESH_CODE, H_BT_M_MAX_CLEANUP, H_BT_M_MAX_CLEANUP_CODE,
  H_BT_M_ACTIVE, H_BT_M_PRESS,
  // 12. berries (38)
  H_BR_CALLBACK,
  H_BR_F_IMAGE, H_BR_F_BAG, H_BR_F_GUI, H_BR_F_UI, H_BR_F_CURRENT, H_BR_F_CB_SCOPE, H_BR_F_CB_BERRY,
  H_BR_M_VISIBLE, H_BR_M_SET_VISIBLE,
  H_BR_M_ENTER, H_BR_M_ENTER_CODE, H_BR_M_EXIT, H_BR_M_EXIT_CODE,
  H_BR_M_SELECT, H_BR_M_SELECT_CODE, H_BR_M_CAN, H_BR_M_CAN_CODE,
  H_BR_M_BALL, H_BR_M_BALL_CODE, H_BR_M_PHOTO, H_BR_M_PHOTO_CODE,
  H_BR_M_CONFIRM, H_BR_M_CONFIRM_CODE, H_BR_M_DIRECT, H_BR_M_DIRECT_CODE,
  H_BR_M_RESULT, H_BR_M_RESULT_CODE, H_BR_M_ERROR, H_BR_M_ERROR_CODE,
  H_BR_M_SUBMIT, H_BR_M_SUBMIT_CODE, H_BR_M_PEEK, H_BR_M_PEEK_CODE,
  H_BR_M_BREAD, H_BR_M_BREAD_CODE, H_BR_M_BALL_TYPE, H_BR_M_BALL_TYPE_CODE,
  // 13. incubator (20)
  H_INC_F_EGG, H_INC_F_ID, H_INC_F_TARGET, H_INC_F_START, H_INC_F_ASSIGNMENT,
  H_INC_F_EGG_TYPE, H_INC_F_ITEM, H_INC_F_INC_ID, H_INC_F_PK_ID, H_INC_F_EXPIRY,
  H_INC_M_PUT, H_INC_M_PUT_CODE, H_INC_M_INCUBATORS, H_INC_M_INCUBATORS_CODE,
  H_INC_M_EGGS, H_INC_M_EGGS_CODE, H_INC_M_ENUMERATE, H_INC_M_MOVE, H_INC_M_CURRENT, H_INC_M_DISPOSE,
  // 14. buddy (25)
  H_BD_CALLBACK, H_BD_CONTRACT,
  H_BD_F_RPC, H_BD_F_POKEMON, H_BD_F_BAG, H_BD_F_CB_RPC, H_BD_F_RESULT, H_BD_F_BUDDY_ID, H_BD_F_FEED_STATS,
  H_BD_M_STATS, H_BD_M_STATS_CODE, H_BD_M_HAS, H_BD_M_HAS_CODE, H_BD_M_POKEMON, H_BD_M_POKEMON_CODE,
  H_BD_M_REMAINING, H_BD_M_REMAINING_CODE, H_BD_M_FULLNESS, H_BD_M_FULLNESS_CODE,
  H_BD_M_COOLDOWN, H_BD_M_COOLDOWN_CODE, H_BD_M_FEED, H_BD_M_FEED_CODE, H_BD_M_RESULT, H_BD_M_RESULT_CODE,
  // 15. gifts (127)
  H_GF_STICKER_LIST_CLASS, H_GF_LIST_CLASS, H_GF_SELECTED_CLASS, H_GF_ROWS_CLASS, H_GF_CB_CLASS,
  H_GF_F_DATA, H_GF_F_PANEL_FRIEND, H_GF_F_PLAYER, H_GF_F_PLAYER_ID, H_GF_F_SENDER,
  H_GF_F_OPEN_DETAILS, H_GF_F_SEND_FRIEND, H_GF_F_SEND_STICKER, H_GF_F_STICKER_SVC,
  H_GF_F_OPEN_BTN, H_GF_F_SEND_BTN, H_GF_F_CHOOSE_BTN, H_GF_F_STICKER_BTN,
  H_GF_F_WIDGET_BTN, H_GF_F_WIDGET_STICKER, H_GF_F_DELETE_MODE, H_GF_F_SURPRISE,
  H_GF_F_SEQ_GUI, H_GF_F_SEQ_RESP, H_GF_F_SEQ_STATE, H_GF_F_OPEN_RESULT,
  H_GF_F_LIST_ITEMS, H_GF_F_SELECTED, H_GF_F_SEND_RESULT, H_GF_F_ROWS, H_GF_F_LIST_SURPRISE,
  H_GF_F_PANEL_GIFTING, H_GF_F_PANEL_RPC, H_GF_F_RPC_STICKERS, H_GF_F_GIFT_ID, H_GF_F_STICKER_ID,
  H_GF_F_CHECK_RESULT, H_GF_F_CHECK_PENDING, H_GF_F_NIA_UTIL, H_GF_F_CB_SVC, H_GF_F_CB_ID,
  H_GF_M_PROF_OPEN, H_GF_M_PROF_OPEN_CODE, H_GF_M_PROF_CLOSE, H_GF_M_PROF_CLOSE_CODE,
  H_GF_M_PANEL, H_GF_M_PANEL_CODE, H_GF_M_DISABLED, H_GF_M_DISABLED_CODE,
  H_GF_M_CHECK, H_GF_M_CHECK_CODE, H_GF_M_CHOOSE, H_GF_M_CHOOSE_CODE,
  H_GF_M_OPEN_READY, H_GF_M_OPEN_READY_CODE, H_GF_M_OPEN_CLOSE, H_GF_M_OPEN_CLOSE_CODE,
  H_GF_M_OPEN, H_GF_M_OPEN_CODE, H_GF_M_SEND_READY, H_GF_M_SEND_READY_CODE,
  H_GF_M_SEND_CLOSE, H_GF_M_SEND_CLOSE_CODE, H_GF_M_SEND, H_GF_M_SEND_CODE,
  H_GF_M_STICKERS, H_GF_M_STICKERS_CODE, H_GF_M_LIST_INIT, H_GF_M_LIST_INIT_CODE,
  H_GF_M_LIST_READY, H_GF_M_LIST_READY_CODE, H_GF_M_LIST_CLOSE, H_GF_M_LIST_CLOSE_CODE,
  H_GF_M_GIFT_UPDATE, H_GF_M_GIFT_UPDATE_CODE, H_GF_M_GIFT_BOX, H_GF_M_GIFT_BOX_CODE,
  H_GF_M_GIFT_CLICK, H_GF_M_GIFT_CLICK_CODE, H_GF_M_STICKER_INIT, H_GF_M_STICKER_INIT_CODE,
  H_GF_M_STICKER_CLICK, H_GF_M_STICKER_CLICK_CODE, H_GF_M_SHEET_READY, H_GF_M_SHEET_READY_CODE,
  H_GF_M_SHEET_CLOSE, H_GF_M_SHEET_CLOSE_CODE, H_GF_M_INVENTORY, H_GF_M_INVENTORY_CODE,
  H_GF_M_SEL_STICKER, H_GF_M_SEL_STICKER_CODE, H_GF_M_QUANTITY, H_GF_M_QUANTITY_CODE,
  H_GF_M_ANY_STICKERS, H_GF_M_ANY_STICKERS_CODE, H_GF_M_ACTIVE, H_GF_M_INTERACTABLE,
  H_GF_M_STICKER_COUNT, H_GF_M_STICKER_ITEM, H_GF_M_SEQ, H_GF_M_SEQ_CODE,
  H_GF_M_LIST_COUNT, H_GF_M_LIST_ITEM, H_GF_M_SEL_FIRST,
  H_GF_M_LIST_EXIT, H_GF_M_LIST_EXIT_CODE, H_GF_M_SEND_RESULT, H_GF_M_SEND_RESULT_CODE,
  H_GF_M_ROW_COUNT, H_GF_M_ROW_ITEM, H_GF_M_RPC_SEND, H_GF_M_RPC_SEND_CODE,
  H_GF_M_SENDABLE, H_GF_M_SENDABLE_CODE, H_GF_M_RESERVE, H_GF_M_RESERVE_CODE,
  H_GF_M_RETURN_STICKER, H_GF_M_RETURN_STICKER_CODE,
  H_GF_M_GET_ENUM, H_GF_M_CUR_GIFT, H_GF_M_MOVE_GIFT, H_GF_M_DISP_GIFT, H_GF_M_NIA_ID,
  H_GF_M_RPC_RESULT, H_GF_M_RPC_RESULT_CODE, H_GF_M_RPC_FINALLY, H_GF_M_RPC_FINALLY_CODE,
  // 16. spawn (2)
  H_SP_METHOD, H_SP_CODE,
  // 17. vgp (36)
  H_VGP_M_ENCOUNTER, H_VGP_M_CAPTURE, H_VGP_M_MANUAL_UI, H_VGP_M_MANUAL_UI_CODE,
  H_VGP_M_LOCATION, H_VGP_M_ID, H_VGP_M_VISIBLE, H_VGP_M_MAX_ITEMS, H_VGP_M_MAX_PK,
  H_VGP_M_ITEM_COUNT, H_VGP_M_TOTAL_ITEMS, H_VGP_M_PK_COUNT, H_VGP_M_ACTIVE, H_VGP_M_RANGE,
  H_VGP_M_CLOSED, H_VGP_M_COOLING, H_VGP_M_FORT, H_VGP_M_COOLDOWN, H_VGP_M_CTOR, H_VGP_M_FIND_ALL,
  H_VGP_F_FORT_ID, H_VGP_F_FORT_LAT, H_VGP_F_FORT_LON, H_VGP_F_ID, H_VGP_F_LAT, H_VGP_F_LON,
  H_VGP_F_TARGET_LAT, H_VGP_F_TARGET_LON, H_VGP_F_RESULT, H_VGP_F_UNTIL, H_VGP_F_CATCH_STATUS, H_VGP_F_BAG_GM,
  H_VGP_EXP_PARENT, H_VGP_EXP_ARRAY, H_VGP_EXP_BARRIER, H_VGP_EXP_UNREFLECT,
  // 18. vgpMarker (6)
  H_VGPM_CAMERA, H_VGPM_TRANSFORM, H_VGPM_POSITION, H_VGPM_PROJECT, H_VGPM_WIDTH, H_VGPM_HEIGHT,
  // 19. mapPoi (7)
  H_MP_GYM, H_MP_RAID, H_MP_INFO, H_MP_END, H_MP_SPAWN, H_MP_BATTLE, H_MP_HIDDEN,
  // 20. mapPoiExtra (8)
  H_MPE_LEVEL, H_MPE_BOSS_CLASS, H_MPE_BOSS, H_MPE_BOSS_ID, H_MPE_TEAM_M,
  H_MPE_GYM_DISP_CLASS, H_MPE_GYM_DISP, H_MPE_SLOTS,
  PGR_OPT_HANDLE_COUNT
};
static_assert(PGR_OPT_HANDLE_COUNT == 595, "595 optional handles");

enum OptInt {
  I_TR_SIZE_XXS = 0, I_TR_SIZE_XS, I_TR_SIZE_M, I_TR_SIZE_XL, I_TR_SIZE_XXL,
  I_TR_RESULT_SUCCESS, I_TR_COSTUME_NONE, I_TR_ART_NONE, I_TR_ASSET_NONE,
  I_SPIN_SUCCESS,
  I_ITEMS_SUCCESS,
  I_BAT_STATUS_ACTIVE, I_BAT_STATUS_UNSET, I_BAT_DODGE_LEFT, I_BAT_MAX_ATTACK,
  I_INC_UNLIMITED, I_INC_SPECIAL,
  I_BUDDY_SUCCESS,
  I_GIFT_OPEN_SUCCESS, I_GIFT_SEND_SUCCESS, I_GIFT_CHECK_SUCCESS,
  I_VGP_SPIN_SUCCESS, I_VGP_RPC_ID,
  I_VGP_CS_SUCCESS, I_VGP_CS_ESCAPE, I_VGP_CS_FLEE, I_VGP_CS_MISSED, I_VGP_CS_ERROR, I_VGP_CS_ITEM_REPL,
  PGR_OPT_INT_COUNT
};
static_assert(PGR_OPT_INT_COUNT == 29, "29 optional static enum ints");

inline constexpr size_t kFieldMetaMax = 96;
inline constexpr size_t kItemNameMax  = 512;
inline constexpr size_t kRaidTierMax  = 64;
inline constexpr size_t kTeamNameMax  = 32;
inline constexpr size_t kReasonMax    = 96;

} // namespace pgr_shared_meta

#ifdef __cplusplus
extern "C" {
#endif

// Per-subfeature availability codes (zero subfeatures are DELEGATED in API 2).
enum PgrSharedOptional {
  PGR_SHARED_OPT_DELEGATED   = 0,
  PGR_SHARED_OPT_AVAILABLE   = 1,
  PGR_SHARED_OPT_UNAVAILABLE = 2
};

// resolve() status (0 == OK). Negatives below -29 are this module's; others propagate feature-47
// PgrMetaStatus codes verbatim. Note: PGR_MD_ERR_NOT_FOUND (-9) is returned ONLY when assemblies are 0
// or a required class is not yet loaded internally; resolve maps that to NOT_READY (+1).
// Missing required members return -33 and are never treated as cold-start retry.
enum PgrSharedMetaStatus {
  PGR_SHARED_OK                   = 0,
  PGR_SHARED_NOT_READY            = 1,
  PGR_SHARED_ERR_NULL_ARG         = -1,
  PGR_SHARED_ERR_CONFIG           = -2,
  PGR_SHARED_ERR_ATTACH           = -30,
  PGR_SHARED_ERR_METADATA         = -31,
  PGR_SHARED_ERR_INTERNAL         = -32,
  PGR_SHARED_ERR_MEMBER_NOT_FOUND = -33,
  PGR_SHARED_ERR_GC_ABI           = -34
};

// Extra IL2CPP exports outside feature-47's 31 exports. The first 4 are mandatory for prepare()
// (CatchSuccess static read + pointerHandleAbi GC code fingerprint); the remaining 10 are optional
// and checked only by the optional subfeatures that use them.
struct PgrSharedMetaExtraExports {
  void *field_static_get_value;    //  0  mandatory: void  (void* field, void* out)
  void *gchandle_new;              //  1  mandatory: void* (void* obj, bool pinned) [code-inspected only]
  void *gchandle_get_target;       //  2  mandatory: void* (void* handle)           [code-inspected only]
  void *gchandle_free;             //  3  mandatory: void  (void* handle)           [code-inspected only]
  void *class_get_fields;          //  4  optional (items, mapPoi.extra): void* (void* klass, void** iter)
  void *field_get_name;            //  5  optional (items, mapPoi.extra): const char* (void* field)
  void *type_get_object;           //  6  optional (catchScene, items.lookup): void* (void* type)
  void *array_length;              //  7  optional (catchScene): size_t (void* array)
  void *method_get_object;         //  8  optional (items.lookup): void* (void* method, void* klass)
  void *runtime_class_init;        //  9  optional (items.lookup): void  (void* klass)
  void *class_get_parent;          // 10  optional (vgp): void* (void* klass)
  void *array_new;                 // 11  optional (vgp): void* (void* elem_klass, uint64_t len)
  void *gc_wbarrier_set_field;     // 12  optional (vgp): void  (void* obj, void* target_addr, void* val)
  void *method_get_from_reflection;// 13  optional (vgp): void* (void* reflection_method)
};
static_assert(sizeof(PgrSharedMetaExtraExports) == 112, "14 LP64 extra exports");

struct PgrSharedMetadataConfig {
  uint32_t version, size;      // == 2, == sizeof(PgrSharedMetadataConfig)
  PgrMetaBindConfig binding;   // 31 core exports + module range (+ optional fixture seg_query)
  void *thread_current;        // il2cpp_thread_current export (caller-attached; checked != null)
  uint64_t owner_tid;          // gettid() of the attached caller thread
  const PgrSharedMetaExtraExports *extra; // mandatory extra exports
};
static_assert(sizeof(PgrSharedMetadataConfig) == 96, "LP64 config (API 2)");

struct PgrSharedFieldMetaEntry {
  uint64_t offset;
  uint32_t kind;               // PgrMetaFieldKind (1=ref, 2=value, 3=enum-int32)
  uint32_t size;               // value size when kind==2, else 0
  char name[64];
  char owner[128];
};
static_assert(sizeof(PgrSharedFieldMetaEntry) == 208, "PgrSharedFieldMetaEntry size");

struct PgrSharedEnumEntry {
  int32_t value;
  char name[60];
};
static_assert(sizeof(PgrSharedEnumEntry) == 64, "PgrSharedEnumEntry size");

struct PgrSharedMetadataOutput {
  uint32_t version, size;                                                        // +0     (8B)
  const void *classes[pgr_shared_meta::kClassCount];                             // +8     (56B)
  const void *opt_classes[pgr_shared_meta::kOptClassCount];                      // +64    (1016B)
  const void *fields[pgr_shared_meta::kFieldCount];                              // +1080  (48B)
  const void *field_types[pgr_shared_meta::kFieldCount];                         // +1128  (48B)
  const void *field_type_classes[pgr_shared_meta::kFieldCount];                  // +1176  (48B)
  const void *methods[pgr_shared_meta::kMethodCount];                            // +1224  (56B)
  const void *method_code[pgr_shared_meta::kMethodCount];                        // +1280  (56B)
  int32_t success;                                                               // +1336  (4B)
  uint32_t caught_id_size;                                                       // +1340  (4B)
  int32_t optional[pgr_shared_meta::kOptionalCount];                             // +1344  (84B)
  int32_t optional_error[pgr_shared_meta::kOptionalCount];                       // +1428  (84B)
  char optional_reason[pgr_shared_meta::kOptionalCount][pgr_shared_meta::kReasonMax]; // +1512 (2016B)
  const void *opt_handles[pgr_shared_meta::PGR_OPT_HANDLE_COUNT];                // +3528  (4760B)
  int32_t opt_ints[pgr_shared_meta::PGR_OPT_INT_COUNT];                          // +8288  (116B)
  uint32_t field_meta_count;                                                     // +8404  (4B)
  PgrSharedFieldMetaEntry field_meta[pgr_shared_meta::kFieldMetaMax];            // +8408  (19968B)
  uint32_t item_name_count;                                                      // +28376 (4B)
  uint32_t raid_tier_count;                                                      // +28380 (4B)
  uint32_t team_name_count;                                                      // +28384 (4B)
  uint32_t enrichment_flags; // +28388: bit0 raid enum complete; bit1 team enum complete
  PgrSharedEnumEntry item_names[pgr_shared_meta::kItemNameMax];                  // +28392 (32768B)
  PgrSharedEnumEntry raid_tiers[pgr_shared_meta::kRaidTierMax];                  // +61160 (4096B)
  PgrSharedEnumEntry team_names[pgr_shared_meta::kTeamNameMax];                  // +65256 (2048B)
};
static_assert(sizeof(PgrSharedMetadataOutput) == 67304, "LP64 output (API 2 complete)");

struct PgrSharedMetadataApi {
  uint32_t version, size, config_size, output_size;
  int (*resolve)(const PgrSharedMetadataConfig *, PgrSharedMetadataOutput *);
};
static_assert(sizeof(PgrSharedMetadataApi) == 24, "LP64 API");

const void *pgrazor_shared_metadata_api(uint32_t version);

#ifdef __cplusplus
}
#endif
