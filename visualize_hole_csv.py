#!/usr/bin/env python3
"""Generate a self-contained HTML dashboard from a mini hole CSV."""

from __future__ import annotations

import argparse
import csv
import html
import json
import math
from pathlib import Path
from typing import Dict, Iterator, List, Optional, Tuple


PERIOD_FIELDS = (
    "period_alloc",
    "period_measured",
    "period_requested",
    "period_hole",
    "period_failed",
    "period_measure_error",
)

LIVE_FIELDS = (
    "live_alloc",
    "live_requested",
    "live_allocated",
    "live_hole",
)

LIVE_EVENT_FIELDS = (
    "period_free",
    "period_untracked_free",
    "period_live_track_failed",
)


def parse_metadata(line: str) -> Dict[str, str]:
    parts = [part.strip() for part in line.strip().split(",")]
    metadata = {"format": parts[0].lstrip("#") if parts else "unknown"}
    for part in parts[1:]:
        if "=" in part:
            key, value = part.split("=", 1)
            metadata[key] = value
    return metadata


def read_preamble(path: Path) -> Tuple[Dict[str, str], List[str]]:
    with path.open("r", encoding="utf-8", newline="") as source:
        metadata_line = source.readline()
        header_line = source.readline()
    if not metadata_line.startswith("#mini_malloc_hole_"):
        raise ValueError("不是 mini malloc hole CSV：缺少格式元信息行")
    if not header_line:
        raise ValueError("CSV 缺少字段表头")
    header = next(csv.reader([header_line]))
    required = {"start_ns", "end_ns", *PERIOD_FIELDS}
    missing = sorted(required.difference(header))
    if missing:
        raise ValueError(f"CSV 缺少必要字段：{', '.join(missing)}")
    return parse_metadata(metadata_line), header


def discover_buckets(header: List[str]) -> List[Tuple[str, str, str]]:
    fields = set(header)
    buckets = []
    for field in header:
        if not field.startswith("count_"):
            continue
        label = field[len("count_") :]
        hole_field = f"hole_{label}"
        if hole_field in fields:
            buckets.append((label, field, hole_field))
    return buckets


def parse_int(value: str) -> int:
    return int(value.strip() or "0")


def iter_rows(
    path: Path,
    header: List[str],
    selected_pid: Optional[int],
) -> Iterator[Tuple[int, Dict[str, int]]]:
    with path.open("r", encoding="utf-8", newline="") as source:
        source.readline()
        source.readline()
        reader = csv.reader(source)
        for line_number, values in enumerate(reader, start=3):
            if not values or all(not value.strip() for value in values):
                continue
            if len(values) != len(header):
                yield line_number, {}
                continue
            try:
                row = {
                    name: parse_int(value)
                    for name, value in zip(header, values)
                }
            except ValueError:
                yield line_number, {}
                continue
            if selected_pid is not None and "pid" in row:
                if row["pid"] != selected_pid:
                    continue
            yield line_number, row


def empty_totals() -> Dict[str, int]:
    return {field: 0 for field in PERIOD_FIELDS}


def bucket_capacity(label: str) -> Optional[int]:
    try:
        return int(label)
    except ValueError:
        return None


