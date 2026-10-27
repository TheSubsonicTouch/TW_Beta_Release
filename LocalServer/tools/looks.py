"""Build the outfit lookbook: named looks made of the game's own clothing pieces.

The client draws the player from `CustomizationBag.ClothingStoreItemUIDs` - a list of
`UFG::StoreClothing` UIDs - so any combination of pieces the game ships can be worn. The
wardrobe lets a player assemble one by hand; this file curates the good combinations and
gives each a name, so a look can be put on in one step.

    python looks.py                  rebuild data/looks.json from the table below
    python looks.py --list           print the lookbook
    python looks.py --check          validate the lookbook already on disk

Every piece is looked up in `devdata/gamedb.json` (the dump of the game's own data), so a
typo is a build error rather than a wrong UID in a profile. Because the client resolves
each UID against its own data, a look can only ever use pieces the game already knows -
which is what keeps these safe to wear.

A look is a *combination*, not a new item: no new UID is invented. That matters, because
a store tile with a UID the client has no record for is exactly the null-pointer class of
crash the rest of this server is built to avoid.

Slots come from the same data the wardrobe groups by (`mSubType`), and a look may hold at
most one piece per slot, so every look is a combination the wardrobe itself would allow.
"""
import argparse
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.normpath(os.path.join(HERE, "..", "devdata", "gamedb.json"))
OUT_PATH = os.path.normpath(os.path.join(HERE, "..", "data", "looks.json"))

THEMES = ["Gang", "Zombie", "Swat", "Tactical", "Suit", "Triad", "No clothes"]

