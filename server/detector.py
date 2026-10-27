import logging
import math
import statistics
import time
from collections import deque

log = logging.getLogger("detector")

SRC, SNK = "unit_source", "unit_sink"

# All values editable at runtime from the dashboard (persisted in data/config.json).
DEFAULTS = {
    # --- flow rule
    "flow_window_s": 8.0,          # averaging window for flow comparison
    "transit_volume_l": 0.04,      # water volume between the two flowmeters (hose + T + meter bodies)
    "min_flow_lpm": 1.0,           # flowmeter reliable minimum; rules are idle below this
    "flow_abs_thr_lpm": 0.30,      # minimum mismatch considered a leak (sensor tolerance guard)
    "flow_rel_thr": 0.10,          # ... or this fraction of the source flow, whichever is larger
    # --- acoustic / vibration rule
    "z_thr": 4.0,                  # z-score treated as "clearly anomalous" (score reaches 0.5)
    "min_sd_db": 1.0,              # floor for baseline std-dev (dB)
    # --- combination and state machine
    "w_flow": 0.65, "w_ac": 0.35,
    "suspect_on": 0.30, "suspect_off": 0.15, "leak_on": 0.60, "leak_off": 0.40,
    "suspect_hold_s": 3.0, "leak_hold_s": 4.0, "clear_s": 8.0,
    # --- localisation
    "loc_min_excess_db": 3.0, "loc_band": 0.15,
    # --- housekeeping
    "baseline_minutes": 3.0, "unit_timeout_s": 5.0, "retention_days": 7.0,
}


def clip(x, lo=0.0, hi=1.0):
    return max(lo, min(hi, x))


def feats(m: dict) -> dict:
    """Detector features from one message; None when the sensor reports not-ok."""
    mic, vib = m.get("mic") or {}, m.get("vib") or {}
    mic_hf = vib_db = None
    b = mic.get("bands") or []
    if mic.get("ok") and len(b) >= 4:
        mic_hf = 10 * math.log10(sum(10 ** (x / 10) for x in b[1:4]) + 1e-20)      # 500 Hz - 4 kHz
    r = vib.get("rms") or []
    if vib.get("ok") and len(r) == 3:
        vib_db = 20 * math.log10(math.sqrt(sum(float(x) ** 2 for x in r)) + 1e-6)
    return {"mic": mic_hf, "vib": vib_db}