def first_pass(
    path: Path,
    metadata: Dict[str, str],
    header: List[str],
    buckets: List[Tuple[str, str, str]],
    selected_pid: Optional[int],
) -> Dict[str, object]:
    totals = empty_totals()
    bucket_totals = {
        label: {"count": 0, "hole": 0}
        for label, _, _ in buckets
    }
    row_count = 0
    malformed = 0
    start_ns = None
    end_ns = None
    pids = set()
    peak_hole = 0
    peak_alloc = 0
    peak_ratio = 0.0
    live_supported = all(field in header for field in LIVE_FIELDS)
    live_event_totals = {
        field: 0 for field in LIVE_EVENT_FIELDS if field in header
    }
    live_latest = {
        field: 0 for field in LIVE_FIELDS
    }
    live_bucket_latest = {
        label: {"count": 0, "hole": 0}
        for label, _, _ in buckets
    }
    peak_live_hole = 0
    peak_live_allocated = 0
    peak_live_ratio = 0.0

    for _, row in iter_rows(path, header, selected_pid):
        if not row:
            malformed += 1
            continue
        row_count += 1
        row_start = row["start_ns"]
        row_end = row["end_ns"]
        start_ns = row_start if start_ns is None else min(start_ns, row_start)
        end_ns = row_end if end_ns is None else max(end_ns, row_end)
        if "pid" in row:
            pids.add(row["pid"])

        for field in PERIOD_FIELDS:
            totals[field] += row[field]
        for label, count_field, hole_field in buckets:
            bucket_totals[label]["count"] += row[count_field]
            bucket_totals[label]["hole"] += row[hole_field]

        actual = row["period_requested"] + row["period_hole"]
        ratio = row["period_hole"] / actual if actual else 0.0
        peak_hole = max(peak_hole, row["period_hole"])
        peak_alloc = max(peak_alloc, row["period_alloc"])
        peak_ratio = max(peak_ratio, ratio)
        for field in live_event_totals:
            live_event_totals[field] += row[field]
        if live_supported:
            for field in LIVE_FIELDS:
                live_latest[field] = row[field]
            for label, _, _ in buckets:
                live_count = f"live_count_{label}"
                live_hole = f"live_hole_{label}"
                if live_count in row and live_hole in row:
                    live_bucket_latest[label]["count"] = row[live_count]
                    live_bucket_latest[label]["hole"] = row[live_hole]
            live_actual = row["live_allocated"]
            live_ratio = (
                row["live_hole"] / live_actual if live_actual else 0.0
            )
            peak_live_hole = max(peak_live_hole, row["live_hole"])
            peak_live_allocated = max(
                peak_live_allocated, live_actual
            )
            peak_live_ratio = max(peak_live_ratio, live_ratio)

    if not pids and selected_pid is not None:
        pids.add(selected_pid)
    elif not pids:
        metadata_pid = metadata.get("pid") or metadata.get("creator_pid")
        if metadata_pid and metadata_pid.isdigit():
            pids.add(int(metadata_pid))

    requested = totals["period_requested"]
    hole = totals["period_hole"]
    actual = requested + hole
    live_actual = live_latest["live_allocated"]
    live_summary = {
        "allocations": live_latest["live_alloc"],
        "requested_bytes": live_latest["live_requested"],
        "allocated_bytes": live_actual,
        "hole_bytes": live_latest["live_hole"],
        "hole_ratio": (
            live_latest["live_hole"] / live_actual
            if live_actual else 0.0
        ),
        "total_free": live_event_totals.get("period_free", 0),
        "total_untracked_free": live_event_totals.get(
            "period_untracked_free", 0
        ),
        "total_tracking_failures": live_event_totals.get(
            "period_live_track_failed", 0
        ),
        "peak_hole_bytes": peak_live_hole,
        "peak_allocated_bytes": peak_live_allocated,
        "peak_hole_ratio": peak_live_ratio,
        "bucket_totals": live_bucket_latest,
    }
    summary = {
        "row_count": row_count,
        "malformed_rows": malformed,
        "start_ns": start_ns or 0,
        "end_ns": end_ns or 0,
        "duration_ns": max(0, (end_ns or 0) - (start_ns or 0)),
        "pids": sorted(pids),
        "totals": totals,
        "actual_bytes": actual,
        "hole_ratio": hole / actual if actual else 0.0,
        "hole_over_requested": hole / requested if requested else 0.0,
        "average_hole": (
            hole / totals["period_measured"]
            if totals["period_measured"]
            else 0.0
        ),
        "peak_period_hole": peak_hole,
        "peak_period_alloc": peak_alloc,
        "peak_period_ratio": peak_ratio,
        "bucket_totals": bucket_totals,
        "live_supported": live_supported,
        "live": live_summary,
    }
    return summary