# One entry per look: the pieces are (mPartName, mDisplayName) pairs from the game data.
# Where several UIDs share a display name they are tint variants of the same garment, and
# the lowest UID wins (deterministic, and they look the same).
LOOKS = [
    # ---- gang --------------------------------------------------------------------
    dict(id="corner-boy", name="Corner Boy", theme="Gang",
         blurb="Hood up, chain out, nothing in your pockets.",
         pieces=[("Grubby Hoodie", "Grubby Hoody"), ("JeansBaggy", "Baggy Jeans"),
                 ("Shell Toe Sneaker", "Shell Toe Sneaker"), ("H_Trucker", "Trucker Cap"),
                 ("N_GoldChain", "Gold Chain")]),
    dict(id="block-boss", name="Block Boss", theme="Gang",
         blurb="You own the corner and the corner knows it.",
         pieces=[("Designer Jacket", "Black Leather Jacket"),
                 ("P_Manhattan", "Manhattan Melee Outfit pants"),
                 ("White Top Sneakers", "White Tops"), ("N_ThickChain", "Thick Chain"),
                 ("W_MetalRolex", "Cambria Scallop Watch")]),
    dict(id="ride-or-die", name="Ride or Die", theme="Gang",
         blurb="Leather, boots and aviators. Mostly out of the way of the bike.",
         pieces=[("Motorcycle Jacket", "Motorcycle Jacket Original"),
                 ("P_Leather", "Leather Pants"), ("Fashionable Boot", "Fantom Zip Boots"),
                 ("G_Thin", "Aspirazone Aviators"), ("W_LeatherBands", "Leather Bands")]),
    dict(id="street-soldier", name="Street Soldier", theme="Gang",
         blurb="Cargo, boots and a bandana: everyone knows who you run with.",
         pieces=[("Hip Jacket", "Hipster Jacket"), ("P_Cargo", "Camo Cargo Pants"),
                 ("Worker Boots", "Worksite Boots"), ("G_BandanaMask", "Bandana Mask"),
                 ("H_Toque", "Slouchy Toque")]),
    dict(id="bam-bam", name="Bam Bam", theme="Gang",
         blurb="Baggy jeans, big kicks, bigger chain.",
         pieces=[("TShirt Printed", "Bam Bam Butterfly"), ("SuperBaggy", "Super Baggy Jeans"),
                 ("Kicks", "IKOZE Kicks"), ("H_BaseBall", "Baseball Cap"),
                 ("W_ThickChain", "Thick Gold Chain")]),
    dict(id="sun-on-yee", name="Sun On Yee", theme="Gang",
         blurb="White tee, clean sneakers - quiet support, loud loyalty.",
         pieces=[("TShirt Printed", "Sun On Yee - White"),
                 ("Straight Leg Jeans", "Straight Leg Jeans"),
                 ("Shell Toe Sneaker", "Shell Toe Sneaker"), ("H_Trucker", "Trucker Cap")]),

    # ---- zombie ------------------------------------------------------------------
    dict(id="fresh-zombie", name="Fresh Zombie", theme="Zombie",
         blurb="Just turned, still dressed. Smells exactly how you think.",
         pieces=[("T_TorsoZombie", "T_TorsoZombie - Tint [0]"),
                 ("P_JeansZombie", "P_JeansZombie - Tint [0]"),
                 ("Zombie Feet", "Zombie Feet - Tint [0]"),
                 ("H_HeadZombie", "H_HeadZombie - Tint [0]")]),
    dict(id="neon-zombie", name="Neon Zombie", theme="Zombie",
         blurb="The other zombie skin - brighter jeans, worse intentions.",
         pieces=[("T_TorsoZombie", "T_TorsoZombie - Set [ZombieNVidia] - Tint [0]"),
                 ("P_JeansZombie", "P_JeansZombie - Set [Zombie Jeans NVIDIA] - Tint [0]"),
                 ("Zombie Feet", "Zombie Feet - Tint [0]"),
                 ("H_HeadZombie", "H_HeadZombie - Tint [0]")]),
    dict(id="zombie-boss", name="Zombie Boss", theme="Zombie",
         blurb="Died rich. Kept the trousers and the chain.",
         pieces=[("T_TorsoZombie", "T_TorsoZombie - Tint [0]"),
                 ("P_SuitBloody", "Bloody Suit Pants"),
                 ("Zombie Feet", "Zombie Feet - Tint [0]"),
                 ("H_HeadZombie", "H_HeadZombie - Tint [0]"),
                 ("N_ThickChain", "Thick Chain")]),

    # ---- swat --------------------------------------------------------------------
    dict(id="swat-pointman", name="Swat Pointman", theme="Swat",
         blurb="First through the door, helmet down.",
         pieces=[("T_Swat", "T_Swat - Tint [0]"), ("P_Swat", "P_Swat - Tint [0]"),
                 ("S_Swat", "S_Swat - Tint [0]"), ("H_Swat", "H_Swat - Tint [0]")]),
    dict(id="swat-breacher", name="Swat Breacher", theme="Swat",
         blurb="The heavy suit: more plate, more helmet, less conversation.",
         pieces=[("T_SuperSwat", "T_SuperSwat - Tint [0]"),
                 ("P_SuperSwat", "P_SuperSwat - Tint [0]"),
                 ("S_SuperSwat", "S_SuperSwat - Tint [0]"),
                 ("H_SuperSwat", "H_SuperSwat - Tint [0]")]),
    dict(id="swat-urban", name="Swat Urban", theme="Swat",
         blurb="Swat plate over urban camo, respirator on. Riot weather.",
         pieces=[("T_Swat", "T_Swat - Tint [0]"),
                 ("P_Tactical", "P_Tactical - Set [CamoUrban] - Tint [0]"),
                 ("S_Swat", "S_Swat - Tint [0]"), ("H_Swat", "H_Swat - Tint [0]"),
                 ("G_Sars Variations", "Air Filter - Digital Camo")]),

    # ---- tactical ----------------------------------------------------------------
    dict(id="ghost", name="Ghost", theme="Tactical",
         blurb="Grey tactical cut, black balaclava. No name, no face.",
         pieces=[("T_Tactical", "T_Tactical - Tint [0]"),
                 ("P_Tactical", "P_Tactical - Tint [0]"),
                 ("S_Tactical", "S_Tactical - Tint [0]"),
                 ("H_Balaclava", "Plain Black Balaclava")]),
    dict(id="desert-operator", name="Desert Operator", theme="Tactical",
         blurb="Desert camo and a skull over the face. Contract work.",
         pieces=[("T_Tactical", "T_Tactical - Set [CamoDesertTorso] - Tint [0]"),
                 ("P_Tactical", "P_Tactical - Set [CamoDesert] - Tint [0]"),
                 ("S_Tactical", "S_Tactical - Tint [0]"),
                 ("H_Balaclava", "Balaclava - Full Skull")]),
    dict(id="night-ops", name="Night Ops", theme="Tactical",
         blurb="Urban camo and a gas mask - the city at 3am is a war zone.",
         pieces=[("T_Tactical", "T_Tactical - Set [CamoUrbanTorso] - Tint [0]"),
                 ("P_Tactical", "P_Tactical - Set [CamoUrban] - Tint [0]"),
                 ("S_Tactical", "S_Tactical - Tint [0]"),
                 ("H_GasMask", "Gas Mask")]),
    dict(id="clown-unit", name="Clown Unit", theme="Tactical",
         blurb="Digital pink camo and a clown mask. Nobody takes you seriously until they do.",
         pieces=[("T_Tactical", "T_Tactical - Set [CamoDigitalPinkTorso] - Tint [0]"),
                 ("P_Tactical", "P_Tactical - Set [CamoDigitalPink] - Tint [0]"),
                 ("S_Tactical", "S_Tactical - Tint [0]"),
                 ("H_Balaclava", "Balaclava - Clown")]),

    # ---- suit --------------------------------------------------------------------
    dict(id="boardroom", name="Boardroom", theme="Suit",
         blurb="Closed jacket, slacks, oxfords. Money that never gets wet.",
         pieces=[("Suit Jacket Closed", "Suit Jacket, closed"), ("Slacks", "Slacks"),
                 ("Oxfords", "Oxfords"), ("W_MetalRolex", "Cambria Scallop Watch")]),
    dict(id="undertaker", name="Undertaker", theme="Suit",
         blurb="Black suit, black shoes. You are not here to be liked.",
         pieces=[("Suit Jacket Funeral", "Suit Jacket Funeral - Set [Black Suit] - Tint [0]"),
                 ("P_SuitDress", "Suit Pants"),
                 ("Brogue Loafer", "Couronne Brogue Loafer")]),
    dict(id="blood-money", name="Blood Money", theme="Suit",
         blurb="The suit from the last job. Nobody asked why it is that colour.",
         pieces=[("Bloody Suit Jacket", "Bloody Suit Jacket"),
                 ("P_SuitBloody", "Bloody Suit Pants"),
                 ("Brogue Loafer", "Couronne Brogue Loafer"), ("N_GoldChain", "Gold Chain")]),
    dict(id="miami-vice", name="Miami Vice", theme="Suit",
         blurb="Jacket, no shirt, aviators, white shoes. Subtle as a brick.",
         pieces=[("T_SuitJacketShirtless", "Suit Jacket, Shirtless"),
                 ("P_SuitDress", "Suit Pants"), ("Square Toe Loafer", "Square Toe Loafer"),
                 ("G_Thin", "Aspirazone Aviators"), ("N_GoldChain", "Gold Chain")]),
    dict(id="suit-and-tee", name="Suit & Tee", theme="Suit",
         blurb="Jacket over a tee, business casual below, ink where the collar ends.",
         pieces=[("T_SuitJacketTshirt", "Suit Jacket with T-Shirt"),
                 ("P_SuitDress_Style", "Business Casual"), ("Oxfords", "Oxfords"),
                 ("Phoenix Tattoo", "Phoenix Tattoo")]),
    dict(id="white-suit", name="White Suit", theme="Suit",
         blurb="White jacket, white sneakers, jade at the throat. Loud without shouting.",
         pieces=[("Suit Jacket Funeral", "Suit Jacket Funeral - Set [White Suit] - Tint [0]"),
                 ("P_SuitDress", "Suit Pants"), ("White Top Sneakers", "White Tops"),
                 ("N_Jade", "Jade Pendant")]),

    # ---- triad -------------------------------------------------------------------
    dict(id="red-pole", name="Red Pole", theme="Triad",
         blurb="Shirtless jacket, dragon print, pointed toes. The one who gives the orders.",
         pieces=[("T_SuitJacketShirtless", "Suit Jacket, Shirtless"),
                 ("P_JeansDesigner", "Dragon Print"), ("Fancy Pointed Toe", "Fancy Pointed Toe"),
                 ("N_BeadDragon", "Bead Dragon")]),
    dict(id="temple-guardian", name="Temple Guardian", theme="Triad",
         blurb="Monk's top and trousers, cloth shoes, bronze at the wrist.",
         pieces=[("T_Monk", "T_Monk - Tint [0]"), ("P_Monk", "P_Monk - Tint [0]"),
                 ("S_KungFu", "S_KungFu"), ("W_KungFuMaster_Bracelet", "W_KungFuMaster_Bracelet - Tint [0]")]),
    dict(id="jade-lotus", name="Jade Lotus", theme="Triad",
         blurb="Dancer silks and a jade pendant. Every step is a threat.",
         pieces=[("T_KungFuDancer", "T_KungFuDancer - Tint [0]"),
                 ("P_KungFuDancer", "P_KungFuDancer"), ("S_KungFuDancer", "S_KungFuDancer"),
                 ("N_Jade", "Jade Pendant")]),
    dict(id="tiger-style", name="Tiger Style", theme="Triad",
         blurb="Peasant top ripped at the shoulders, Guan Yu down the arm.",
         pieces=[("T_KungFuPeasant", "T_KungFuPeasant - Tint [0]"),
                 ("P_KungFuRipped", "P_KungFuRipped"), ("S_KungFuRipped", "S_KungFuRipped"),
                 ("Guan Yu Tattoo", "Guan Yu Tattoo")]),
    dict(id="flying-dagger", name="Flying Dagger", theme="Triad",
         blurb="Wing Chun top, master's trousers, hair up. Hands do the talking.",
         pieces=[("T_WingChung", "T_WingChung - Tint [0]"),
                 ("P_KungFuMaster", "P_KungFuMaster"), ("S_KungFuMaster", "S_KungFuMaster"),
                 ("H_Hair_FauxHawk", "Pompadour")]),
    dict(id="monkey-lord", name="Monkey Lord", theme="Triad",
         blurb="The king's mask and top, wushu below. Chaos, styled.",
         pieces=[("T_MonkeyKing", "T_MonkeyKing - Tint [0]"), ("P_Wushu", "P_Wushu"),
                 ("S_Wushu", "S_Wushu"), ("M_MonkeyKing", "M_MonkeyKing - Tint [0]")]),
    dict(id="quiet-sage", name="Quiet Sage", theme="Triad",
         blurb="Vintage silk top, monk's trousers, grey hair. Old money, older hands.",
         pieces=[("T_ShenZen", "T_ShenZen - Tint [0]"), ("P_Monk", "P_Monk - Tint [0]"),
                 ("Martial Arts Sneaker", "Shoes_MartialArts"),
                 ("H_Hair_Grey", "Touch of Grey")]),

    # ---- second wave: the looks added after the first lookbook --------------------
    dict(id="dockside-crew", name="Dockside Crew", theme="Gang",
         blurb="Puffer vest, work boots, hands in your pockets. The docks remember you.",
         pieces=[("Puffer Vest", "Puffy Vest"), ("P_Grubby", "Grubby"),
                 ("Worker Boots", "Worksite Boots"), ("H_Toque", "Striped Toque"),
                 ("G_Disguise", "Disguise!")]),
    dict(id="wolf-pack", name="Wolf Pack", theme="Gang",
         blurb="Leather, dark denim, boots, face covered. Ride in, ride out.",
         pieces=[("Leather Jacket", "Leather Jacket"), ("Douche Jeans", "Douche Jeans"),
                 ("Classic Boot", "Combat Boots"), ("H_Balaclava", "Balaclava - Puma"),
                 ("W_LeatherBands", "Leather Bands")]),
    dict(id="zombie-riot", name="Zombie Riot", theme="Zombie",
         blurb="One of the dead, still in tactical trousers, respirator over the jaw.",
         pieces=[("T_TorsoZombie", "T_TorsoZombie - Tint [0]"),
                 ("P_Tactical", "P_Tactical - Set [CamoUrban] - Tint [0]"),
                 ("Zombie Feet", "Zombie Feet - Tint [0]"),
                 ("H_HeadZombie", "H_HeadZombie - Tint [0]"),
                 ("G_Sars", "Air Filter - Old")]),
    dict(id="zombie-biker", name="Zombie Biker", theme="Zombie",
         blurb="Dead, leather trousers, still wearing the wristbands from the crash.",
         pieces=[("T_TorsoZombie", "T_TorsoZombie - Tint [0]"),
                 ("P_Leather", "Leather Pants"),
                 ("Zombie Feet", "Zombie Feet - Tint [0]"),
                 ("H_HeadZombie", "H_HeadZombie - Tint [0]"),
                 ("W_LeatherBands", "Leather Bands")]),
    dict(id="swat-nightwatch", name="Swat Nightwatch", theme="Swat",
         blurb="Swat plate and boots, super swat helmet, aviators under the visor.",
         pieces=[("T_Swat", "T_Swat - Tint [0]"), ("P_Swat", "P_Swat - Tint [0]"),
                 ("S_Swat", "S_Swat - Tint [0]"),
                 ("H_SuperSwat", "H_SuperSwat - Tint [0]"),
                 ("G_Thin", "Aspirazone Aviators")]),
    dict(id="consigliere", name="Consigliere", theme="Suit",
         blurb="Black suit, brogues, glasses. You do the talking, someone else does the rest.",
         pieces=[("Suit Jacket Closed", "Suit Jacket, closed"), ("Slacks", "Slacks"),
                 ("Brogue Loafer", "Couronne Brogue Loafer"),
                 ("G_Dogs", "Classic Couronne"), ("W_MetalRound", "Couronne Speedster Watch")]),
    dict(id="golden-koi", name="Golden Koi", theme="Triad",
         blurb="Golden koi on the chest, silk below, jade at the wrist.",
         pieces=[("TShirt Printed", "Golden Koi - Black"), ("P_ShenZen", "P_ShenZen - Tint [0]"),
                 ("Martial Arts Sneaker", "Shoes_MartialArts"), ("N_Jade", "Jade Pendant")]),
    dict(id="bare-minimum", name="Bare Minimum", theme="No clothes",
         blurb="Green plaid boxers and a bracelet. It's a look.",
         pieces=[("P_UWBoxers", "Boxers: Green Plaid"),
                 ("Digital Bracelet", "Digital Bracelet")]),

    # ---- no clothes --------------------------------------------------------------
    dict(id="bare-essentials", name="Bare Essentials", theme="No clothes",
         blurb="Boxers. That is the outfit.",
         pieces=[("P_UWBoxers", "Boxers: Plain")]),
    dict(id="full-monty", name="Full Monty", theme="No clothes",
         blurb="The game's own bare-legs mesh, and nothing else at all.",
         pieces=[("Legs", "Bare Legs DEBUG ONLY")]),
    dict(id="bare-knuckle", name="Bare Knuckle", theme="No clothes",
         blurb="Boxers, both fists taped, one chain. Ready for the cage.",
         pieces=[("P_UWBoxers", "Boxers: Plain"), ("W_MMA_Right", "W_MMA_Right - Tint [0]"),
                 ("W_MMA_left", "W_MMA_left - Tint [0]"), ("N_GoldChain", "Gold Chain")]),
]