class Detector:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.flow = {SRC: deque(maxlen=900), SNK: deque(maxlen=900)}   # (t, lpm)
        self.last: dict = {}
        self.baseline = None
        self.calibrating = False
        self.cal_start = 0.0
        self.cal_ac: dict = {}
        self.cal_mm: list = []
        self.state = "NORMAL"
        self.t_up = None
        self.t_down = None

    # ------------------------------------------------------------ baseline
    def set_baseline(self, b):
        self.baseline = b

    def start_baseline(self, now: float):
        self.calibrating = True
        self.cal_start = now
        self.cal_ac = {u: {"mic": [], "vib": []} for u in (SRC, SNK)}
        self.cal_mm = []
        self.state, self.t_up, self.t_down = "NORMAL", None, None

    def stop_baseline(self):
        self.calibrating = False
        units = {}
        for u, d in self.cal_ac.items():
            if min(len(d["mic"]), len(d["vib"])) < 20:
                return None, (f"not enough samples for {u} (mic={len(d['mic'])}, vib={len(d['vib'])}). "
                              "Unit must be online, sensors OK and water flowing >= min_flow_lpm.")
            units[u] = {"mic_mu": statistics.fmean(d["mic"]), "mic_sd": statistics.pstdev(d["mic"]),
                        "vib_mu": statistics.fmean(d["vib"]), "vib_sd": statistics.pstdev(d["vib"]), "n": len(d["mic"])}
        if len(self.cal_mm) < 10:
            return None, "not enough two-flowmeter samples (are both units online with flow >= min_flow_lpm?)"
        self.baseline = {"units": units, "flow_bias": statistics.fmean(self.cal_mm),
                         "flow_sd": statistics.pstdev(self.cal_mm), "created": time.time()}
        return self.baseline, "ok"

    # ------------------------------------------------------------ data in
    def ingest(self, m: dict, t: float):
        u = m["unit"]
        lpm = (m.get("flow") or {}).get("lpm")
        f = feats(m)
        if u in self.flow and lpm is not None:
            self.flow[u].append((t, float(lpm)))
        self.last[u] = {"t": t, "lpm": lpm, **f}
        if self.calibrating and u in self.cal_ac and self._water(t):
            if f["mic"] is not None and f["vib"] is not None:
                self.cal_ac[u]["mic"].append(f["mic"])
                self.cal_ac[u]["vib"].append(f["vib"])

    def _water(self, now: float) -> bool:
        for L in self.last.values():
            if now - L["t"] < 3 and (L["lpm"] or 0) >= self.cfg["min_flow_lpm"]:
                return True
        return False

    def _avg(self, u: str, t_end: float, window: float):
        pts = [q for (t, q) in self.flow[u] if t_end - window < t <= t_end]
        return sum(pts) / len(pts) if len(pts) >= 3 else None

    # ------------------------------------------------------------ evaluation (1 Hz)
    def evaluate(self, now: float) -> dict:
        c = self.cfg
        w = c["flow_window_s"]
        q_raw = self._avg(SRC, now, w)
        delay = clip(c["transit_volume_l"] / max(q_raw, 0.5) * 60.0, 0.0, 5.0) if q_raw else 0.0
        qs = self._avg(SRC, now - delay, w)
        qk = self._avg(SNK, now, w)

        bias = self.baseline["flow_bias"] if self.baseline else 0.0
        mismatch = thr = None
        s_flow, flow_on = 0.0, False
        if qs is not None and qk is not None:
            mismatch = qs - qk - bias
            thr = max(c["flow_abs_thr_lpm"], c["flow_rel_thr"] * qs)
            if qs >= c["min_flow_lpm"]:
                s_flow = clip(mismatch / (2 * thr))
                flow_on = mismatch > thr
                if self.calibrating:
                    self.cal_mm.append(qs - qk)       # raw (bias not subtracted) for the baseline
        water = (qs is not None and qs >= c["min_flow_lpm"]) or (qk is not None and qk >= c["min_flow_lpm"])

        z, ex, a = {}, {}, {}
        if self.baseline and water and not self.calibrating:
            for u in (SRC, SNK):
                L, B = self.last.get(u), self.baseline["units"].get(u)
                if not L or not B or now - L["t"] > 3:
                    continue
                zm = (L["mic"] - B["mic_mu"]) / max(B["mic_sd"], c["min_sd_db"]) if L["mic"] is not None else 0.0
                zv = (L["vib"] - B["vib_mu"]) / max(B["vib_sd"], c["min_sd_db"]) if L["vib"] is not None else 0.0
                z[u] = {"mic": round(zm, 2), "vib": round(zv, 2)}
                ex[u] = {"mic": max(0.0, (L["mic"] or B["mic_mu"]) - B["mic_mu"]),
                         "vib": max(0.0, (L["vib"] or B["vib_mu"]) - B["vib_mu"])}
                a[u] = clip(max(zm, zv) / (2 * c["z_thr"]))
        s_ac = sum(a.values()) / len(a) if a else 0.0

        if self.calibrating:
            score = 0.0
        elif self.baseline:
            score = c["w_flow"] * s_flow + c["w_ac"] * s_ac
        else:
            score = s_flow

        loc, ratio = "n/a", None
        if SRC in ex and SNK in ex:
            es, ek = max(ex[SRC].values()), max(ex[SNK].values())
            if max(es, ek) < c["loc_min_excess_db"]:
                loc = "unclear (no acoustic excess)"
            else:
                As, Ak = 10 ** (es / 20), 10 ** (ek / 20)
                ratio = As / (As + Ak)
                if ratio > 0.5 + c["loc_band"]:
                    loc = "closer to unit_source"
                elif ratio < 0.5 - c["loc_band"]:
                    loc = "closer to unit_sink"
                else:
                    loc = "between units (central)"

        prev = self.state
        new = None if self.calibrating else self._step(now, score)
        st = {
            "state": self.state, "score": round(score, 3), "s_flow": round(s_flow, 3), "s_ac": round(s_ac, 3),
            "q_src": None if qs is None else round(qs, 3), "q_snk": None if qk is None else round(qk, 3),
            "mismatch": None if mismatch is None else round(mismatch, 3),
            "thr": None if thr is None else round(thr, 3), "delay_s": round(delay, 2),
            "flow_rule_on": flow_on, "water": water, "z": z, "location": loc,
            "loc_ratio": None if ratio is None else round(ratio, 2), "baseline_ok": self.baseline is not None,
        }
        if new:
            st["event"] = {"state": new, "prev": prev, "score": score, "s_flow": s_flow, "s_ac": s_ac,
                           "mismatch": mismatch, "location": loc, "detail": str(z)}
        return st

    def _step(self, now: float, score: float):
        c, new = self.cfg, None
        if self.state == "NORMAL":
            if score >= c["suspect_on"]:
                self.t_up = self.t_up or now
                if now - self.t_up >= c["suspect_hold_s"]:
                    new = "SUSPECTED"
            else:
                self.t_up = None
        elif self.state == "SUSPECTED":
            if score >= c["leak_on"]:
                self.t_up = self.t_up or now
                self.t_down = None
                if now - self.t_up >= c["leak_hold_s"]:
                    new = "LEAK"
            else:
                self.t_up = None
                if score < c["suspect_off"]:
                    self.t_down = self.t_down or now
                    if now - self.t_down >= c["clear_s"]:
                        new = "NORMAL"
                else:
                    self.t_down = None
        else:  # LEAK
            if score < c["leak_off"]:
                self.t_down = self.t_down or now
                if now - self.t_down >= c["clear_s"]:
                    new = "SUSPECTED"
            else:
                self.t_down = None
        if new:
            self.state, self.t_up, self.t_down = new, None, None
        return new

