/* global document */

(function () {
  const plot = document.getElementById("plot");
  const pdnPhysicalRoot = document.getElementById("pdnPhysicalRoot");
  const pdnCanvasIntegrated = document.getElementById("pdnCanvasIntegrated");
  const pdnLayerGrid = document.getElementById("pdnLayerGrid");
  const pdnConnects = document.getElementById("pdnConnects");
  const pdnGraphRoot = document.getElementById("pdnGraphRoot");
  const pdnGraphCanvas = document.getElementById("pdnGraphCanvas");
  const pdnGraphMeta = document.getElementById("pdnGraphMeta");
  const frame = document.getElementById("frame");
  const statusEl = document.getElementById("status");
  const manifestInput = document.getElementById("manifest");
  const meshInput = document.getElementById("mesh");
  const fileInput = document.getElementById("fileInput");
  const zoomSlider = document.getElementById("zoomSlider");
  const zoomValue = document.getElementById("zoomValue");
  const zoomIn = document.getElementById("zoomIn");
  const zoomOut = document.getElementById("zoomOut");

  let currentKind = "current";
  let pdnPhysicalCache = null;
  let pdnGraphCache = null;
  let pdnGraphView = null;
  let pdnGraphHoverEl = null;

  function setStatus(msg) { statusEl.textContent = msg || ""; }
  function zoomPercent() { return Number(zoomSlider.value) / 100; }
  function syncZoomLabel() { zoomValue.textContent = `${zoomSlider.value}%`; }

  function hashHue(s) {
    let h = 2166136261;
    for (let i = 0; i < s.length; i += 1) h = Math.imul(h ^ s.charCodeAt(i), 16777619);
    return Math.abs(h) % 360;
  }
  function metalRankFromName(name) {
    const m = String(name).match(/(\d+)/);
    return m ? parseInt(m[1], 10) : 0;
  }
  function uniqueLayersSorted(straps) {
    return [...new Set(straps.map((s) => s.layer))].sort(
      (a, b) => metalRankFromName(a) - metalRankFromName(b) || a.localeCompare(b),
    );
  }
  function layerColorMap(layerNames) {
    const sorted = [...new Set(layerNames)].sort(
      (a, b) => metalRankFromName(a) - metalRankFromName(b) || a.localeCompare(b),
    );
    // Distinct, colorblind-friendly-ish palette; cycles if many layers.
    const palette = [
      "#2563eb", "#16a34a", "#ea580c", "#7c3aed", "#0891b2",
      "#dc2626", "#65a30d", "#0f766e", "#c2410c", "#be185d",
      "#0369a1", "#4338ca",
    ];
    const m = new Map();
    sorted.forEach((name, i) => {
      m.set(name, palette[i % palette.length]);
    });
    return m;
  }

  function hideCustomRoots() {
    if (pdnPhysicalRoot) pdnPhysicalRoot.style.display = "none";
    if (pdnGraphRoot) pdnGraphRoot.style.display = "none";
    if (pdnLayerGrid) pdnLayerGrid.innerHTML = "";
    pdnPhysicalCache = null;
    pdnGraphCache = null;
    pdnGraphView = null;
    if (pdnGraphHoverEl) pdnGraphHoverEl.style.display = "none";
  }
  function ensurePdnGraphHover() {
    if (!pdnGraphRoot) return null;
    pdnGraphRoot.style.position = "relative";
    if (!pdnGraphHoverEl) {
      const el = document.createElement("div");
      el.style.position = "absolute";
      el.style.pointerEvents = "none";
      el.style.display = "none";
      el.style.background = "rgba(24,24,27,0.94)";
      el.style.color = "#fafafa";
      el.style.border = "1px solid rgba(255,255,255,0.18)";
      el.style.borderRadius = "6px";
      el.style.padding = "6px 8px";
      el.style.fontSize = "0.75rem";
      el.style.whiteSpace = "pre";
      el.style.zIndex = "20";
      el.style.boxShadow = "0 4px 14px rgba(0,0,0,0.2)";
      pdnGraphRoot.appendChild(el);
      pdnGraphHoverEl = el;
    }
    return pdnGraphHoverEl;
  }
  function drawTriangle(ctx, x, y, r) {
    const h = Math.sqrt(3) * r;
    ctx.beginPath();
    ctx.moveTo(x, y - (2 / 3) * h);
    ctx.lineTo(x - r, y + (1 / 3) * h);
    ctx.lineTo(x + r, y + (1 / 3) * h);
    ctx.closePath();
  }
  function htmlEsc(s) {
    return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function drawPdnStrapsOnCanvas(canvas, strapList, world, sz) {
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    const { minx, maxx, miny, maxy, core, die } = world;
    canvas.width = sz;
    canvas.height = sz;
    ctx.clearRect(0, 0, sz, sz);
    const pad = 28;
    const w = maxx - minx;
    const h = maxy - miny;
    const scale = Math.min((sz - 2 * pad) / w, (sz - 2 * pad) / h);
    const sx = (x) => pad + (x - minx) * scale;
    const sy = (y) => pad + (maxy - y) * scale;

    for (const s of strapList) {
      const hue = hashHue(`${s.grid}\t${s.layer}`);
      ctx.fillStyle = s.fp ? `hsla(${hue}, 55%, 52%, 0.26)` : `hsla(${hue}, 58%, 48%, 0.42)`;
      const x0 = sx(s.llx);
      const y0 = sy(s.ury);
      ctx.fillRect(x0, y0, (s.urx - s.llx) * scale, (s.ury - s.lly) * scale);
      if (s.fp) {
        ctx.strokeStyle = "rgba(0,0,0,0.14)";
        ctx.lineWidth = 0.5;
        ctx.strokeRect(x0, y0, (s.urx - s.llx) * scale, (s.ury - s.lly) * scale);
      }
    }
    if (die) {
      ctx.strokeStyle = "rgba(82,82,91,0.88)";
      ctx.lineWidth = 1.2;
      ctx.setLineDash([]);
      ctx.strokeRect(sx(die[0]), sy(die[3]), (die[2] - die[0]) * scale, (die[3] - die[1]) * scale);
    }
    if (core) {
      ctx.strokeStyle = "rgba(24,24,27,0.95)";
      ctx.lineWidth = 1.6;
      ctx.setLineDash([7, 5]);
      ctx.strokeRect(sx(core[0]), sy(core[3]), (core[2] - core[0]) * scale, (core[3] - core[1]) * scale);
      ctx.setLineDash([]);
    }
  }

  function parsePdnTclPhysicalTsv(text) {
    const lines = text.split(/\r?\n/);
    let prevLine = "";
    let core = null;
    let die = null;
    const straps = [];
    const connects = [];
    const numRow = /^#\t([-0-9.eE]+)\t([-0-9.eE]+)\t([-0-9.eE]+)\t([-0-9.eE]+)\s*$/;
    for (const line of lines) {
      const nm = line.match(numRow);
      if (nm) {
        const box = [Number(nm[1]), Number(nm[2]), Number(nm[3]), Number(nm[4])];
        if (prevLine.includes("core_llx")) core = box;
        if (prevLine.includes("die_llx")) die = box;
      }
      prevLine = line;
      if (!line || line.startsWith("#") || line.startsWith("kind\t")) continue;
      const p = line.split("\t");
      if (p[0] === "strap" && p.length >= 13) {
        straps.push({ grid: p[1], layer: p[2], fp: Number(p[7]), hz: Number(p[8]), llx: Number(p[9]), lly: Number(p[10]), urx: Number(p[11]), ury: Number(p[12]) });
      } else if (p[0] === "connect" && p.length >= 4) {
        connects.push({ grid: p[1], lo: p[2], hi: p[3] });
      }
    }
    let minx = Infinity, miny = Infinity, maxx = -Infinity, maxy = -Infinity;
    const bump = (llx, lly, urx, ury) => {
      minx = Math.min(minx, llx); miny = Math.min(miny, lly);
      maxx = Math.max(maxx, urx); maxy = Math.max(maxy, ury);
    };
    for (const s of straps) bump(s.llx, s.lly, s.urx, s.ury);
    if (core) bump(core[0], core[1], core[2], core[3]);
    if (die) bump(die[0], die[1], die[2], die[3]);
    if (!Number.isFinite(minx) || minx >= maxx) { minx = 0; miny = 0; maxx = 1100; maxy = 1100; }
    return { straps, connects, core, die, minx, maxx, miny, maxy };
  }

  function parseGraphNodesTsv(text) {
    const out = [];
    for (const line of text.split(/\r?\n/)) {
      if (!line || line.startsWith("#") || line.startsWith("id\t")) continue;
      const p = line.split("\t");
      if (p.length < 8) continue;
      out.push({
        id: Number(p[0]),
        layer_idx: Number(p[1]),
        layer_name: p[2],
        x: Number(p[3]),
        y: Number(p[4]),
        v: Number(p[5]),
        fixed: Number(p[6]) === 1,
        i_load: Number(p[7]),
        grid: p.length >= 9 && p[8] ? p[8] : "N/A (not exported)",
      });
    }
    return out;
  }

  function parseGraphEdgesTsv(text) {
    const out = [];
    for (const line of text.split(/\r?\n/)) {
      if (!line || line.startsWith("#") || line.startsWith("u\t")) continue;
      const p = line.split("\t");
      if (p.length < 4) continue;
      out.push({ u: Number(p[0]), v: Number(p[1]), kind: p[2], g: Number(p[3]) });
    }
    return out;
  }
  function parsePsmVsrcBoxesTsv(text) {
    const out = [];
    for (const line of text.split(/\r?\n/)) {
      if (!line || line.startsWith("#")) continue;
      const p = line.split("\t");
      if (p.length < 5) continue;
      if (p[0] === "llx_um") continue;
      const llx = Number(p[0]);
      const lly = Number(p[1]);
      const urx = Number(p[2]);
      const ury = Number(p[3]);
      if (!Number.isFinite(llx) || !Number.isFinite(lly) || !Number.isFinite(urx) || !Number.isFinite(ury)) continue;
      out.push({
        llx, lly, urx, ury,
        cx: 0.5 * (llx + urx),
        cy: 0.5 * (lly + ury),
        layer: p[5] || "",
        bterm: p[6] || "",
      });
    }
    return out;
  }

  function redrawPdnPhysicalCanvas(zoomMul) {
    if (!pdnPhysicalCache || !pdnCanvasIntegrated) return;
    const { straps, connects, core, die, minx, maxx, miny, maxy } = pdnPhysicalCache;
    const world = { minx, maxx, miny, maxy, core, die };
    const z = zoomMul || 1;
    const rect = frame.getBoundingClientRect();
    const baseIntegrated = Math.min(1100, Math.max(480, Math.floor(rect.width) - 24));
    drawPdnStrapsOnCanvas(pdnCanvasIntegrated, straps, world, Math.round(baseIntegrated * z));

    const layersSorted = uniqueLayersSorted(straps);
    if (pdnLayerGrid) {
      pdnLayerGrid.innerHTML = "";
      const layerTile = Math.min(320, Math.max(160, Math.round(220 * z)));
      for (const layer of layersSorted) {
        const sub = straps.filter((s) => s.layer === layer);
        const wrap = document.createElement("div");
        wrap.style.flex = "0 0 auto";
        wrap.style.border = "1px solid #e4e4e7";
        wrap.style.borderRadius = "8px";
        wrap.style.padding = "8px";
        wrap.style.background = "#fff";
        const cap = document.createElement("div");
        cap.textContent = `${layer} — ${sub.length} rect(s)`;
        cap.style.fontSize = "0.78rem";
        cap.style.fontWeight = "600";
        cap.style.color = "#3f3f46";
        cap.style.marginBottom = "6px";
        const cv = document.createElement("canvas");
        cv.style.display = "block";
        cv.style.borderRadius = "4px";
        cv.style.background = "#fafafa";
        wrap.appendChild(cap);
        wrap.appendChild(cv);
        pdnLayerGrid.appendChild(wrap);
        drawPdnStrapsOnCanvas(cv, sub, world, layerTile);
      }
    }
    pdnConnects.textContent = connects.length === 0 ? "(no add_pdn_connect rows)" : connects.map((c) => `${c.grid}: ${c.lo} — ${c.hi}  (no via x,y in Tcl)`).join("\n");
    setStatus(`PDN Tcl physical: ${straps.length} strap rects, ${layersSorted.length} layers, ${connects.length} connects`);
  }

  function redrawPdnGraphCanvas(zoomMul) {
    if (!pdnGraphCache || !pdnGraphCanvas) return;
    const { nodes, edges, vsrcBoxes } = pdnGraphCache;
    const ctx = pdnGraphCanvas.getContext("2d");
    if (!ctx || nodes.length === 0) return;
    const hideLayer = (name) => String(name).toLowerCase() === "metal1";
    const visibleNodes = nodes.filter((n) => !hideLayer(n.layer_name));
    const visibleIds = new Set(visibleNodes.map((n) => n.id));
    const visibleEdges = edges.filter((e) => visibleIds.has(e.u) && visibleIds.has(e.v));
    if (visibleNodes.length === 0) return;
    const rect = frame.getBoundingClientRect();
    const baseW = Math.min(1200, Math.max(640, Math.floor(rect.width) - 24));
    const w = Math.round(baseW * (zoomMul || 1));
    const h = Math.round(Math.max(480, w * 0.75));
    pdnGraphCanvas.width = w;
    pdnGraphCanvas.height = h;

    let minx = Infinity, miny = Infinity, maxx = -Infinity, maxy = -Infinity;
    for (const n of visibleNodes) {
      minx = Math.min(minx, n.x); miny = Math.min(miny, n.y);
      maxx = Math.max(maxx, n.x); maxy = Math.max(maxy, n.y);
    }
    const pad = 24;
    const sx = (x) => pad + ((x - minx) / Math.max(1e-9, maxx - minx)) * (w - 2 * pad);
    const sy = (y) => pad + ((maxy - y) / Math.max(1e-9, maxy - miny)) * (h - 2 * pad);

    const byId = new Map(visibleNodes.map((n) => [n.id, n]));
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, w, h);

    for (const e of visibleEdges) {
      const a = byId.get(e.u); const b = byId.get(e.v);
      if (!a || !b) continue;
      ctx.strokeStyle = e.kind === "via" ? "rgba(109,40,217,0.35)" : "rgba(24,24,27,0.18)";
      ctx.lineWidth = e.kind === "via" ? 1.2 : 0.5;
      ctx.beginPath();
      ctx.moveTo(sx(a.x), sy(a.y));
      ctx.lineTo(sx(b.x), sy(b.y));
      ctx.stroke();
    }

    const layers = [...new Set(visibleNodes.map((n) => n.layer_name))].sort(
      (a, b) => metalRankFromName(a) - metalRankFromName(b),
    );
    const colorByLayer = layerColorMap(layers);
    const countsByLayer = new Map(layers.map((l) => [l, 0]));
    for (const n of visibleNodes) countsByLayer.set(n.layer_name, (countsByLayer.get(n.layer_name) || 0) + 1);

    for (const n of visibleNodes) {
      const layerColor = colorByLayer.get(n.layer_name) || "#334155";
      const px = sx(n.x);
      const py = sy(n.y);
      const r = n.fixed ? 2.8 : 1.8;
      if (n.fixed) {
        // Fixed nodes: triangle + layer-colored stroke.
        ctx.fillStyle = "rgba(220,38,38,0.95)";
        drawTriangle(ctx, px, py, r);
        ctx.fill();
        ctx.strokeStyle = layerColor;
        ctx.lineWidth = 1.0;
        ctx.stroke();
      } else {
        ctx.fillStyle = layerColor;
        ctx.beginPath();
        ctx.arc(px, py, r, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    for (const s of (vsrcBoxes || [])) {
      const x = sx(s.cx);
      const y = sy(s.cy);
      const rr = 4.5;
      ctx.strokeStyle = "rgba(17,24,39,0.95)";
      ctx.lineWidth = 1.3;
      ctx.beginPath();
      ctx.arc(x, y, rr, 0, Math.PI * 2);
      ctx.stroke();
      ctx.beginPath();
      ctx.moveTo(x - rr - 2, y);
      ctx.lineTo(x + rr + 2, y);
      ctx.moveTo(x, y - rr - 2);
      ctx.lineTo(x, y + rr + 2);
      ctx.stroke();
    }

    const viaCount = visibleEdges.filter((e) => e.kind === "via").length;
    const wireCount = visibleEdges.length - viaCount;
    const legendRows = layers.map((l) => {
      const c = colorByLayer.get(l) || "#334155";
      const cnt = countsByLayer.get(l) || 0;
      return `<span style="display:inline-flex;align-items:center;gap:6px;margin-right:12px;margin-bottom:4px;">
        <span style="width:11px;height:11px;border-radius:2px;background:${c};border:1px solid rgba(24,24,27,0.2);display:inline-block;"></span>
        <span>${htmlEsc(l)} (${cnt})</span>
      </span>`;
    }).join("");
    pdnGraphMeta.innerHTML = `
      <div>nodes: ${visibleNodes.length} shown / ${nodes.length} total</div>
      <div>edges: ${visibleEdges.length} shown / ${edges.length} total (wire=${wireCount}, via=${viaCount})</div>
      <div>hidden in GUI: metal1</div>
      <div>source centers from BTerm boxes: ${(vsrcBoxes || []).length} (black cross-circle markers)</div>
      <div style="margin-top:6px;">layer legend:</div>
      <div style="margin-top:4px;line-height:1.35;">${legendRows}</div>
      <div style="margin-top:6px;">fixed nodes: red triangles with layer-color outline</div>
    `;
    pdnGraphView = {
      nodes: visibleNodes.map((n) => {
        const px = sx(n.x);
        const py = sy(n.y);
        return {
          ...n,
          px,
          py,
          r: n.fixed ? 5.0 : 4.0,
          color: colorByLayer.get(n.layer_name) || "#334155",
        };
      }),
      pad,
    };
    setStatus(`PDN graph: ${visibleNodes.length} nodes shown (${nodes.length} total), ${visibleEdges.length} edges shown`);
  }

  function loadPdnTclPhysicalCanvas() {
    const wrap = document.getElementById("wrap3d");
    if (wrap) { wrap.style.display = "none"; wrap.innerHTML = ""; }
    plot.style.display = "none";
    if (pdnGraphRoot) pdnGraphRoot.style.display = "none";
    if (pdnPhysicalRoot) pdnPhysicalRoot.style.display = "block";
    setStatus("Loading research/out/pdn_tcl_physical.tsv …");
    fetch(`/out/pdn_tcl_physical.tsv?t=${Date.now()}`)
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status} — run phys_load to generate research/out/pdn_tcl_physical.tsv`);
        return r.text();
      })
      .then((text) => {
        pdnPhysicalCache = parsePdnTclPhysicalTsv(text);
        redrawPdnPhysicalCanvas(zoomPercent());
      })
      .catch((e) => {
        setStatus(String(e.message || e));
        hideCustomRoots();
        plot.style.display = "block";
      });
  }

  function loadPdnGraphCanvas() {
    const wrap = document.getElementById("wrap3d");
    if (wrap) { wrap.style.display = "none"; wrap.innerHTML = ""; }
    plot.style.display = "none";
    if (pdnPhysicalRoot) pdnPhysicalRoot.style.display = "none";
    if (pdnGraphRoot) pdnGraphRoot.style.display = "block";
    setStatus("Loading solver graph TSVs …");
    Promise.all([
      fetch(`/out/ir_pdn_graph_nodes.tsv?t=${Date.now()}`).then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status} for ir_pdn_graph_nodes.tsv (run graph mode solve first)`);
        return r.text();
      }),
      fetch(`/out/ir_pdn_graph_edges.tsv?t=${Date.now()}`).then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status} for ir_pdn_graph_edges.tsv (run graph mode solve first)`);
        return r.text();
      }),
      fetch(`/out/floorplan_db_extract/psm_vsrc_boxes_VDD.tsv?t=${Date.now()}`)
        .then((r) => (r.ok ? r.text() : fetch(`/out/psm_vsrc_boxes_VDD.tsv?t=${Date.now()}`)
          .then((r2) => (r2.ok ? r2.text() : "")))),
    ])
      .then(([nt, et, vt]) => {
        pdnGraphCache = {
          nodes: parseGraphNodesTsv(nt),
          edges: parseGraphEdgesTsv(et),
          vsrcBoxes: vt ? parsePsmVsrcBoxesTsv(vt) : [],
        };
        redrawPdnGraphCanvas(zoomPercent());
      })
      .catch((e) => {
        setStatus(String(e.message || e));
        hideCustomRoots();
        plot.style.display = "block";
      });
  }
  if (pdnGraphCanvas) {
    pdnGraphCanvas.addEventListener("mouseleave", () => {
      if (pdnGraphHoverEl) pdnGraphHoverEl.style.display = "none";
    });
    pdnGraphCanvas.addEventListener("mousemove", (ev) => {
      if (!pdnGraphView || !pdnGraphRoot) return;
      const hover = ensurePdnGraphHover();
      if (!hover) return;
      const rect = pdnGraphCanvas.getBoundingClientRect();
      const cx = (ev.clientX - rect.left) * (pdnGraphCanvas.width / Math.max(1, rect.width));
      const cy = (ev.clientY - rect.top) * (pdnGraphCanvas.height / Math.max(1, rect.height));
      let best = null;
      let bestD2 = Infinity;
      for (const n of pdnGraphView.nodes) {
        const dx = n.px - cx;
        const dy = n.py - cy;
        const d2 = dx * dx + dy * dy;
        const hitR = n.r + 2.5;
        if (d2 <= hitR * hitR && d2 < bestD2) {
          best = n;
          bestD2 = d2;
        }
      }
      if (!best) {
        hover.style.display = "none";
        return;
      }
      hover.style.display = "block";
      hover.textContent = [
        `id: ${best.id}`,
        `layer: ${best.layer_name} (idx=${best.layer_idx})`,
        `grid: ${best.grid}`,
        `xy: (${best.x.toFixed(3)}, ${best.y.toFixed(3)}) um`,
        `V: ${best.v.toFixed(6)} V`,
        `I_load: ${best.i_load.toExponential(3)} A`,
        `fixed: ${best.fixed ? "yes" : "no"}`,
      ].join("\n");
      const rootRect = pdnGraphRoot.getBoundingClientRect();
      const lx = ev.clientX - rootRect.left + 12;
      const ly = ev.clientY - rootRect.top + 12;
      hover.style.left = `${Math.round(lx)}px`;
      hover.style.top = `${Math.round(ly)}px`;
    });
  }

  function activeButton(kind) {
    document.querySelectorAll(".plot-btn").forEach((b) => {
      b.classList.toggle("active", b.dataset.kind === kind);
    });
  }

  function loadFromServer(kind) {
    currentKind = kind;
    activeButton(kind);
    const man = encodeURIComponent(manifestInput.value.trim() || "research/mempool.json");
    const mesh = encodeURIComponent(meshInput.value.trim() || "research/out/ir_mesh_nodes.tsv");
    const ts = Date.now();

    if (kind === "pdn_tcl_physical") { loadPdnTclPhysicalCanvas(); return; }
    if (kind === "pdn_graph") { loadPdnGraphCanvas(); return; }

    hideCustomRoots();

    if (kind === "model3d") {
      plot.style.display = "none";
      let wrap = document.getElementById("wrap3d");
      if (!wrap) {
        wrap = document.createElement("div");
        wrap.id = "wrap3d";
        wrap.style.display = "flex";
        wrap.style.flexWrap = "wrap";
        wrap.style.gap = "10px";
        wrap.style.width = "100%";
        wrap.style.minHeight = "min(720px, 85vh)";
        frame.appendChild(wrap);
      }
      wrap.style.display = "flex";
      wrap.innerHTML = "";
      setStatus("Running plot.py model3d (Plotly HTML)…");
      fetch(`/api/render?kind=model3d&manifest=${man}&mesh=${mesh}&t=${ts}`)
        .then((r) => {
          if (!r.ok) return r.text().then((t) => { throw new Error(t.slice(0, 400) || String(r.status)); });
          return r.json();
        })
        .then((j) => {
          const addIframe = (file, title) => {
            const box = document.createElement("div");
            box.style.flex = "1 1 45%";
            box.style.minWidth = "280px";
            const cap = document.createElement("div");
            cap.textContent = title;
            cap.style.fontSize = "0.8rem";
            cap.style.color = "#52525b";
            cap.style.marginBottom = "4px";
            const iframe = document.createElement("iframe");
            iframe.style.width = "100%";
            iframe.style.height = "min(680px, 80vh)";
            iframe.style.border = "1px solid #d4d4d8";
            iframe.style.borderRadius = "6px";
            iframe.style.background = "#fff";
            iframe.src = `/out/${encodeURIComponent(file)}?t=${ts}`;
            box.appendChild(cap);
            box.appendChild(iframe);
            wrap.appendChild(box);
          };
          addIframe(j.std, "Core / standard-cell grid (add_pdn_stripe -grid {grid})");
          addIframe(j.macro, "Macro grids (CORE_macro_grid_* stripes)");
          setStatus("3D: drag to orbit, scroll to zoom (Plotly in each pane).");
        })
        .catch((e) => {
          wrap.innerHTML = "";
          setStatus("model3d failed — " + e.message);
        });
      return;
    }

    const wrap = document.getElementById("wrap3d");
    if (wrap) { wrap.style.display = "none"; wrap.innerHTML = ""; }
    plot.style.display = "block";
    const url = `/api/render?kind=${encodeURIComponent(kind)}&manifest=${man}&mesh=${mesh}&t=${ts}`;
    setStatus("Rendering…");
    plot.onload = () => {
      applyPlotZoom();
      setStatus(`Showing: ${kind} (server)`);
    };
    plot.onerror = () => setStatus("Failed to load image (check server terminal for plot.py errors).");
    plot.src = url;
  }

  function applyPlotZoom() {
    const z = zoomPercent();
    syncZoomLabel();
    if (currentKind === "pdn_tcl_physical" && pdnPhysicalCache) { redrawPdnPhysicalCanvas(z); return; }
    if (currentKind === "pdn_graph" && pdnGraphCache) { redrawPdnGraphCanvas(z); return; }
    if (!plot.naturalWidth) { plot.style.width = "100%"; return; }
    plot.style.width = `${Math.round(plot.naturalWidth * z)}px`;
    plot.style.height = "auto";
  }

  document.querySelectorAll(".plot-btn").forEach((btn) => {
    btn.addEventListener("click", () => loadFromServer(btn.dataset.kind));
  });

  document.getElementById("btnRefresh").addEventListener("click", () => loadFromServer(currentKind));

  fileInput.addEventListener("change", () => {
    const f = fileInput.files && fileInput.files[0];
    if (!f) return;
    const url = URL.createObjectURL(f);
    plot.onload = () => {
      applyPlotZoom();
      setStatus(`Local file: ${f.name}`);
      URL.revokeObjectURL(url);
    };
    hideCustomRoots();
    plot.style.display = "block";
    plot.src = url;
    document.querySelectorAll(".plot-btn").forEach((b) => b.classList.remove("active"));
  });

  zoomSlider.addEventListener("input", () => applyPlotZoom());
  zoomIn.addEventListener("click", () => {
    const v = Math.min(300, Number(zoomSlider.value) + 10);
    zoomSlider.value = String(v);
    applyPlotZoom();
  });
  zoomOut.addEventListener("click", () => {
    const v = Math.max(25, Number(zoomSlider.value) - 10);
    zoomSlider.value = String(v);
    applyPlotZoom();
  });

  window.openOutPng = function (name) {
    hideCustomRoots();
    plot.style.display = "block";
    plot.onload = () => {
      applyPlotZoom();
      setStatus(`Static: research/out/${name}`);
    };
    plot.src = `/out/${encodeURIComponent(name)}?t=${Date.now()}`;
  };

  syncZoomLabel();
  loadFromServer("current");
})();