# At most one piece per slot. These are the wardrobe's own slots (its tree is built from
# mSubType), so a look never asks for two hats, two trousers, and so on.
MAX_PER_SLOT = {"torso": 1, "legs": 1, "feet": 1, "head": 1, "mask": 1, "glasses": 1,
                "neck": 1, "hair": 1, "face": 1, "beard": 1, "tattoo": 1,
                "wrist_left": 1, "wrist_right": 1, "accessory": 1}

_ACCESSORY_SLOTS = {"Glasses": "glasses", "Masks": "mask", "Neck": "neck",
                    "Left Wrist": "wrist_left", "Right Wrist": "wrist_right"}
_BODY_SLOTS = {"Hair": "hair", "Face": "face", "Facial Hair": "beard", "Tattoo": "tattoo"}
_PREFIX_SLOTS = [("T_", "torso"), ("P_", "legs"), ("S_", "feet"), ("H_", "head"),
                 ("G_", "glasses"), ("M_", "mask"), ("W_", "wrist_right")]


# The game names its own outfit pieces after the part they replace ("T_Swat - Tint [0]").
# These are the same pieces, said the way a wardrobe would say them.
PRETTY = {
    "T_TorsoZombie": "Zombie Torso", "P_JeansZombie": "Zombie Jeans",
    "H_HeadZombie": "Zombie Head", "Zombie Feet": "Zombie Feet",
    "T_Swat": "Swat Armour", "P_Swat": "Swat Trousers", "S_Swat": "Swat Boots",
    "H_Swat": "Swat Helmet",
    "T_SuperSwat": "Super Swat Armour", "P_SuperSwat": "Super Swat Trousers",
    "S_SuperSwat": "Super Swat Boots", "H_SuperSwat": "Super Swat Helmet",
    "T_Tactical": "Tactical Jacket", "P_Tactical": "Tactical Trousers",
    "S_Tactical": "Tactical Boots",
    "T_Monk": "Monk's Robe", "P_Monk": "Monk's Trousers", "S_KungFu": "Cloth Shoes",
    "T_KungFuDancer": "Dancer's Silks", "P_KungFuDancer": "Dancer's Trousers",
    "S_KungFuDancer": "Dancer's Shoes",
    "T_KungFuPeasant": "Peasant Tunic", "P_KungFuRipped": "Ripped Trousers",
    "S_KungFuRipped": "Ripped Shoes",
    "T_WingChung": "Wing Chun Top", "P_KungFuMaster": "Master's Trousers",
    "S_KungFuMaster": "Master's Shoes",
    "T_MonkeyKing": "Monkey King Top", "P_MonkeyKing": "Monkey King Trousers",
    "S_MonkeyKing": "Monkey King Boots", "M_MonkeyKing": "Monkey King Mask",
    "T_ShenZen": "Shen Zen Silk Top", "T_Wushu": "Wushu Top", "P_Wushu": "Wushu Trousers",
    "S_Wushu": "Wushu Shoes",
    "W_MMA_Right": "Taped Fist (right)", "W_MMA_left": "Taped Fist (left)",
    "W_KungFuMaster_Bracelet": "Master's Bracelet",
    "Martial Arts Sneaker": "Martial Arts Sneaker", "P_Manhattan": "Manhattan Melee Pants",
}


