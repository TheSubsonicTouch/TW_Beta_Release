// freecam_spawn_models.h - the character property sets the model list starts from.
//
// The executable names four of these itself (police, player, taxi_driver_faceaction and
// ThumbnailMan). The rest of the list is what "Rescan models" reads out of the game
// folder, which leaves two gaps: on a fresh install there is nothing in the list until
// that scan has run, and the people worth spawning - the rival crews - are in the list
// only if the scan happens to find each of them by name.
//
// So the ones the game's own data names are written down here: the gang families this
// build inherits from Sleeping Dogs, 18K, Sun On Yee, Water Street, the Dog Boys and the
// cult, with one of each fighting style per family, and the elite and boss variants. They
// are added first, ahead of FreeCam.npc.txt and the scan, so a rescan can never drop them.
//
// A name that a particular build does not know is not a crash and not a silent no-op: the
// spawner hashes it, looks it up, and answers "this build does not know '...'" without
// creating anything. The enemy flag is what the menu reads when one of these is picked to
// turn the spawn side to Hostile - a rival crew that arrives friendly is not what the
// name says, and the toggle in the menu stays the thing that decides it from then on.
#ifndef TW_FREECAM_SPAWN_MODELS_H
#define TW_FREECAM_SPAWN_MODELS_H

namespace tw {
namespace game {

struct SpawnModelName {
    const char *name;   // the property set, spelled the way the game spells it
    bool enemy;         // a rival crew: picking it turns the spawn side to Hostile
};

static const SpawnModelName kSpawnModels[] = {
    // The four the executable names itself, in the order the list has always started with.
    {"object-physical-character-police", false},
    {"object-physical-character-player", false},
    {"object-physical-character-taxi_driver_faceaction", false},
    {"object-physical-character-ThumbnailMan", false},

    // The rival crews. Each family ships one model per fighting style - brawler,
    // grappler, quick and striker - which is the fighting class the game's own gang table
    // lists, so one of each is here. The suffixed variants the game also ships (_amb,
    // _easy, _hard, _pistol, _NIS, _tutorial) are the same character set up for a
    // particular encounter, and are left out to keep the list walkable.
    {"object-physical-character-Thug_18k_brawler", true},
    {"object-physical-character-Thug_18k_grap", true},
    {"object-physical-character-Thug_18k_quick", true},
    {"object-physical-character-Thug_18k_striker", true},

    {"object-physical-character-Thug_Soy_brawler", true},
    {"object-physical-character-Thug_Soy_grap", true},
    {"object-physical-character-Thug_Soy_quick", true},
    {"object-physical-character-Thug_Soy_striker", true},

    {"object-physical-character-Thug_Wsg_brawler", true},
    {"object-physical-character-Thug_Wsg_grap", true},
    {"object-physical-character-Thug_Wsg_quick", true},
    {"object-physical-character-Thug_Wsg_striker", true},

    {"object-physical-character-Thug_Dbs_brawler", true},
    {"object-physical-character-Thug_Dbs_grap", true},
    {"object-physical-character-Thug_Dbs_quick", true},
    {"object-physical-character-Thug_Dbs_striker", true},

    {"object-physical-character-Thug_Cult_Brawler", true},
    {"object-physical-character-Thug_Cult_Grappler", true},
    {"object-physical-character-Thug_Cult_Quick", true},
    {"object-physical-character-Thug_Cult_Striker", true},

    {"object-physical-character-Thug_BsL_brawler", true},
    {"object-physical-character-Thug_BsL_grap", true},
    {"object-physical-character-Thug_BsL_quick", true},
    {"object-physical-character-Thug_BsL_striker", true},

    // The elite tier, the named rivals and the one-off characters that are still people
    // rather than story parts: these are the ones an encounter puts in the street, not a
    // mission set that is only complete in its own scene.
    {"object-physical-character-SDTWGangMembers-Enforcer", true},
    {"object-physical-character-SDTWGangMembers-EnforcerDrunken", true},
    {"object-physical-character-SDTWGangMembers-EnforcerMMA", true},
    {"object-physical-character-SDTWGangMembers-EnforcerMuayThai", true},
    {"object-physical-character-SDTWGangMembers-Thug_brawler", true},
    {"object-physical-character-SDTWGangMembers-Thug_elite", true},
    {"object-physical-character-SDTWGangMembers-Thug_grap", true},
    {"object-physical-character-SDTWGangMembers-Thug_quick", true},
    {"object-physical-character-SDTWGangMembers-Thug_striker", true},
    {"object-physical-character-ThugElite", true},
    {"object-physical-character-RivalEnforcer01", true},
    {"object-physical-character-BouncerBoss01", true},
    {"object-physical-character-80sTriad", true},
    {"object-physical-character-CultLeader", true},
    {"object-physical-character-DockWorkerThug", true},
};

// How many names the built-in table holds, for the callers that only need the count.
constexpr int kSpawnModelCount =
    static_cast<int>(sizeof(kSpawnModels) / sizeof(kSpawnModels[0]));

}  // namespace game
}  // namespace tw

#endif  // TW_FREECAM_SPAWN_MODELS_H