def aggregate_points(
    path: Path,
    header: List[str],
    selected_pid: Optional[int],
    row_count: int,
    max_points: int,
) -> List[Dict[str, object]]:
    if row_count == 0:
        return []
    group_size = max(1, math.ceil(row_count / max_points))
    points: List[Dict[str, object]] = []
    group: List[Dict[str, int]] = []
    cumulative_hole = 0
    cumulative_requested = 0
    live_supported = all(field in header for field in LIVE_FIELDS)

    def finish_group(rows: List[Dict[str, int]]) -> None:
        nonlocal cumulative_hole, cumulative_requested
        if not rows:
            return
        start_ns = min(row["start_ns"] for row in rows)
        end_ns = max(row["end_ns"] for row in rows)
        duration_seconds = max(0, end_ns - start_ns) / 1_000_000_000
        duration_seconds = max(duration_seconds, 1e-9)
        alloc = sum(row["period_alloc"] for row in rows)
        requested = sum(row["period_requested"] for row in rows)
        hole = sum(row["period_hole"] for row in rows)
        failed = sum(row["period_failed"] for row in rows)
        actual = requested + hole
        latest = rows[-1]
        live_allocated = (
            latest["live_allocated"] if live_supported else 0
        )
        cumulative_hole += hole
        cumulative_requested += requested
        points.append(
            {
                "startNs": start_ns,
                "endNs": end_ns,
                "rows": len(rows),
                "allocRate": alloc / duration_seconds,
                "requestedRate": requested / duration_seconds,
                "holeRate": hole / duration_seconds,
                "failedRate": failed / duration_seconds,
                "holeRatio": hole / actual if actual else 0.0,
                "averageHole": hole / alloc if alloc else 0.0,
                "periodAlloc": alloc,
                "periodRequested": requested,
                "periodHole": hole,
                "cumulativeHole": cumulative_hole,
                "cumulativeRequested": cumulative_requested,
                "periodFree": sum(
                    row.get("period_free", 0) for row in rows
                ),
                "liveAlloc": (
                    latest["live_alloc"] if live_supported else 0
                ),
                "liveRequested": (
                    latest["live_requested"] if live_supported else 0
                ),
                "liveAllocated": live_allocated,
                "liveHole": (
                    latest["live_hole"] if live_supported else 0
                ),
                "liveHoleRatio": (
                    latest["live_hole"] / live_allocated
                    if live_supported and live_allocated else 0.0
                ),
            }
        )

    for _, row in iter_rows(path, header, selected_pid):
        if not row:
            continue
        group.append(row)
        if len(group) >= group_size:
            finish_group(group)
            group = []
    finish_group(group)
    return points


def make_bucket_rows(
    summary: Dict[str, object],
    buckets: List[Tuple[str, str, str]],
) -> List[Dict[str, object]]:
    totals = summary["bucket_totals"]
    live_totals = summary["live"]["bucket_totals"]
    rows = []
    for label, _, _ in buckets:
        count = totals[label]["count"]
        hole = totals[label]["hole"]
        capacity = bucket_capacity(label)
        allocated = capacity * count if capacity is not None else None
        ratio = (
            hole / allocated
            if allocated is not None and allocated
            else None
        )
        live_count = live_totals[label]["count"]
        live_hole = live_totals[label]["hole"]
        live_allocated = (
            capacity * live_count if capacity is not None else None
        )
        live_ratio = (
            live_hole / live_allocated
            if live_allocated is not None and live_allocated
            else None
        )
        rows.append(
            {
                "label": label.replace("_plus", "+"),
                "rawLabel": label,
                "count": count,
                "hole": hole,
                "averageHole": hole / count if count else 0.0,
                "capacity": allocated,
                "requested": (
                    allocated - hole if allocated is not None else None
                ),
                "ratio": ratio,
                "liveCount": live_count,
                "liveHole": live_hole,
                "liveAverageHole": (
                    live_hole / live_count if live_count else 0.0
                ),
                "liveRatio": live_ratio,
            }
        )
    return rows


def json_for_html(value: object) -> str:
    return json.dumps(
        value, ensure_ascii=False, separators=(",", ":")
    ).replace("</", "<\\/")


