import asyncio
import csv
import io
import json
import logging
import re
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles

from db import DB
from detector import DEFAULTS, Detector
from logsetup import setup_logging

BASE = Path(__file__).parent
setup_logging(BASE / "logs")
log = logging.getLogger("app")
evlog = logging.getLogger("leak.events")

db = DB(BASE / "data" / "leakdata.db")
CFG_PATH = BASE / "data" / "config.json"


def load_cfg() -> dict:
    cfg = dict(DEFAULTS)
    try:
        cfg.update({k: v for k, v in json.loads(CFG_PATH.read_text()).items() if k in DEFAULTS})
    except FileNotFoundError:
        pass
    except Exception as e:  # corrupt file: keep defaults, say so
        log.error("config.json unreadable (%s) - using defaults", e)
    return cfg


det = Detector(load_cfg())
_bl = db.kv_get("baseline")
if _bl:
    det.set_baseline(json.loads(_bl))
    log.info("baseline loaded from DB")

units: dict[str, dict] = {}
clients: set[WebSocket] = set()
latest_status: dict = {}
UNIT_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")


# ------------------------------------------------------------------ helpers
async def broadcast(obj: dict) -> None:
    if not clients:
        return
    txt = json.dumps(obj)
    dead = []
    for ws in list(clients):
        try:
            await ws.send_text(txt)
        except Exception:
            dead.append(ws)
    for ws in dead:
        clients.discard(ws)


def validate(m) -> None:
    if not isinstance(m, dict):
        raise ValueError("message is not an object")
    if not isinstance(m.get("unit"), str) or not UNIT_RE.match(m["unit"]):
        raise ValueError("bad or missing 'unit'")
    for k in ("flow", "vib", "mic", "sys"):
        if not isinstance(m.get(k), dict):
            raise ValueError(f"missing '{k}' object")


def unit_summaries(now: float) -> list[dict]:
    out = []
    for name, u in sorted(units.items()):
        m = u.get("last") or {}
        out.append({
            "unit": name,
            "online": now - u["last_seen"] < det.cfg["unit_timeout_s"],
            "last_seen_s": round(now - u["last_seen"], 1),
            "msgs": u["count"], "fw": m.get("fw"),
            "rssi": (m.get("sys") or {}).get("rssi"), "heap": (m.get("sys") or {}).get("heap"),
            "up_s": (m.get("sys") or {}).get("up_s"), "reconnects": (m.get("sys") or {}).get("reconnects"),
            "buf": (m.get("sys") or {}).get("buf"), "drops": (m.get("sys") or {}).get("drops"),
            "mpu_ok": (m.get("vib") or {}).get("ok"), "mic_ok": (m.get("mic") or {}).get("ok"),
            "lpm": (m.get("flow") or {}).get("lpm"), "flow_active": (m.get("flow") or {}).get("active"),
            "clock_skew_s": u.get("skew"),
        })
    return out


def finish_baseline() -> tuple[bool, str]:
    bl, msg = det.stop_baseline()
    if bl is None:
        log.warning("baseline failed: %s", msg)
        return False, msg
    db.kv_set("baseline", json.dumps(bl))
    log.info("baseline stored: flow_bias=%.3f L/min flow_sd=%.3f units=%s",
             bl["flow_bias"], bl["flow_sd"], {u: round(v["mic_mu"], 1) for u, v in bl["units"].items()})
    return True, "baseline stored"


# ------------------------------------------------------------------ background ticker
async def ticker():
    was_online: dict[str, bool] = {}
    last_prune = 0.0
    while True:
        await asyncio.sleep(1.0)
        try:
            now = time.time()
            for name, u in units.items():
                online = now - u["last_seen"] < det.cfg["unit_timeout_s"]
                if name in was_online and online != was_online[name]:
                    (log.info if online else log.warning)(
                        "unit %s: %s (last seen %.1f s ago)", name, "ONLINE" if online else "OFFLINE", now - u["last_seen"])
                was_online[name] = online

            if det.calibrating and now - det.cal_start >= det.cfg["baseline_minutes"] * 60:
                finish_baseline()

            st = det.evaluate(now)
            ev = st.pop("event", None)
            if ev:
                db.insert_event(ev)
                evlog.warning("STATE %s->%s score=%.2f s_flow=%.2f s_ac=%.2f mismatch=%s L/min loc=%s",
                              ev["prev"], ev["state"], ev["score"], ev["s_flow"], ev["s_ac"],
                              "n/a" if ev["mismatch"] is None else f"{ev['mismatch']:.2f}", ev["location"])
            st["type"] = "status"
            st["ts"] = int(now * 1000)
            st["units"] = unit_summaries(now)
            st["calibrating"] = det.calibrating
            st["cal_elapsed_s"] = round(now - det.cal_start, 0) if det.calibrating else 0
            st["cal_total_s"] = det.cfg["baseline_minutes"] * 60
            latest_status.clear()
            latest_status.update(st)
            await broadcast(st)

            if now - last_prune > 3600:
                last_prune = now
                n = db.prune(det.cfg["retention_days"])
                if n:
                    log.info("pruned %d old readings", n)
        except Exception:
            log.exception("ticker error")


@asynccontextmanager
async def lifespan(app: FastAPI):
    task = asyncio.create_task(ticker())
    log.info("server started")
    yield
    task.cancel()


app = FastAPI(title="Leak Monitor", lifespan=lifespan)


