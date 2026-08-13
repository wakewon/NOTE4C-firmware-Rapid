"""Repeatable A/B runs.

For every (image, recipe) pair this writes the device payload, two PC previews
and a JSON sidecar holding the full parameter set, the palette profile that
produced it and the objective metrics. Nothing about a result is implicit: a
``.bin`` can always be traced back to the exact recipe that made it.

The contact sheet is the part you actually use. It puts the candidates
side by side at matched scale, and lets you flip between the honest per-pixel
render and the eye-simulated one, because a halftone judged at 1:1 on a
backlit monitor tells you almost nothing about how it reads on paper.
"""

from __future__ import annotations

import html
import json
import time
import urllib.request
from pathlib import Path

from .palette import PaletteProfile
from .pipeline import Recipe, convert, write_outputs


def run_matrix(
    images: list[str | Path],
    recipes: list[Recipe],
    out_dir: str | Path,
    *,
    render_profile: str = "note4c-measured-v1",
    preview_scale: int = 1,
    verbose: bool = True,
) -> dict:
    """Convert every image with every recipe and build the contact sheet.

    ``render_profile`` is the panel's measured appearance. Every preview and
    every metric in the run uses it, whatever palette an individual recipe was
    built against, so the comparison is against one physical reality.
    """
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    profiles: dict[str, PaletteProfile] = {}
    render = PaletteProfile.load(render_profile)
    manifest = {
        "generated": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "format": "bwry2bpp (400x300, 2bpp, 30000 bytes)",
        "render_profile": render.name,
        "images": [],
    }

    for image in images:
        image = Path(image)
        stem = image.stem
        image_dir = out_dir / stem
        image_dir.mkdir(parents=True, exist_ok=True)
        entry = {"source": str(image), "name": stem, "results": []}

        for recipe in recipes:
            if recipe.profile not in profiles:
                profiles[recipe.profile] = PaletteProfile.load(recipe.profile)
            t0 = time.perf_counter()
            result = convert(image, recipe, profiles[recipe.profile], render_profile=render)
            elapsed = time.perf_counter() - t0

            sidecar = write_outputs(result, image_dir, recipe.name, preview_scale=preview_scale)
            sidecar["elapsed_seconds"] = round(elapsed, 3)
            (image_dir / f"{recipe.name}.json").write_text(
                json.dumps(sidecar, indent=2, ensure_ascii=False) + "\n"
            )

            entry["results"].append(
                {
                    "recipe": recipe.name,
                    "description": recipe.description,
                    "profile": recipe.profile,
                    "algorithm": "legacy-floyd-steinberg" if recipe.legacy else recipe.dither.algorithm,
                    "serpentine": False if recipe.legacy else recipe.dither.serpentine,
                    "bin": f"{stem}/{recipe.name}.bin",
                    "preview": f"{stem}/{recipe.name}_preview.png",
                    "sim": f"{stem}/{recipe.name}_sim.png",
                    "sidecar": f"{stem}/{recipe.name}.json",
                    "metrics": sidecar.get("metrics", {}),
                    "elapsed_seconds": round(elapsed, 3),
                }
            )
            if verbose:
                m = sidecar.get("metrics", {})
                print(
                    f"  {stem:<24} {recipe.name:<28} "
                    f"dE={m.get('hvs_delta_e', {}).get('mean', 0):5.2f}  "
                    f"confetti={m.get('spurious_chroma', {}).get('rate_in_neutral', 0):7.4f}  "
                    f"aniso={m.get('texture_anisotropy', 0):6.4f}  "
                    f"{elapsed:5.2f}s"
                )

        manifest["images"].append(entry)

    manifest["profiles"] = {name: p.to_dict() for name, p in profiles.items()}
    manifest["profiles"].setdefault(render.name, render.to_dict())
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    (out_dir / "index.html").write_text(build_contact_sheet(manifest))
    return manifest


# --------------------------------------------------------------------------
# Device push
# --------------------------------------------------------------------------


