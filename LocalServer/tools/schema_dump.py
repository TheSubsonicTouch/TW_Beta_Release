"""Print the client's own response schema out of Reflection.rdb.

    python schema_dump.py errands             # the job + challenge reply types
    python schema_dump.py list [substring]    # every reflected type name
    python schema_dump.py show "UFG::TrialPlayerProgress"
    python schema_dump.py find "GameService ErrandSlotData"

Reflection.rdb sits next to TriadWars.exe (the same folder as the LocalServer
folder). The server reads it at start-up when it is there; this tool is for looking
at it by hand, e.g. to confirm what a reply for a screen that was never emulated has
to contain. Without the file the server still runs - these dumps just say so.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..")))

import schema  # noqa: E402

INTERESTING = [
    "GameService ErrandSlotData",
    "GameService CompletedErrandSlotData",
    "UFG::TrialContainer",
    "UFG::TrialPlayerProgress",
    "UFG::TrialReward",
    "UFG::ErrandTypeMetadata",
    "UFG::ErrandRewards",
    "UFG::GameRewardTuner",
]


def main(argv):
    if not schema.loaded:
        print("no Reflection.rdb found; looked in:")
        for path in schema.CANDIDATES:
            print("  " + path)
        return 1
    if not argv or argv[0] == "errands":
        for name in INTERESTING:
            print(schema.describe(name))
            print()
        for element in ("GameService ErrandSlotData", "GameService CompletedErrandSlotData"):
            print("arrays of %s: %r" % (element, schema.SCHEMA.find_array_member(element)))
        return 0
    command = argv[0]
    if command == "list":
        pattern = argv[1].lower() if len(argv) > 1 else ""
        for name in sorted(schema.SCHEMA.types):
            if pattern in name.lower():
                print(name)
        return 0
    if command == "show":
        for name in argv[1:]:
            print(schema.describe(name))
            print()
        return 0
    if command == "find":
        for element in argv[1:]:
            print("%s:" % element)
            for cls, member in schema.SCHEMA.find_array_member(element):
                print("  %s.%s" % (cls, member))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