def pretty_name(part, display):
    """'T_Tactical - Set [CamoUrbanTorso] - Tint [0]' -> 'Tactical Jacket (Camo Urban)'."""
    def tidy(tag):
        tag = re.sub(r"Torso$", "", tag.strip())
        return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", tag)

    name = PRETTY.get(part)
    if name is not None:
        match = re.search(r"Set \[([^\]]+)\]", display)
        return name + (" (%s)" % tidy(match.group(1)) if match else "")
    # 'Suit Jacket Funeral - Set [Black Suit] - Tint [0]' -> 'Suit Jacket Funeral (Black Suit)'
    match = re.search(r"\s*-\s*Set \[([^\]]+)\]", display)
    base = (display[:match.start()] if match else display)
    base = re.sub(r"\s*-\s*Tint \[\d+\]$", "", base).strip()
    return base + (" (%s)" % tidy(match.group(1)) if match else "")


def slot_of(record):
    """Which wardrobe slot a StoreClothing piece fills, from its own mSubType."""
    parts = (record.get("mSubType") or "").split("|")
    part = record.get("mPartName") or ""
    category = parts[0]
    sub = parts[1] if len(parts) > 1 else ""
    if category == "Tops" or category == "Chest":
        return "torso"
    if category == "Clothing":                  # Clothing|Chest|Swag, Clothing|Hat
        return "head" if sub == "Hat" else "torso"
    if category == "Pants":
        return "legs"
    if category == "Shoes":
        return "feet"
    if category == "Hats":
        return "head"
    if category == "Body":
        return _BODY_SLOTS.get(sub, "face")
    if category == "Accessories":
        return _ACCESSORY_SLOTS.get(sub, "accessory")
    # Outfit-only pieces carry no category; group them by the part they replace.
    if part == "Legs" or part == "Zombie Feet":
        return "legs" if part == "Legs" else "feet"
    for prefix, slot in _PREFIX_SLOTS:
        if part.startswith(prefix):
            if part == "W_MMA_left":
                return "wrist_left"
            return slot
    return "accessory"


