"""Fill mockup_template.dc.html with layout constants and asset URLs.

usage: python3 build_mockup.py <out.dc.html> local|blob [blobmap.json]
  local -> file:// paths for the Playwright 4K render
  blob  -> /_blob/<id> URLs from blobmap.json (asset name -> id) for the artifact
"""
import json, sys, pathlib

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import layout as L

out, mode = sys.argv[1], sys.argv[2]
RENDER = HERE / "render"
FILES = {
    "plate": RENDER / "plate_2k.jpg",
    "knob_body": RENDER / "knob_body.png", "knob_pointer": RENDER / "knob_pointer.png",
    "knob_big_body": RENDER / "knob_big_body.png", "knob_big_pointer": RENDER / "knob_big_pointer.png",
    "led_on": RENDER / "led_on.png", "led_off": RENDER / "led_off.png",
    "button_on": RENDER / "button_on.png", "button_off": RENDER / "button_off.png",
    "round_on": RENDER / "round_on.png", "fader_cap": RENDER / "fader_cap.png",
    "key_white": RENDER / "key_white.png", "key_white_down": RENDER / "key_white_down.png",
    "key_black": RENDER / "key_black.png", "key_black_down": RENDER / "key_black_down.png",
    "crown": pathlib.Path("/home/claude/render/crown.png"),
    "ghost": pathlib.Path("/home/claude/render/ghost.png"),
}
if mode == "local":
    FILES["plate"] = RENDER / "plate_4k.png"
    url = {k: "file://" + str(v) for k, v in FILES.items()}
else:
    ids = json.loads(pathlib.Path(sys.argv[3]).read_text())
    url = {k: "/_blob/" + ids[k] for k in FILES}

html = (HERE / "mockup_template.dc.html").read_text()
# longest names first so knob_big_body is not eaten by knob_b...
for k in sorted(url, key=len, reverse=True):
    html = html.replace("ASSET:" + k, url[k])
assert "ASSET:" not in html, "unmapped asset"

consts = (
    "const KNOB_POS = " + json.dumps(L.KNOBS) + ";\n"
    "const ROUND_POS = " + json.dumps(L.ROUND_BUTTONS) + ";\n"
    "const FADER_POS = " + json.dumps(L.FADER) + ";\n"
    "const KEYS_POS = " + json.dumps(L.KEYS) + ";\n"
)
html = html.replace("class Component extends DCLogic {", consts + "class Component extends DCLogic {", 1)
pathlib.Path(out).write_text(html)
print("wrote", out)