def build_html(
    source_path: Path,
    title: str,
    metadata: Dict[str, str],
    summary: Dict[str, object],
    points: List[Dict[str, object]],
    bucket_rows: List[Dict[str, object]],
) -> str:
    payload = {
        "source": str(source_path),
        "metadata": metadata,
        "summary": summary,
        "points": points,
        "buckets": bucket_rows,
    }
    safe_title = html.escape(title)
    data_json = json_for_html(payload)
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>{safe_title}</title>
<style>
:root {{
  color-scheme: dark;
  --bg:#0b1020; --panel:#131a2b; --panel2:#182238; --line:#2a3651;
  --text:#eef3ff; --muted:#93a4bf; --blue:#53a7ff; --orange:#ff9b57;
  --green:#52d6a0; --red:#ff6b7a; --violet:#a98bff;
}}
* {{ box-sizing:border-box }}
body {{
  margin:0; background:
    radial-gradient(circle at 15% -10%,#1d335d 0,transparent 35%),
    radial-gradient(circle at 100% 0,#3a1f47 0,transparent 28%),var(--bg);
  color:var(--text); font:14px/1.5 Inter,ui-sans-serif,system-ui,-apple-system,
    BlinkMacSystemFont,"Segoe UI","Microsoft YaHei",sans-serif;
}}
.wrap {{ max-width:1500px; margin:auto; padding:28px }}
header {{ display:flex; gap:20px; justify-content:space-between; align-items:end;
  margin-bottom:22px }}
h1 {{ margin:0; font-size:28px; letter-spacing:.2px }}
.subtitle {{ color:var(--muted); margin-top:7px; word-break:break-all }}
.badge {{ border:1px solid #40547c; background:#15233d; color:#b9d8ff;
  padding:6px 10px; border-radius:999px; white-space:nowrap }}
.cards {{ display:grid; grid-template-columns:repeat(6,minmax(150px,1fr));
  gap:12px; margin-bottom:16px }}
.card,.panel {{ background:linear-gradient(145deg,rgba(24,34,56,.96),
  rgba(16,23,39,.96)); border:1px solid var(--line);
  box-shadow:0 14px 35px rgba(0,0,0,.18); border-radius:14px }}
.card {{ padding:16px }}
.label {{ color:var(--muted); font-size:12px; text-transform:uppercase;
  letter-spacing:.7px }}
.value {{ font-size:24px; font-weight:750; margin-top:4px }}
.hint {{ color:var(--muted); font-size:12px; margin-top:3px }}
.warning {{ display:none; margin:0 0 16px; padding:11px 14px;
  border:1px solid #8d6032; background:#3a2718; color:#ffd8a8;
  border-radius:10px }}
.grid {{ display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:16px }}
.panel {{ padding:17px; min-width:0 }}
.panel h2 {{ font-size:15px; margin:0 0 11px }}
.chart {{ position:relative; height:250px }}
canvas {{ width:100%; height:100%; display:block }}
.tooltip {{ display:none; position:absolute; pointer-events:none; z-index:4;
  background:#070b14ee; border:1px solid #40506d; border-radius:9px;
  padding:8px 10px; color:#eaf2ff; min-width:170px; box-shadow:0 8px 24px #0008 }}
.wide {{ grid-column:1/-1 }}
.bucket-grid {{ display:grid; grid-template-columns:minmax(0,1.1fr)
  minmax(400px,1fr); gap:18px }}
.bar-row {{ display:grid; grid-template-columns:72px 1fr 95px; gap:10px;
  align-items:center; margin:7px 0 }}
.bar-track {{ height:12px; background:#0d1424; border-radius:8px; overflow:hidden }}
.bar-fill {{ height:100%; border-radius:8px;
  background:linear-gradient(90deg,var(--orange),#ffd05b) }}
.num {{ text-align:right; font-variant-numeric:tabular-nums }}
.scroll {{ overflow:auto; max-height:470px; border:1px solid var(--line);
  border-radius:10px }}
table {{ border-collapse:collapse; width:100%; font-variant-numeric:tabular-nums }}
th,td {{ padding:9px 11px; border-bottom:1px solid #253049; text-align:right;
  white-space:nowrap }}
th:first-child,td:first-child {{ text-align:left }}
th {{ position:sticky; top:0; background:#182238; color:#b9c9e2; cursor:pointer }}
tr:hover td {{ background:#1b2740 }}
.empty {{ padding:35px; text-align:center; color:var(--muted) }}
footer {{ color:var(--muted); margin-top:18px; font-size:12px }}
@media (max-width:1100px) {{
  .cards {{ grid-template-columns:repeat(3,1fr) }}
  .bucket-grid {{ grid-template-columns:1fr }}
}}
@media (max-width:720px) {{
  .wrap {{ padding:16px }} header {{ align-items:start; flex-direction:column }}
  .cards {{ grid-template-columns:repeat(2,1fr) }}
  .grid {{ grid-template-columns:1fr }} .wide {{ grid-column:auto }}
}}
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div><h1>{safe_title}</h1><div class="subtitle" id="source"></div></div>
    <div class="badge" id="formatBadge"></div>
  </header>
  <section class="cards" id="cards"></section>
  <div class="warning" id="warning"></div>
  <section class="grid">
    <div class="panel"><h2 id="primaryHoleTitle">空洞产生速率</h2><div class="chart">
      <canvas id="holeRate"></canvas><div class="tooltip"></div></div></div>
    <div class="panel"><h2 id="primaryRatioTitle">周期空洞率</h2><div class="chart">
      <canvas id="holeRatio"></canvas><div class="tooltip"></div></div></div>
    <div class="panel"><h2>分配次数速率</h2><div class="chart">
      <canvas id="allocRate"></canvas><div class="tooltip"></div></div></div>
    <div class="panel"><h2>累计产生空洞</h2><div class="chart">
      <canvas id="cumulativeHole"></canvas><div class="tooltip"></div></div></div>
    <div class="panel wide">
      <h2 id="bucketTitle">Size class 空洞贡献</h2>
      <div class="bucket-grid">
        <div id="bucketBars"></div>
        <div class="scroll"><table id="bucketTable">
          <thead></thead><tbody></tbody>
        </table></div>
      </div>
    </div>
    <div class="panel wide"><h2>最近聚合时间点</h2>
      <div class="scroll"><table id="timeTable">
        <thead></thead><tbody></tbody></table></div>
    </div>
  </section>
  <footer id="footer"></footer>
</div>
<script>
const D={data_json};
const S=D.summary, P=D.points, B=D.buckets;
const hasLive=!!S.live_supported, L=S.live;
const liveEstimated=D.metadata.live_values==="estimated";
const nf=new Intl.NumberFormat("zh-CN",{{maximumFractionDigits:2}});
function bytes(v){{
  if(!Number.isFinite(v)) return "—";
  const u=["B","KiB","MiB","GiB","TiB","PiB"]; let i=0,n=Math.abs(v);
  while(n>=1024&&i<u.length-1){{n/=1024;i++}}
  return (v<0?"-":"")+nf.format(n)+" "+u[i];
}}
function rate(v){{return bytes(v)+"/s"}}
function pct(v){{return Number.isFinite(v)?nf.format(v*100)+"%":"—"}}
function integer(v){{return new Intl.NumberFormat("zh-CN").format(v||0)}}
function timeNs(v){{return v?new Date(v/1e6).toLocaleString():"—"}}
document.getElementById("source").textContent=D.source;
document.getElementById("formatBadge").textContent=
  (D.metadata.format||"unknown")+" · "+S.row_count+" 行";
const cards=hasLive?[
  [liveEstimated?"估算存活空洞":"当前存活空洞",
    bytes(L.hole_bytes),"采样时刻仍存活"],
  [liveEstimated?"估算存活空洞率":"当前存活空洞率",
    pct(L.hole_ratio),"live hole / live allocated"],
  ["当前存活实际占用",bytes(L.allocated_bytes),"requested + hole"],
  ["当前存活请求",bytes(L.requested_bytes),"进程启动后新分配"],
  ["当前存活分配",integer(L.allocations),"尚未释放的跟踪块"],
  ["未跟踪释放",integer(L.total_untracked_free),
    "继承指针或跟踪失败"],
]:[
  ["总空洞",bytes(S.totals.period_hole),"累计内部碎片"],
  ["总体空洞率",pct(S.hole_ratio),"hole / (requested + hole)"],
  ["请求字节",bytes(S.totals.period_requested),"用户实际请求"],
  ["分配次数",integer(S.totals.period_alloc),"成功分配"],
  ["平均每次空洞",bytes(S.average_hole),"按 measured 计算"],
  ["进程",S.pids.length?S.pids.join(", "):"—",
    S.pids.length>1?S.pids.length+" 个 PID":"日志 PID"],
];
document.getElementById("cards").innerHTML=cards.map(c=>
  `<div class="card"><div class="label">${{c[0]}}</div>
   <div class="value">${{c[1]}}</div><div class="hint">${{c[2]}}</div></div>`
).join("");
if(hasLive&&L.total_tracking_failures>0){{
  const warning=document.getElementById("warning");
  warning.style.display="block";
  warning.textContent="存活表跟踪失败 "+integer(L.total_tracking_failures)+
    " 次；当前存活值存在低估，请增大 MINI_HOLE_LIVE_CAPACITY 或启用采样。";
}}

function chart(id,key,color,format){{
  const canvas=document.getElementById(id), tip=canvas.nextElementSibling;
  const parent=canvas.parentElement;
  function draw(){{
    const r=parent.getBoundingClientRect(),dpr=devicePixelRatio||1;
    canvas.width=Math.max(1,r.width*dpr); canvas.height=Math.max(1,r.height*dpr);
    const c=canvas.getContext("2d"); c.scale(dpr,dpr);
    const w=r.width,h=r.height,pad={{l:58,r:15,t:12,b:30}};
    c.clearRect(0,0,w,h);
    if(!P.length){{c.fillStyle="#93a4bf";c.fillText("暂无周期数据",20,35);return}}
    const vals=P.map(x=>Number(x[key])||0), rawMax=Math.max(...vals);
    const max=rawMax>0?rawMax:1;
    c.strokeStyle="#2a3651";c.fillStyle="#8496b3";c.font="11px system-ui";
    for(let i=0;i<=4;i++){{const y=pad.t+(h-pad.t-pad.b)*i/4;
      c.beginPath();c.moveTo(pad.l,y);c.lineTo(w-pad.r,y);c.stroke();
      const val=max*(1-i/4);c.fillText(format(val),4,y+4)}}
    const x=i=>pad.l+(w-pad.l-pad.r)*(P.length===1?.5:i/(P.length-1));
    const y=v=>h-pad.b-(h-pad.t-pad.b)*v/max;
    c.beginPath(); vals.forEach((v,i)=>i?c.lineTo(x(i),y(v)):c.moveTo(x(i),y(v)));
    c.strokeStyle=color;c.lineWidth=2;c.stroke();
    c.lineTo(x(vals.length-1),h-pad.b);c.lineTo(x(0),h-pad.b);c.closePath();
    const g=c.createLinearGradient(0,pad.t,0,h-pad.b);
    g.addColorStop(0,color+"55");g.addColorStop(1,color+"05");c.fillStyle=g;c.fill();
    canvas._geom={{x,vals,pad,w,h}};
  }}
  canvas.addEventListener("mousemove",e=>{{
    if(!P.length||!canvas._geom)return;
    const r=canvas.getBoundingClientRect(),g=canvas._geom;
    let i=P.length===1?0:Math.round((e.clientX-r.left-g.pad.l)/
      (g.w-g.pad.l-g.pad.r)*(P.length-1));
    i=Math.max(0,Math.min(P.length-1,i));const p=P[i];
    tip.style.display="block";tip.style.left=Math.min(e.offsetX+12,r.width-190)+"px";
    tip.style.top=Math.max(8,e.offsetY-55)+"px";
    tip.innerHTML=`<b>${{timeNs(p.endNs)}}</b><br>${{format(p[key])}}`;
  }});
  canvas.addEventListener("mouseleave",()=>tip.style.display="none");
  new ResizeObserver(draw).observe(parent);draw();
}}
if(hasLive){{
  document.getElementById("primaryHoleTitle").textContent=
    liveEstimated?"估算存活空洞":"当前存活空洞";
  document.getElementById("primaryRatioTitle").textContent=
    liveEstimated?"估算存活空洞率":"当前存活空洞率";
}}
chart("holeRate",hasLive?"liveHole":"holeRate","#ff9b57",
  hasLive?bytes:rate);
chart("holeRatio",hasLive?"liveHoleRatio":"holeRatio","#a98bff",pct);
chart("allocRate","allocRate","#53a7ff",v=>nf.format(v)+"/s");
chart("cumulativeHole","cumulativeHole","#52d6a0",bytes);

const BK=hasLive?{{
  count:"liveCount",hole:"liveHole",average:"liveAverageHole",
  ratio:"liveRatio"
}}:{{count:"count",hole:"hole",average:"averageHole",ratio:"ratio"}};
document.getElementById("bucketTitle").textContent=hasLive?
  (liveEstimated?"估算存活 Size class 空洞":"当前存活 Size class 空洞"):
  "Size class 空洞贡献";
document.querySelector("#bucketTable thead").innerHTML=
  `<tr><th data-key="label">桶</th><th data-key="${{BK.count}}">${{
    hasLive?"存活次数":"次数"}}</th>
   <th data-key="${{BK.hole}}">${{hasLive?"存活空洞":"总空洞"}}</th>
   <th data-key="${{BK.average}}">平均空洞</th>
   <th data-key="${{BK.ratio}}">桶内空洞率</th></tr>`;
function renderBuckets(rows){{
  const ranked=[...rows]
    .filter(x=>x[BK.count]>0||x[BK.hole]>0)
    .sort((a,b)=>b[BK.hole]-a[BK.hole]);
  const top=ranked.slice(0,12);
  const max=Math.max(...top.map(x=>x[BK.hole]),1);
  document.getElementById("bucketBars").innerHTML=top.length?top.map(x=>{{
    const width=Math.sqrt(x[BK.hole]/max)*100;
    return `<div class="bar-row"><span>${{x.label}}</span>
      <div class="bar-track"><div class="bar-fill" style="width:${{width}}%"></div></div>
      <span class="num">${{bytes(x[BK.hole])}}</span></div>`;
  }}).join(""):`<div class="empty">没有桶数据</div>`;
  const body=document.querySelector("#bucketTable tbody");
  body.innerHTML=rows.map(x=>`<tr><td>${{x.label}}</td>
    <td>${{integer(x[BK.count])}}</td>
    <td>${{bytes(x[BK.hole])}}</td><td>${{bytes(x[BK.average])}}</td>
    <td>${{x[BK.ratio]===null?"—":pct(x[BK.ratio])}}</td></tr>`).join("");
}}
let bucketSort={{key:null,desc:false}};
function sortBuckets(key){{
  if(bucketSort.key===key)bucketSort.desc=!bucketSort.desc;
  else bucketSort={{key,desc:key!=="label"}};
  const rows=[...B].sort((a,b)=>{{
    const av=a[key]??-1,bv=b[key]??-1;
    return (typeof av==="string"?av.localeCompare(bv):av-bv)*(bucketSort.desc?-1:1);
  }});renderBuckets(rows);
}}
document.querySelectorAll("#bucketTable th[data-key]").forEach(th=>
  th.addEventListener("click",()=>sortBuckets(th.dataset.key)));
sortBuckets(BK.hole);

const timeTable=document.getElementById("timeTable");
timeTable.querySelector("thead").innerHTML=hasLive?
  `<tr><th>结束时间</th><th>存活分配</th><th>存活请求</th>
   <th>存活空洞</th><th>存活空洞率</th><th>周期分配</th><th>周期释放</th></tr>`:
  `<tr><th>结束时间</th><th>覆盖行数</th><th>分配次数</th>
   <th>请求量</th><th>空洞量</th><th>空洞率</th></tr>`;
timeTable.querySelector("tbody").innerHTML=P.slice(-20).reverse().map(p=>
  hasLive?
  `<tr><td>${{timeNs(p.endNs)}}</td><td>${{integer(p.liveAlloc)}}</td>
   <td>${{bytes(p.liveRequested)}}</td><td>${{bytes(p.liveHole)}}</td>
   <td>${{pct(p.liveHoleRatio)}}</td><td>${{integer(p.periodAlloc)}}</td>
   <td>${{integer(p.periodFree)}}</td></tr>`:
  `<tr><td>${{timeNs(p.endNs)}}</td><td>${{integer(p.rows)}}</td>
   <td>${{integer(p.periodAlloc)}}</td><td>${{bytes(p.periodRequested)}}</td>
   <td>${{bytes(p.periodHole)}}</td><td>${{pct(p.holeRatio)}}</td></tr>`
).join("") || `<tr><td colspan="${{hasLive?7:6}}" class="empty">
  CSV 目前只有表头，没有周期数据</td></tr>`;
document.getElementById("footer").textContent=
  `时间范围：${{timeNs(S.start_ns)}} — ${{timeNs(S.end_ns)}} · `+
  `原始数据行 ${{S.row_count}} · 图表点 ${{P.length}} · `+
  `跳过异常行 ${{S.malformed_rows}}`+
  (hasLive?" · 存活口径：进程启动后新分配，继承内存不计":"")+
  (liveEstimated?" · 采样估算 1/"+D.metadata.live_sample_rate:"");
</script>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(
        description="把 mini malloc hole CSV 转成自包含 HTML 仪表盘"
    )
    parser.add_argument("csv_path", type=Path, help="输入 CSV")
    parser.add_argument(
        "-o", "--output", type=Path,
        help="输出 HTML；默认与 CSV 同目录同名",
    )
    parser.add_argument(
        "--max-points", type=int, default=1500,
        help="时间图最多保留的聚合点数（默认 1500）",
    )
    parser.add_argument(
        "--pid", type=int,
        help="共享 CSV 只分析指定 PID",
    )
    parser.add_argument(
        "--title", default="Mini 内存空洞分析",
        help="报告标题",
    )
    parser.add_argument(
        "--summary-json", type=Path,
        help="可选：同时输出机器可读汇总 JSON",
    )
    args = parser.parse_args()

    path = args.csv_path.resolve()
    if not path.is_file():
        parser.error(f"CSV 不存在：{path}")
    if args.max_points < 50:
        parser.error("--max-points 必须 >= 50")

    metadata, header = read_preamble(path)
    if args.pid is not None and "pid" not in header:
        metadata_pid = metadata.get("pid")
        if metadata_pid != str(args.pid):
            parser.error(
                f"该单 PID CSV 的 PID 是 {metadata_pid}，不是 {args.pid}"
            )
    buckets = discover_buckets(header)
    summary = first_pass(
        path, metadata, header, buckets, args.pid
    )
    points = aggregate_points(
        path, header, args.pid,
        int(summary["row_count"]), args.max_points,
    )
    bucket_rows = make_bucket_rows(summary, buckets)

    output = (
        args.output.resolve()
        if args.output
        else path.with_suffix(".html")
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    report = build_html(
        path, args.title, metadata, summary, points, bucket_rows
    )
    output.write_text(report, encoding="utf-8")

    if args.summary_json:
        summary_path = args.summary_json.resolve()
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(
            json.dumps(
                {
                    "source": str(path),
                    "metadata": metadata,
                    "summary": summary,
                    "buckets": bucket_rows,
                },
                ensure_ascii=False,
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )

    totals = summary["totals"]
    print(f"报告：{output}")
    print(
        "汇总："
        f"alloc={totals['period_alloc']} "
        f"requested={totals['period_requested']} "
        f"hole={totals['period_hole']} "
        f"hole_ratio={summary['hole_ratio'] * 100:.4f}% "
        f"rows={summary['row_count']} points={len(points)}"
    )
    if summary["live_supported"]:
        live = summary["live"]
        print(
            "存活："
            f"alloc={live['allocations']} "
            f"requested={live['requested_bytes']} "
            f"allocated={live['allocated_bytes']} "
            f"hole={live['hole_bytes']} "
            f"hole_ratio={live['hole_ratio'] * 100:.4f}% "
            f"untracked_free={live['total_untracked_free']} "
            f"track_failed={live['total_tracking_failures']}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