def _index(clothing):
    index = collections.defaultdict(list)
    for record in clothing:
        index[_key(record)].append(record)
    return index


def _key(record):
    """(part, display) with the stray spaces some records carry removed."""
    return ((record.get("mPartName") or "").strip(), (record.get("mDisplayName") or "").strip())


def build(db_path=DB_PATH, out_path=OUT_PATH, verbose=True):
    """Resolve LOOKS against the game data, validate, and write the lookbook."""
    with open(db_path, encoding="utf-8") as f:
        db = json.load(f)
    index = _index(db["UFG::StoreClothing"])
    looks, problems = [], []
    for spec in LOOKS:
        pieces, slots = [], collections.Counter()
        for part, display in spec["pieces"]:
            hits = sorted(index.get((part.strip(), display.strip())) or [],
                          key=lambda c: int(c["$uid"]))
            if not hits:
                near = sorted({d for (p, d) in index if p == part})
                problems.append("%s: no piece %r / %r (part has: %s)"
                                % (spec["id"], part, display, ", ".join(near[:6]) or "nothing"))
                continue
            record = hits[0]
            slot = slot_of(record)
            slots[slot] += 1
            if slots[slot] > MAX_PER_SLOT.get(slot, 1):
                problems.append("%s: two pieces in the %r slot (%s)"
                                % (spec["id"], slot, record.get("mDisplayName")))
            display = record.get("mDisplayName") or ""
            pieces.append({"uid": record["$uid"], "name": pretty_name(part, display),
                           "data_name": display, "part": part, "slot": slot,
                           "category": record.get("mSubType") or ""})
        uids = [p["uid"] for p in pieces]
        if len(set(uids)) != len(uids):
            problems.append("%s: the same piece twice" % spec["id"])
        if not pieces:
            problems.append("%s: no pieces" % spec["id"])
        looks.append(dict(id=spec["id"], name=spec["name"], theme=spec["theme"],
                          blurb=spec["blurb"], pieces=pieces))
    seen = collections.Counter(l["id"] for l in looks)
    problems += ["duplicate look id %r" % k for k, n in seen.items() if n > 1]
    ids = [l["id"] for l in looks]
    order = {t: i for i, t in enumerate(THEMES)}
    looks.sort(key=lambda l: (order.get(l["theme"], len(order)), ids.index(l["id"])))
    if problems:
        print("\n".join(problems), file=sys.stderr)
        raise SystemExit("%d problem(s); %s not written" % (len(problems), out_path))
    data = {
        "note": "Named looks made of the game's own UFG::StoreClothing pieces. "
                "Generated by tools/looks.py - edit the table there, not this file.",
        "themes": THEMES,
        "looks": looks,
    }
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    if verbose:
        print("%s: %d looks, %d pieces, %d distinct pieces"
              % (out_path, len(looks), sum(len(l["pieces"]) for l in looks),
                 len({p["uid"] for l in looks for p in l["pieces"]})))
        for theme in THEMES:
            names = [l["name"] for l in looks if l["theme"] == theme]
            print("  %-11s %d  %s" % (theme, len(names), ", ".join(names)))
    return data