def push_to_device(base_url: str, payload: bytes, title: str = "", timeout: float = 20.0) -> dict:
    """POST a 2bpp payload to the device's LAN upload endpoint.

    This writes to the device's photo album. It is deliberately not part of
    ``run_matrix``; call it only when you actually want the panel updated.
    """
    base = base_url.rstrip("/")
    req = urllib.request.Request(
        f"{base}/upload?format=bwry2bpp",
        data=payload,
        headers={"Content-Type": "application/octet-stream"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = json.loads(resp.read().decode("utf-8"))

    if title and body.get("id"):
        meta = json.dumps({"id": body["id"], "title": title, "body": f"A/B: {title}"}).encode()
        meta_req = urllib.request.Request(
            f"{base}/photo/meta", data=meta, headers={"Content-Type": "application/json"}, method="POST"
        )
        with urllib.request.urlopen(meta_req, timeout=timeout):
            pass
    return body


# --------------------------------------------------------------------------
# Contact sheet
# --------------------------------------------------------------------------

_CSS = """
:root { color-scheme: light dark; --bg:#f6f6f4; --fg:#16161a; --card:#fff; --line:#dcdcd6; --muted:#6b6b73; }
@media (prefers-color-scheme: dark) { :root { --bg:#131316; --fg:#ececed; --card:#1c1c20; --line:#33333a; --muted:#9a9aa4; } }
* { box-sizing: border-box; }
body { margin:0; padding:24px; background:var(--bg); color:var(--fg);
       font:14px/1.5 ui-sans-serif,-apple-system,"Segoe UI",Roboto,"PingFang SC","Microsoft YaHei",sans-serif; }
h1 { font-size:20px; margin:0 0 4px; }
h2 { font-size:16px; margin:32px 0 12px; padding-top:16px; border-top:1px solid var(--line); }
.sub { color:var(--muted); margin:0 0 20px; }
.bar { position:sticky; top:0; z-index:10; background:var(--bg); padding:12px 0; margin-bottom:8px;
       border-bottom:1px solid var(--line); display:flex; gap:16px; flex-wrap:wrap; align-items:center; }
.bar label { display:flex; gap:6px; align-items:center; cursor:pointer; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(420px,1fr)); gap:16px; }
.card { background:var(--card); border:1px solid var(--line); border-radius:10px; overflow:hidden; }
.card img { display:block; width:100%; height:auto; image-rendering:pixelated; background:#fff; }
.card.zoom2 img { width:200%; max-width:none; }
.card.zoom2 { overflow:auto; }
.name { font-weight:600; padding:10px 12px 2px; }
.desc { color:var(--muted); padding:0 12px 8px; font-size:12.5px; }
.m { display:flex; flex-wrap:wrap; gap:4px 14px; padding:0 12px 12px; font-size:12px; color:var(--muted);
     font-family:ui-monospace,SFMono-Regular,Menlo,monospace; }
.m b { color:var(--fg); font-weight:600; }
.best { color:#0a7d34; font-weight:700; }
@media (prefers-color-scheme: dark) { .best { color:#4ade80; } }
.boost img { filter:contrast(1.55) brightness(1.06); }
a { color:inherit; }
table { border-collapse:collapse; font-size:12.5px; width:100%; margin-top:8px; }
th,td { text-align:left; padding:4px 10px 4px 0; border-bottom:1px solid var(--line); white-space:nowrap; }
th { color:var(--muted); font-weight:600; }
td.num { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; }
.legend { color:var(--muted); font-size:12px; margin:8px 0 4px; max-width:80ch; }
.legend b { color:var(--fg); font-weight:600; }
"""

_JS = """
const sheet = document.body;
function apply() {
  const mode = document.querySelector('input[name=view]:checked').value;
  document.querySelectorAll('.card img').forEach(img => {
    img.src = mode === 'sim' ? img.dataset.sim : img.dataset.preview;
  });
  sheet.classList.toggle('boost', document.getElementById('boost').checked);
  const z = document.getElementById('zoom').checked;
  document.querySelectorAll('.card').forEach(c => c.classList.toggle('zoom2', z));
}
document.querySelectorAll('.bar input').forEach(el => el.addEventListener('change', apply));
apply();
"""


def _metric_cells(metrics: dict) -> str:
    de = metrics.get("hvs_delta_e", {})
    ref = metrics.get("hvs_delta_e_vs_reference", {})
    sc = metrics.get("spurious_chroma", {})
    ink = metrics.get("ink_usage", {})
    retain = metrics.get("source_colour_retention", {})
    excess = metrics.get("native_colour_excess", {})
    return (
        f"<span>dE <b>{de.get('mean', 0):.2f}</b></span>"
        f"<span>dE/ref <b>{ref.get('mean', 0):.2f}</b></span>"
        f"<span>confetti <b>{sc.get('rate_in_neutral', 0) * 100:.2f}%</b></span>"
        f"<span>aniso <b>{metrics.get('texture_anisotropy', 0):.3f}</b></span>"
        f"<span>colour keep <b>{retain.get('score', 0) * 100:.1f}%</b></span>"
        f"<span>native +C <b>{excess.get('mean_excess_chroma', 0):.1f}</b></span>"
        f"<span>R <b>{ink.get('red', 0) * 100:.1f}%</b> Y <b>{ink.get('yellow', 0) * 100:.1f}%</b></span>"
    )


def _summary_table(results: list[dict]) -> str:
    def best(key_fn, lower_is_better=True):
        vals = [(key_fn(r), r["recipe"]) for r in results]
        vals = [v for v in vals if v[0] is not None]
        if not vals:
            return None
        return (min if lower_is_better else max)(vals)[1]

    b_de = best(lambda r: r["metrics"].get("hvs_delta_e", {}).get("mean"))
    b_cf = best(lambda r: r["metrics"].get("spurious_chroma", {}).get("rate_in_neutral"))
    b_an = best(lambda r: r["metrics"].get("texture_anisotropy"))

    rows = []
    for r in results:
        m = r["metrics"]
        de = m.get("hvs_delta_e", {}).get("mean", 0)
        ref = m.get("hvs_delta_e_vs_reference", {}).get("mean", 0)
        cf = m.get("spurious_chroma", {}).get("rate_in_neutral", 0) * 100
        an = m.get("texture_anisotropy", 0)
        rows.append(
            "<tr>"
            f"<td>{html.escape(r['recipe'])}</td>"
            f"<td>{html.escape(r['algorithm'])}{' serp' if r['serpentine'] else ''}</td>"
            f"<td class='num{' best' if r['recipe'] == b_de else ''}'>{de:.2f}</td>"
            f"<td class='num'>{ref:.2f}</td>"
            f"<td class='num{' best' if r['recipe'] == b_cf else ''}'>{cf:.2f}%</td>"
            f"<td class='num{' best' if r['recipe'] == b_an else ''}'>{an:.3f}</td>"
            f"<td class='num'>{r['elapsed_seconds']:.2f}s</td>"
            f"<td><a href='{html.escape(r['bin'])}'>bin</a> · <a href='{html.escape(r['sidecar'])}'>json</a></td>"
            "</tr>"
        )
    return (
        "<table><tr><th>recipe</th><th>algorithm</th><th>dE self</th><th>dE ref</th>"
        "<th>confetti</th><th>anisotropy</th><th>time</th><th>files</th></tr>"
        + "".join(rows)
        + "</table>"
        + "<p class='legend'><b>dE self</b> how closely the halftone hit its own tone-mapped "
        "target &mdash; a measure of the ditherer, only comparable between recipes that share a "
        "tone curve. <b>dE ref</b> distance from a plain colorimetric rendering of the source; a "
        "deliberate tone choice raises it, so it is a description, not a score. "
        "<b>confetti</b> share of neutral source area that got red or yellow ink &mdash; lower is "
        "always better. <b>anisotropy</b> directionality of the halftone texture; worms and grids "
        "raise it, blue noise drives it toward zero.</p>"
    )


def build_contact_sheet(manifest: dict) -> str:
    parts = [
        "<!doctype html><html><head><meta charset='utf-8'>",
        "<meta name='viewport' content='width=device-width,initial-scale=1'>",
        "<title>Note4C B/W/R/Y A/B</title>",
        f"<style>{_CSS}</style></head><body>",
        "<h1>Note4C B/W/R/Y conversion A/B</h1>",
        f"<p class='sub'>{html.escape(manifest['generated'])} · {html.escape(manifest['format'])} · "
        "the panel is the arbiter; the numbers below only narrow the field.</p>",
        "<div class='bar'>",
        "<label><input type='radio' name='view' value='preview' checked> per-pixel render</label>",
        "<label><input type='radio' name='view' value='sim'> eye-simulated</label>",
        "<label><input type='checkbox' id='boost'> boost contrast</label>",
        "<label><input type='checkbox' id='zoom'> 2x</label>",
        "</div>",
    ]

    for image in manifest["images"]:
        parts.append(f"<h2>{html.escape(image['name'])}</h2>")
        parts.append(f"<p class='sub'>{html.escape(image['source'])}</p>")
        parts.append(_summary_table(image["results"]))
        parts.append("<div class='grid'>")
        for r in image["results"]:
            parts.append(
                "<div class='card'>"
                f"<img data-preview='{html.escape(r['preview'])}' data-sim='{html.escape(r['sim'])}' "
                f"src='{html.escape(r['preview'])}' alt='{html.escape(r['recipe'])}'>"
                f"<div class='name'>{html.escape(r['recipe'])}</div>"
                f"<div class='desc'>{html.escape(r['description'])}</div>"
                f"<div class='m'>{_metric_cells(r['metrics'])}</div>"
                "</div>"
            )
        parts.append("</div>")

    parts.append(f"<script>{_JS}</script></body></html>")
    return "".join(parts)
