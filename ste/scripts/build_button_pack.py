# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the button-prompt texture packs (face buttons) from the launcher's copy of Kenney "Input Prompts" (CC0).

usage: python -I build_button_pack.py --zip <launcher>\\assets\\glyphs\\kenney_input-prompts_1.5.zip --out <exe dir>\\texturepacks

Writes <out>\\button_prompts_xbox and <out>\\button_prompts_playstation (manifest.json with "button_style" + credits, <key16>.png files, USED_FILES.txt).
The pack is chosen at run time by STE_BUTTON_PROMPTS / [input] button_prompts (xbox_original = no pack = untouched game art).
Keys are the texture keys of the game's A/B/X/Y glyphs (docs\\BUTTON_PROMPTS_INVENTORY.md). Nothing here is game data: the art is Kenney's, the keys are 64-bit hashes.
"""
import argparse, hashlib, json, os, sys, zipfile

PINNED_SHA256 = "ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1"   # kenney_input-prompts_1.5.zip
KEYS = {"a": "bf84496567b098d2", "b": "63eeb3959c0a20a3", "x": "904ff629baa9fa11", "y": "aff4ae387fe77b23"}
ART = {
    "xbox": {k: "Xbox Series/Default/xbox_button_color_%s.png" % k for k in KEYS},
    "playstation": {"a": "PlayStation Series/Default/playstation_button_color_cross.png",
                    "b": "PlayStation Series/Default/playstation_button_color_circle.png",
                    "x": "PlayStation Series/Default/playstation_button_color_square.png",
                    "y": "PlayStation Series/Default/playstation_button_color_triangle.png"},
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    data = open(a.zip, "rb").read()
    sha = hashlib.sha256(data).hexdigest()
    if sha != PINNED_SHA256:
        sys.exit("zip SHA-256 %s differs from the pinned %s: refusing (add the new hash after checking the licence)" % (sha, PINNED_SHA256))
    z = zipfile.ZipFile(a.zip)
    for style, files in ART.items():
        d = os.path.join(a.out, "button_prompts_" + style)
        os.makedirs(d, exist_ok=True)
        used = []
        for k, src in files.items():
            open(os.path.join(d, KEYS[k] + ".png"), "wb").write(z.read(src))
            used.append("%s -> %s.png" % (src, KEYS[k]))
        man = {"id": "button_prompts." + style, "name": "Button prompts (%s)" % style, "version": "1", "profile": "textures", "enabled": False,
               "button_style": style, "priority": 50,
               "credits": "Kenney Input Prompts 1.5 (CC0 1.0, https://kenney.nl/assets/input-prompts), zip SHA-256 " + sha}
        open(os.path.join(d, "manifest.json"), "w").write(json.dumps(man, indent=2) + "\n")
        open(os.path.join(d, "USED_FILES.txt"), "w").write("source zip: kenney_input-prompts_1.5.zip sha256 %s\n" % sha + "\n".join(used) + "\n")
        print("wrote", d, len(used), "glyphs")


main()