# ------------------------------------------------------------------ ingest
@app.post("/ingest")
async def ingest(request: Request):
    rx = time.time()
    try:
        body = await request.json()
    except Exception as e:
        log.warning("ingest parse error from=%s err=%s", request.client.host, e)
        raise HTTPException(400, "bad json")
    msgs = body if isinstance(body, list) else [body]
    accepted = 0
    for m in msgs:
        try:
            validate(m)
        except ValueError as e:
            log.warning("ingest rejected from=%s err=%s", request.client.host, e)
            continue
        name = m["unit"]
        u = units.get(name)
        if u is None:
            u = units[name] = {"count": 0, "first_seen": rx}
            log.info("unit first seen: %s from %s fw=%s", name, request.client.host, m.get("fw"))
        u["count"] += 1
        u["last_seen"] = rx
        u["last"] = m
        if m.get("time_ok") and m.get("ts"):
            u["skew"] = round(rx - m["ts"] / 1000.0, 2)           # + means device clock is behind
        age = u.get("skew") if u.get("skew") is not None else 0
        late = age is not None and abs(age) > 30                  # buffered replay (or big clock skew)
        try:
            db.insert_reading(m, int(rx * 1000))
        except Exception:
            log.exception("db insert failed")
            continue
        if not late:
            det.ingest(m, rx)                                      # server clock is authoritative live
        m["t"] = int(rx * 1000)
        await broadcast({"type": "sample", **m})
        accepted += 1
    if accepted == 0:
        raise HTTPException(400, "no valid messages")
    return {"ok": True, "accepted": accepted}


# ------------------------------------------------------------------ REST
@app.get("/health")
def health():
    return {"ok": True, "time": time.time()}


@app.get("/api/units")
def api_units():
    return unit_summaries(time.time())


@app.get("/api/status")
def api_status():
    return latest_status


@app.get("/api/history")
def api_history(unit: str | None = None, minutes: float = 10, limit: int = 2000):
    limit = max(1, min(limit, 20000))
    since = int((time.time() - minutes * 60) * 1000)
    sql, args = "SELECT * FROM readings WHERE rx >= ?", [since]
    if unit:
        sql += " AND unit = ?"
        args.append(unit)
    sql += " ORDER BY rx DESC LIMIT ?"
    args.append(limit)
    con = db.reader()
    try:
        rows = [dict(r) for r in con.execute(sql, args)]
    finally:
        con.close()
    rows.reverse()
    return rows


@app.get("/api/export.csv")
def api_export(minutes: float = 0, unit: str | None = None):
    sql, args = "SELECT datetime(rx/1000,'unixepoch') AS rx_utc, * FROM readings WHERE 1=1", []
    if minutes > 0:
        sql += " AND rx >= ?"
        args.append(int((time.time() - minutes * 60) * 1000))
    if unit:
        sql += " AND unit = ?"
        args.append(unit)
    sql += " ORDER BY rx"

    def gen():
        con = db.reader()
        try:
            cur = con.execute(sql, args)
            cols = [d[0] for d in cur.description]
            buf = io.StringIO()
            w = csv.writer(buf)
            w.writerow(cols)
            yield buf.getvalue()
            while True:
                rows = cur.fetchmany(2000)
                if not rows:
                    break
                buf.seek(0)
                buf.truncate()
                w.writerows([tuple(r) for r in rows])
                yield buf.getvalue()
        finally:
            con.close()

    fn = time.strftime("leak_readings_%Y%m%d_%H%M%S.csv", time.gmtime())
    return StreamingResponse(gen(), media_type="text/csv", headers={"Content-Disposition": f'attachment; filename="{fn}"'})


@app.get("/api/events")
def api_events(limit: int = 50):
    con = db.reader()
    try:
        return [dict(r) for r in con.execute("SELECT * FROM events ORDER BY id DESC LIMIT ?", (max(1, min(limit, 500)),))]
    finally:
        con.close()


@app.get("/api/config")
def api_get_config():
    return det.cfg


@app.post("/api/config")
async def api_set_config(request: Request):
    body = await request.json()
    if not isinstance(body, dict):
        raise HTTPException(400, "object expected")
    changed = {}
    for k, v in body.items():
        if k not in DEFAULTS:
            raise HTTPException(400, f"unknown key {k}")
        try:
            changed[k] = float(v)
        except (TypeError, ValueError):
            raise HTTPException(400, f"{k} must be a number")
    det.cfg.update(changed)
    CFG_PATH.write_text(json.dumps(det.cfg, indent=2))
    log.info("config updated: %s", changed)
    return det.cfg


@app.post("/api/baseline/start")
def api_baseline_start():
    det.start_baseline(time.time())
    log.info("baseline calibration STARTED (%.1f min)", det.cfg["baseline_minutes"])
    return {"ok": True}


@app.post("/api/baseline/stop")
def api_baseline_stop():
    if not det.calibrating:
        raise HTTPException(400, "not calibrating")
    ok, msg = finish_baseline()
    return {"ok": ok, "message": msg}


# ------------------------------------------------------------------ WebSocket + static
@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    clients.add(ws)
    log.info("dashboard connected (%d clients)", len(clients))
    try:
        if latest_status:
            await ws.send_text(json.dumps(latest_status))
        while True:
            await ws.receive_text()                 # keep-alive; client sends nothing important
    except WebSocketDisconnect:
        pass
    finally:
        clients.discard(ws)
        log.info("dashboard disconnected (%d clients)", len(clients))


app.mount("/static", StaticFiles(directory=BASE / "static"), name="static")


@app.get("/")
def index():
    return FileResponse(BASE / "static" / "index.html")

