#!/usr/bin/env python3
"""
build_index.py  —  Regenerate index.html for the ECEN NameBadge OTA site.

Usage:
    python3 build_index.py <path-to-ota-site-repo>

Called automatically by publish.sh after each firmware publish.
Can also be run manually to rebuild the page without publishing.
"""
import json
import glob
import os
import sys
from datetime import datetime, timezone

def main():
    repo = sys.argv[1] if len(sys.argv) > 1 else "."
    catalogs = sorted(glob.glob(os.path.join(repo, "catalog_*.json")))

    firmwares = []
    for path in catalogs:
        try:
            with open(path) as f:
                d = json.load(f)
            fname = os.path.basename(path)
            d["_catalog_fname"] = fname
            variant = d.get("variant", fname.replace("catalog_","").replace(".json",""))
            # Check for matching full-flash descriptor
            flash_path = os.path.join(repo, f"flash_{variant}.json")
            d["_flash"] = None
            if os.path.exists(flash_path):
                with open(flash_path) as f2:
                    d["_flash"] = json.load(f2)
            firmwares.append(d)
        except Exception as e:
            print(f"WARN: skipping {path}: {e}")

    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    cards = ""
    for fw in firmwares:
        variant  = fw.get("variant", fw["_catalog_fname"].replace("catalog_","").replace(".json",""))
        version  = fw.get("version", "?")
        size_kb  = round(fw.get("size", 0) / 1024, 1)
        built    = fw.get("built", "unknown")
        catalog_fname = fw["_catalog_fname"]
        fl = fw.get("_flash")

        # Full-flash expandable section
        flash_section = ""
        if fl:
            rows = ""
            for p in fl.get("parts", []):
                note = f"<br><small style='color:#f87171'>{p['note']}</small>" if p.get("note") else ""
                rows += f"<tr><td><code>{p['addr']}</code></td><td><a href=\"{p['url']}\" download>{p['file']}</a>{note}</td></tr>"
            cmd = fl.get("esptool_cmd", "")
            cid = f"cmd-{variant}"
            flash_section = f"""
          <details>
            <summary>&#128196; Full USB Flash (includes bootloader)</summary>
            <div class="flash-box">
              <p class="flash-warn">&#9888; The bootloader can only be updated via USB &mdash; it cannot be changed over-the-air.</p>
              <p>Download all files into one folder, then run:</p>
              <div class="cmd-row">
                <code id="{cid}">{cmd}</code>
                <button onclick="copyCmd('{cid}')">Copy</button>
              </div>
              <table class="parts-table">
                <tr><th>Address</th><th>File</th></tr>
                {rows}
              </table>
            </div>
          </details>"""

        cards += f"""
        <div class="card">
          <div class="badge-variant">{variant.upper()}</div>
          <div class="meta">Version {version} &nbsp;&middot;&nbsp; {size_kb} KB &nbsp;&middot;&nbsp; Built {built}</div>
          <label>Manifest URL &mdash; paste into badge portal for OTA wireless update:</label>
          <div class="url-row">
            <input class="url-box" id="url-{variant}" readonly
                   data-fname="{catalog_fname}"
                   value="(loading&hellip;)" />
            <button onclick="copyUrl('url-{variant}')">Copy</button>
          </div>
          <div class="sha">SHA256: <code>{fw.get("sha256","")[:16]}&hellip;</code></div>
          {flash_section}
        </div>"""

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ECEN NameBadge — OTA Firmware</title>
  <style>
    *{{ box-sizing:border-box; margin:0; padding:0; }}
    body{{
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background:#0f172a; color:#e2e8f0; min-height:100vh;
    }}
    header{{
      background:#1e3a8a; padding:24px 32px;
      border-bottom:3px solid #3b82f6;
    }}
    header h1{{ font-size:1.6rem; font-weight:700; letter-spacing:.5px; }}
    header p{{ color:#93c5fd; margin-top:4px; font-size:.9rem; }}
    .grid{{
      display:grid;
      grid-template-columns: repeat(auto-fill, minmax(380px,1fr));
      gap:20px; padding:28px 32px;
    }}
    .card{{
      background:#1e293b; border:1px solid #334155;
      border-radius:10px; padding:20px;
    }}
    .badge-variant{{ font-size:1.2rem; font-weight:700; color:#60a5fa; margin-bottom:6px; }}
    .meta{{ font-size:.8rem; color:#94a3b8; margin-bottom:14px; }}
    label{{ font-size:.8rem; color:#94a3b8; display:block; margin-bottom:4px; }}
    .url-row{{ display:flex; gap:8px; margin-bottom:8px; }}
    .url-box{{
      flex:1; background:#0f172a; color:#e2e8f0;
      border:1px solid #475569; border-radius:6px;
      padding:7px 10px; font-size:.8rem; font-family:monospace;
    }}
    button{{
      background:#3b82f6; color:#fff; border:none;
      border-radius:6px; padding:7px 14px;
      cursor:pointer; font-size:.8rem; white-space:nowrap;
    }}
    button:hover{{ background:#2563eb; }}
    .sha{{ font-size:.72rem; color:#64748b; margin-bottom:12px; }}
    details{{ margin-top:10px; border-top:1px solid #334155; padding-top:10px; }}
    summary{{ cursor:pointer; font-size:.85rem; color:#94a3b8; user-select:none; }}
    summary:hover{{ color:#e2e8f0; }}
    .flash-box{{ margin-top:10px; }}
    .flash-warn{{
      background:#7f1d1d; color:#fca5a5;
      border-radius:6px; padding:8px 12px;
      font-size:.8rem; margin-bottom:10px;
    }}
    .flash-box p{{ font-size:.82rem; color:#94a3b8; margin-bottom:6px; }}
    .cmd-row{{ display:flex; gap:8px; margin-bottom:10px; align-items:flex-start; }}
    .cmd-row code{{
      flex:1; background:#0f172a; color:#86efac;
      border:1px solid #475569; border-radius:6px;
      padding:7px 10px; font-size:.75rem;
      word-break:break-all; white-space:pre-wrap;
    }}
    .parts-table{{ width:100%; border-collapse:collapse; font-size:.8rem; }}
    .parts-table th{{ text-align:left; color:#64748b; padding:3px 6px; }}
    .parts-table td{{ padding:3px 6px; border-top:1px solid #0f172a; }}
    .parts-table a{{ color:#93c5fd; }}
    footer{{ text-align:center; color:#475569; font-size:.8rem; padding:16px; margin-top:8px; }}
    .copied{{ background:#16a34a !important; }}
  </style>
</head>
<body>
  <header>
    <h1>ECEN NameBadge &mdash; OTA Firmware</h1>
    <p>
      <strong>OTA (wireless):</strong> Copy a manifest URL and paste it into the badge portal. &nbsp;|&nbsp;
      <strong>Full USB flash</strong> (including bootloader): expand a variant below.
    </p>
  </header>

  <div class="grid">
    {cards}
  </div>

  <footer>Last updated: {now}</footer>

  <script>
    document.querySelectorAll('.url-box[data-fname]').forEach(el => {{
      const base = window.location.href.replace(/\\/index\\.html$/, '').replace(/\\/$/, '');
      el.value = base + '/' + el.dataset.fname;
    }});
    function copyUrl(id) {{
      const el = document.getElementById(id);
      navigator.clipboard.writeText(el.value).then(() => {{
        const btn = el.nextElementSibling;
        btn.textContent = 'Copied!'; btn.classList.add('copied');
        setTimeout(() => {{ btn.textContent = 'Copy'; btn.classList.remove('copied'); }}, 1500);
      }});
    }}
    function copyCmd(id) {{
      const text = document.getElementById(id).textContent;
      navigator.clipboard.writeText(text).then(() => {{
        event.target.textContent = 'Copied!'; event.target.classList.add('copied');
        setTimeout(() => {{ event.target.textContent = 'Copy'; event.target.classList.remove('copied'); }}, 1500);
      }});
    }}
  </script>
</body>
</html>
"""

    out_path = os.path.join(repo, "index.html")
    with open(out_path, "w") as f:
        f.write(html)
    print(f"Wrote {out_path}  ({len(firmwares)} firmware variant(s))")


if __name__ == "__main__":
    main()