def check(path=OUT_PATH, db_path=DB_PATH):
    """Validate the lookbook on disk against the game data. Returns a list of problems."""
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    with open(db_path, encoding="utf-8") as f:
        db = json.load(f)
    known = {record["$uid"]: record for record in db["UFG::StoreClothing"]}
    problems = []
    for look in data["looks"]:
        slots = collections.Counter()
        if not look["pieces"]:
            problems.append("%s: no pieces" % look["id"])
        for piece in look["pieces"]:
            record = known.get(piece["uid"])
            if record is None:
                problems.append("%s: %s is not a UFG::StoreClothing the game knows"
                                % (look["id"], piece["uid"]))
                continue
            slot = slot_of(record)
            slots[slot] += 1
            if slots[slot] > MAX_PER_SLOT.get(slot, 1):
                problems.append("%s: two pieces in the %r slot" % (look["id"], slot))
        if len({p["uid"] for p in look["pieces"]}) != len(look["pieces"]):
            problems.append("%s: the same piece twice" % look["id"])
    return problems


def load(path=OUT_PATH):
    with open(path, encoding="utf-8") as f:
        return json.load(f)["looks"]


def describe(look):
    """'Grubby Hoody + Baggy Jeans + Shell Toe Sneaker' - the pieces, in slot order."""
    order = ["torso", "legs", "feet", "head", "mask", "face", "hair", "beard",
             "glasses", "neck", "wrist_left", "wrist_right", "tattoo", "accessory"]
    pieces = sorted(look["pieces"], key=lambda p: order.index(p["slot"])
                    if p["slot"] in order else len(order))
    return " + ".join(p["name"] for p in pieces)


def print_book(path=OUT_PATH):
    looks = load(path)
    width = max(len(l["name"]) for l in looks)
    for theme in THEMES:
        group = [l for l in looks if l["theme"] == theme]
        if not group:
            continue
        print("\n%s" % theme.upper())
        for look in group:
            print("  %-*s  %s" % (width, look["name"], describe(look)))
    print("\n%d looks. Wear one with: python playermodel.py set <name>" % len(looks))


def main():
    ap = argparse.ArgumentParser(description="Build the outfit lookbook.")
    ap.add_argument("--check", action="store_true", help="validate data/looks.json only")
    ap.add_argument("--list", action="store_true", help="print the lookbook")
    ap.add_argument("--out", default=OUT_PATH)
    ap.add_argument("--db", default=DB_PATH)
    args = ap.parse_args()
    if args.check:
        problems = check(args.out, args.db)
        if problems:
            print("\n".join(problems), file=sys.stderr)
            return 1
        print("%s: ok (%d looks)" % (args.out, len(load(args.out))))
        return 0
    if args.list:
        print_book(args.out)
        return 0
    build(args.db, args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
