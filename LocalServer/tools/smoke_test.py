"""Check that the local server is up and answering the way the client needs.

    python smoke_test.py [--port 443]

Read-only: it only sends GET requests. Exits 0 when every check passes, 1 otherwise.
Run it after starting the server and before starting the game. It also tells a real
Triad Wars server apart from some other program that happens to be listening on the port.
"""
import argparse
import json
import os
import ssl
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
# pack store -> the bundle store the client pairs it with (TriadWars.exe func 1407100c0)
PACK_TO_BUNDLE_STORE = {
    "17679317271361684130": "2541658861465094719",
    "1333401478411759499": "14396531704373406508",
    "6121745478303023351": "15645208897816992095",
    "6802491388125254368": "330971609030659674",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=443)
    args = ap.parse_args()
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE         # the server's certificate is self-signed

    def get(path):
        url = "https://127.0.0.1:%d/%s" % (args.port, path)
        with urllib.request.urlopen(url, context=ctx, timeout=10) as r:
            return json.loads(r.read())

    failed = []

    def check(name, fn):
        try:
            detail = fn()
            print("ok    %-28s %s" % (name, detail or ""))
        except Exception as e:                      # report every failure, then exit 1
            failed.append(name)
            print("FAIL  %-28s %r" % (name, e))

    def store_access():
        assert get("game/api/v1/storeaccess") == {"mHasAccess": True}

    def player():
        bags = get("game/api/v1/playerdata/serializedplayerdata/8191")
        for key in ("PlayerDataBag", "CurrencyBag", "TurfBag", "FavoursBag", "InventoryBag"):
            assert key in bags, "missing " + key          # bags sit at the ROOT of this reply
        pdb = bags["PlayerDataBag"]
        assert isinstance(pdb["PlayerDataID"], str) and isinstance(pdb["Face"], str)
        return "player %r, turf %s" % (pdb["Name"], bags["TurfBag"]["Turf"]["LocationName"])

    def rivals():
        r = get("game/api/v1/playerdata/opponents")
        assert len(r["mOpponents"]) == 7 and len(r["mOpponentDataBags"]) == 7
        return "7 rivals"

    def face_tiers():
        assert len(get("game/api/v1/playerdata/facetiers")) == 10
        return "10 tiers"

    def quotes():
        assert isinstance(get("game/api/v1/blackmarket/quotes"), list)

    def jobs():
        r = get("game/api/v1/errands")
        assert r["mReturnCode"] == 0 and isinstance(r["mErrands"], list)
        assert isinstance(r["mSecondsToRefresh"], int) and r["mSecondsToRefresh"] > 0
        assert len(r["mErrands"]) <= 3, r["mErrands"]
        for slot in r["mErrands"]:
            assert isinstance(slot["mSlotIndex"], int)
            assert slot["mErrandUID"].isdigit(), slot
            assert slot["mState"] in (0, 1, 2), slot
        return "%d job(s) on the board" % len(r["mErrands"])

    def job_history():
        r = get("game/api/v1/completederrands")
        assert isinstance(r.get("mErrands"), list), r
        return "%d completed job(s)" % len(r["mErrands"])

    def challenges():
        r = get("game/api/v1/trialsscreenvisits")
        progress = r["mPlayerProgress"]
        assert r["mReturnCode"] == 0 and isinstance(progress, dict)
        assert isinstance(progress.get("mProgress", 0), int)
        return "%s, %d/%d" % (progress.get("mTrial") or "no challenge",
                              progress.get("mProgress", 0), progress.get("mTarget", 0))

    def stores():
        with open(os.path.join(DATA, "store_items.json"), encoding="utf-8") as f:
            catalogues = json.load(f)
        served = {}
        for uid, cat in catalogues.items():
            data = get("commerce/api/v1/store/" + uid)["data"]
            assert data, "store %s (%s) is empty" % (uid, cat["name"])
            assert all(i["mCortexUID"].isdigit() for i in data)
            served[uid] = cat
        for pack_store, bundle_store in PACK_TO_BUNDLE_STORE.items():
            singles = [p for p in served[bundle_store]["packs"].values() if len(p) == 1]
            for pack in served[pack_store]["items"]:
                assert [pack] in singles, "pack %s has no 1x bundle in store %s" % (pack, bundle_store)
        return "%d stores filled, every pack has its 1x bundle" % len(served)

    check("server answers", store_access)
    check("player data", player)
    check("rivals", rivals)
    check("face tiers", face_tiers)
    check("black market quotes", quotes)
    check("jobs board", jobs)
    check("completed jobs", job_history)
    check("challenges", challenges)
    check("shop catalogues", stores)
    print("%d check(s) failed" % len(failed) if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
