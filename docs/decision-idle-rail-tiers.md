# DECISION: idle rail-up tiering and forced-full-on-wake

## 1. Tiering: N = 60 s, bound to existing kIdleEnterMs
- **N = 60 s (60000 ms)**. The code already drops the rail at this threshold via `kIdleEnterMs` in `main.cpp:78`. No new constant, no second timer.
- A wake within 60 s gets an instant partial refresh (≈589 ms driving, no cold-panel cost). A wake after 60 s pays one full refresh (~1.5–3 s).
- **Rationale**:
  - Rail-up cost = 7 mA × 60 s = 0.1167 mAh.
  - Full-refresh energy ≈ 0.1 mAh (to be confirmed by open decision #3 meter reading).
  - Break-even = 0.1 mAh / 7 mA ≈ 51 s. 60 s is a round number slightly above break-even with a 1.2× margin.
  - A 2 min window would cost 0.23 mAh — 2.3× the full refresh it tries to avoid. 30 s costs 0.058 mAh but leaves the common short-return case uncovered.
  - This satisfies requirement 5 ("responds quickly"): the frequent case (<60 s) is a fast partial wake.
- **Implementation**: Do not change `kIdleEnterMs`. Ensure `NotesUi::idle` is the only rail-drop path and `poll_keys`/`g_key_while_idle` already wake correctly.

## 2. Forced full on wake: acceptable as-is; do not add windowed-full
- The SSD1681 loses previous-frame RAM when the rail drops; a full waveform is the only way to guarantee a clean image. The current whole-panel full path (`panel_init` + full LUT) is proven and tested.
- **Do not add windowed-full machinery** (writing full LUT over the diff band only). The 2026-11-23 incident was caused by a windowed-addressing bug in the partial path; introducing windowing for the full path buys a few mAh at real regression risk. Open decision #3 (meter measurement of C_full and C_partial) must come first.
- If the retained frame equals the target (no pixel change), skip the refresh entirely — the panel is bistable. The driver already early-returns on `band0 < 0` in `update_once`; ensure `force_full_` is cleared before the check.

## 3. Conditions for revisiting N
- **Metered energy**: When the meter reports C_full and C_partial (open decision #3), recompute N* = C_full / 0.007 A. If C_full ≥ 0.2 mAh, N → 120 s; if C_full ≤ 0.05 mAh, N → 30 s.
- **Ghosting observations**: If any windowed-full attempt leaves residue at band edges, revert to whole-panel full.
- **Idle-current measurement**: If rail-up idle current > 12 mA, cut N to 30 s; if < 5 mA, extend to 120 s.
- **Telemetry**: If >95% of wakes occur >5 min after last input, the hot window is not earning its keep → revisit to 30 s.
- **Battery target**: If daily rail-up cost >5% of battery capacity, cut N to 30 s.

**Bottom line**: N = 60 s, single constant `kIdleEnterMs`, forced full is correct and stays whole-panel. Measure before adding complexity.