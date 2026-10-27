import json
import sqlite3
import threading
import time
from pathlib import Path

FLOATS = ["lpm", "total_l", "vib_x", "vib_y", "vib_z", "vib_rms", "vib_peak", "vib_dom_hz",
          "vib_b0", "vib_b1", "vib_b2", "vib_b3", "mic_db", "mic_centroid",
          "mic_b0", "mic_b1", "mic_b2", "mic_b3", "mic_b4"]
INTS = ["ts", "rx", "seq", "ms", "time_ok", "pulses", "flow_active", "rssi", "heap", "up_s",
        "reconnects", "mpu_ok", "mic_ok", "buf", "drops"]
COLS = ["ts", "rx", "unit", "seq", "ms", "time_ok", "lpm", "total_l", "pulses", "flow_active",
        "vib_x", "vib_y", "vib_z", "vib_rms", "vib_peak", "vib_dom_hz",
        "vib_b0", "vib_b1", "vib_b2", "vib_b3",
        "mic_db", "mic_centroid", "mic_b0", "mic_b1", "mic_b2", "mic_b3", "mic_b4", "mic_spec",
        "rssi", "heap", "up_s", "reconnects", "mpu_ok", "mic_ok", "buf", "drops"]


def _type(c):
    if c in INTS:
        return "INTEGER"
    if c in FLOATS:
        return "REAL"
    return "TEXT"


SCHEMA = (
    "CREATE TABLE IF NOT EXISTS readings (id INTEGER PRIMARY KEY AUTOINCREMENT, "
    + ", ".join(f"{c} {_type(c)}" for c in COLS) + ");"
    "CREATE INDEX IF NOT EXISTS idx_readings_ts ON readings(ts);"
    "CREATE INDEX IF NOT EXISTS idx_readings_unit_ts ON readings(unit, ts);"
    "CREATE TABLE IF NOT EXISTS events (id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, state TEXT, "
    "prev_state TEXT, score REAL, s_flow REAL, s_ac REAL, mismatch REAL, location TEXT, detail TEXT);"
    "CREATE TABLE IF NOT EXISTS kv (key TEXT PRIMARY KEY, value TEXT);"
)


def _at(lst, i):
    try:
        return lst[i]
    except (TypeError, IndexError, KeyError):
        return None


def flatten(m: dict, rx_ms: int) -> tuple:
    flow, vib, mic, sysd = (m.get(k) or {} for k in ("flow", "vib", "mic", "sys"))
    rms = vib.get("rms") or []
    vb, mb = vib.get("bands") or [], mic.get("bands") or []
    vib_total = None
    if len(rms) == 3:
        vib_total = (sum(float(x) ** 2 for x in rms)) ** 0.5
    ts = int(m.get("ts") or 0) if m.get("time_ok") else 0
    row = {
        "ts": ts or rx_ms, "rx": rx_ms, "unit": m["unit"], "seq": m.get("seq"), "ms": m.get("ms"),
        "time_ok": int(bool(m.get("time_ok"))),
        "lpm": flow.get("lpm"), "total_l": flow.get("total_l"), "pulses": flow.get("pulses"),
        "flow_active": int(bool(flow.get("active"))),
        "vib_x": _at(rms, 0), "vib_y": _at(rms, 1), "vib_z": _at(rms, 2), "vib_rms": vib_total,
        "vib_peak": vib.get("peak"), "vib_dom_hz": vib.get("dom_hz"),
        "vib_b0": _at(vb, 0), "vib_b1": _at(vb, 1), "vib_b2": _at(vb, 2), "vib_b3": _at(vb, 3),
        "mic_db": mic.get("db"), "mic_centroid": mic.get("centroid"),
        "mic_b0": _at(mb, 0), "mic_b1": _at(mb, 1), "mic_b2": _at(mb, 2), "mic_b3": _at(mb, 3), "mic_b4": _at(mb, 4),
        "mic_spec": json.dumps(mic.get("spec") or []),
        "rssi": sysd.get("rssi"), "heap": sysd.get("heap"), "up_s": sysd.get("up_s"),
        "reconnects": sysd.get("reconnects"),
        "mpu_ok": int(bool(vib.get("ok"))), "mic_ok": int(bool(mic.get("ok"))),
        "buf": sysd.get("buf"), "drops": sysd.get("drops"),
    }
    return tuple(row[c] for c in COLS)


class DB:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = str(path)
        self.lock = threading.Lock()
        self.con = sqlite3.connect(self.path, check_same_thread=False)
        self.con.execute("PRAGMA journal_mode=WAL")
        self.con.execute("PRAGMA synchronous=NORMAL")
        self.con.executescript(SCHEMA)
        self.con.commit()

    def reader(self) -> sqlite3.Connection:
        con = sqlite3.connect(self.path)
        con.row_factory = sqlite3.Row
        return con

    def insert_reading(self, m: dict, rx_ms: int) -> None:
        q = f"INSERT INTO readings ({','.join(COLS)}) VALUES ({','.join('?' * len(COLS))})"
        with self.lock:
            self.con.execute(q, flatten(m, rx_ms))
            self.con.commit()

    def insert_event(self, ev: dict) -> None:
        with self.lock:
            self.con.execute(
                "INSERT INTO events (ts,state,prev_state,score,s_flow,s_ac,mismatch,location,detail) VALUES (?,?,?,?,?,?,?,?,?)",
                (int(time.time() * 1000), ev["state"], ev["prev"], ev["score"], ev["s_flow"], ev["s_ac"],
                 ev["mismatch"], ev["location"], ev.get("detail", "")))
            self.con.commit()

    def kv_get(self, key: str):
        with self.lock:
            r = self.con.execute("SELECT value FROM kv WHERE key=?", (key,)).fetchone()
        return r[0] if r else None

    def kv_set(self, key: str, value: str) -> None:
        with self.lock:
            self.con.execute("INSERT INTO kv(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))
            self.con.commit()

    def prune(self, days: float) -> int:
        cutoff = int((time.time() - days * 86400) * 1000)
        with self.lock:
            n = self.con.execute("DELETE FROM readings WHERE rx < ?", (cutoff,)).rowcount
            self.con.commit()
        return n

