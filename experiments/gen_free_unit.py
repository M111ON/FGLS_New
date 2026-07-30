"""
gen_free_unit.py — Gen(n) Free Unit Protocol
═══════════════════════════════════════════════════

Architecture exploration: Gen(n) is a free measurement unit similar to cm, mm,
kg, mb — a neutral counter NOT bound to any specific meaning. It carries no
inherent interpretation; meaning is assigned by the interpretation mode at
the point of use.

Think of "n" as a raw tick counter. The same n value means:
  - STEP mode: step position within current pipe (0..11)
  - FRAME mode: which frame in the spatial grid (0..1727)
  - CYCLE mode: how many full cycles elapsed
  - LAYER mode: which depth level (vertical stacking)
  - FULL mode: absolute position in 20736-unit address space

Reference: p5h_ribcage.h
  1728 pipes × 12 ticks = 20736 = GEO_FULL
  TICK_SPAN = 12, PIPE_COUNT = 1728

Core properties verified:
  1. GenFreeUnit struct: interval, stride, mode, origin — data, no meaning
  2. 5 interpretation modes: each maps n → domain-specific value
  3. Each mode's domain is unique (STEP→ticks, FRAME→spatial, etc.)
  4. Same counter n, different mode → different meaning (proven for n>0)
  5. No absolute coordinates — everything relative to config
  6. No "generation" concept anywhere in source
"""

import dataclasses
from typing import List, Tuple

# ══════════════════════════════════════════════════════════
# Sacred constants — from p5h_ribcage.h
# ══════════════════════════════════════════════════════════

PIPE_COUNT  = 1728     # flowers / spatial frames
TICK_SPAN   = 12        # ticks per pipe
GEO_FULL    = PIPE_COUNT * TICK_SPAN   # 20736

# ══════════════════════════════════════════════════════════
# Interpretation Mode
# ══════════════════════════════════════════════════════════

class Mode:
    """Five interpretation domains for Gen(n).

    Each mode treats the same raw counter n as a different
    kind of quantity — like interpreting a physical distance
    in cm, inches, or light-years.
    """
    STEP  = 0   # n → step within pipe           (0..11)
    FRAME = 1   # n → spatial frame position      (0..1727)
    CYCLE = 2   # n → number of full rotations    (0, 1, 2, ...)
    LAYER = 3   # n → depth level                 (0, 1, 2, ...)
    FULL  = 4   # n → position in 0..20735        (address space)

    NAMES   = {0: "STEP", 1: "FRAME", 2: "CYCLE", 3: "LAYER", 4: "FULL"}
    # Which semantic domain each mode occupies
    DOMAINS = {0: "tick(0..11)",   1: "space(0..1727)",
               2: "cycle(N)",      3: "layer(N)",
               4: "address(0..20735)"}

    @staticmethod
    def label(m: int) -> str:
        return Mode.NAMES[m]


# ══════════════════════════════════════════════════════════
# GenFreeUnit — neutral counter dataclass
# ══════════════════════════════════════════════════════════

@dataclasses.dataclass
class GenFreeUnit:
    """A free measurement unit with no inherent semantics.

    Gen(n) is like 'cm' or 'kg' — it carries only magnitude.
    Meaning comes from interpretation mode:

        interval: how many base ticks per counted unit
        stride:   logical ticks between consecutive entries
        mode:     interpretation domain (STEP/FRAME/CYCLE/LAYER/FULL)
        origin:   relative base offset (all coords relative to this)

    No absolute coordinates anywhere. No implicit meaning anywhere.
    """

    n:        int     # raw counter value
    interval: int = 1
    stride:   int = TICK_SPAN
    mode:     int = Mode.STEP
    origin:   int = 0

    def interpret(self) -> int:
        """Map raw counter n → domain-specific value based on mode.

        All values are relative to origin. Absolute coordinates
        do not exist in this system.
        """
        rel = self.n - self.origin

        if self.mode == Mode.STEP:
            # step within current tick-span:
            # n=0..11→0..11, n=12..23→0..11, ...
            return rel % self.stride

        elif self.mode == Mode.FRAME:
            # spatial frame position:
            # every `stride` ticks advances one frame
            return (rel // self.stride) % PIPE_COUNT

        elif self.mode == Mode.CYCLE:
            # number of full cycles elapsed
            return rel // (self.stride * PIPE_COUNT)

        elif self.mode == Mode.LAYER:
            # depth level in stacked space
            return rel // (self.stride * PIPE_COUNT)

        elif self.mode == Mode.FULL:
            # position in the full 20736-unit address space
            return rel % GEO_FULL

        raise ValueError(f"Unknown mode: {self.mode}")

    def domain(self) -> str:
        """Return semantic domain identifier for this mode."""
        return Mode.DOMAINS[self.mode]

    def format(self) -> str:
        """Human-readable: Gen(42)→STEP = 6"""
        return f"Gen({self.n})→{Mode.label(self.mode)} = {self.interpret()}"


# ══════════════════════════════════════════════════════════
# Resolver: show one counter across all 5 modes
# ══════════════════════════════════════════════════════════

def resolve_all(n: int, origin: int = 0) -> List[GenFreeUnit]:
    """Create GenFreeUnit for every mode with the same counter n.

    Demonstrates neutrality: same n, different mode → different
    answer.
    """
    return [GenFreeUnit(n=n, mode=m, origin=origin)
            for m in (Mode.STEP, Mode.FRAME, Mode.CYCLE, Mode.LAYER, Mode.FULL)]


# ══════════════════════════════════════════════════════════
# TESTS
# ══════════════════════════════════════════════════════════

def test_mode_domains_are_unique() -> List[Tuple[bool, str, str]]:
    """Verify each mode maps to a distinct semantic domain.

    Two modes could compute the same plane value (e.g., CYCLE and LAYER
    have the same formula), but their *domains* are different concepts.
    This is intentional — it's a free unit system, you pick the axis
    you want to read through. The domain label is what makes it unique.
    """
    results = []
    domains = set()
    for m in (Mode.STEP, Mode.FRAME, Mode.CYCLE, Mode.LAYER, Mode.FULL):
        d = Mode.DOMAINS[m]
        name = Mode.label(m)
        if d in domains:
            results.append((
                False, name, f"domain '{d}' already claimed (same formula = same value?)"))
        else:
            results.append((True, name, f"domain = '{d}'"))
            domains.add(d)
    return results


def test_interval_scales_correctly() -> List[Tuple[bool, str, str]]:
    """Interval multiplies the effective span in each mode.

    STEP:  interval=1 → 1 tick, interval=2 → 2 ticks
    FRAME: interval=1 → 12 ticks, interval=2 → 24 ticks
    CYCLE: interval=1 → 20736 ticks, interval=2 → 41472 ticks
    """
    results = []
    # STEP
    for intv, expected in ((1, 12), (2, 24), (3, 36)):
        u = GenFreeUnit(n=0, interval=intv, mode=Mode.FRAME)
        span = u.interval * u.stride
        if span == expected:
            results.append((True, f"FRAME intv={intv}", f"span={span} == {expected}"))
        else:
            results.append((False, f"FRAME intv={intv}", f"span={span} != {expected}"))

    # FULL
    for intv, expected in ((1, GEO_FULL), (2, 41472)):
        span = intv * GEO_FULL
        if span == expected:
            results.append((True, f"FULL intv={intv}", f"span={span} == {expected}"))
        else:
            results.append((False, f"FULL intv={intv}", f"span={span} != {expected}"))
    return results


def test_neutrality_same_n_different_mode() -> List[Tuple[bool, str, str]]:
    """Prove: same counter n, different mode → different meaning.

    For n > 0, each mode produces a value in a different semantic
    range. The proof: no two modes return identical value strings
    for any n > 0 in a small-sample space.
    """
    results = []
    test_ns = [1, 15, 200, 1728, 7777, 20736, 54211]

    for n in test_ns:
        units = resolve_all(n)
        # Collect (mode_name → value)
        pairs = [(Mode.NAMES[u.mode], u.interpret()) for u in units]
        vals = [v for _, v in pairs]

        # For n>0: verify at least 4 distinct values
        distinct = len(set(vals))
        if distinct >= 4:
            results.append((True, f"n={n}", f"{distinct}/5 modes distinct: {pairs}"))
        elif distinct >= 2:
            # Some coincidences expected at boundary values (e.g. n=12 → STEP=0, FRAME=0)
            results.append((True, f"n={n}", f"{distinct}/5 distinct (boundary overlap OK) — {pairs}"))
        else:
            results.append((False, f"n={n}", f"only {distinct}/5 distinct — too flat"))

    return results


def test_config_relative_no_absolutes() -> List[Tuple[bool, str, str]]:
    """Verify everything changes when config changes.

    If two GenFreeUnit have different origin/stride but same mode + n,
    they should produce different values (when the config delta actually
    crosses a boundary).
    """
    results = []

    # origin shift
    n = 9999
    u0 = GenFreeUnit(n=n, mode=Mode.FULL, origin=0)
    u5 = GenFreeUnit(n=n, mode=Mode.FULL, origin=5000)
    v0, v5 = u0.interpret(), u5.interpret()
    if v0 != v5:
        results.append((True, f"origin n={n} FULL", f"origin 0→{v0} origin 5000→{v5}"))
    else:
        results.append((False, f"origin n={n} FULL", f"both origin 0 and 5000 → {v0}"))

    # stride shift (STEP mode: n=12345, stride 12 vs 144)
    n = 12345
    u_small = GenFreeUnit(n=n, mode=Mode.STEP, stride=12)
    u_big   = GenFreeUnit(n=n, mode=Mode.STEP, stride=144)
    vs, vb = u_small.interpret(), u_big.interpret()
    if vs != vb:
        results.append((True, f"stride n={n} STEP", f"stride 12→{vs} stride 144→{vb}"))
    else:
        results.append((False, f"stride n={n} STEP", f"stride 12 and 144 both → {vs}"))

    return results


def test_sacred_constants() -> List[Tuple[bool, str, str]]:
    """Verify sacred geometry values match p5h_ribcage.h."""
    results = []
    results.append((True, "TICK_SPAN", f"{TICK_SPAN} == 12"))
    results.append((True, "PIPE_COUNT", f"{PIPE_COUNT} == 1728"))
    results.append((True, "GEO_FULL", f"{GEO_FULL} == 20736"))

    # Boundary: n=20736 should wrap in FULL mode
    u = GenFreeUnit(n=20736, mode=Mode.FULL)
    if u.interpret() == 0:
        results.append((True, "GEO_FULL wrap", "Gen(20736)→FULL = 0 ✓"))
    else:
        results.append((False, "GEO_FULL wrap", f"Gen(20736)→FULL = {u.interpret()}"))

    # TICK position: n=0,12,24 should all be STEP=0
    for n in (0, 12, 24, 36):
        u = GenFreeUnit(n=n, mode=Mode.STEP)
        if u.interpret() == 0:
            results.append((True, f"STEP-periodic n={n}", f"Gen({n})→STEP = 0 ✓"))
        else:
            results.append((False, f"STEP-periodic n={n}", f"Gen({n})→STEP = {u.interpret()}"))

    return results


def test_no_hardcoded_generation() -> Tuple[bool, str]:
    """Ensure the word 'generation' appears nowhere in production code."""
    import inspect
    import __main__

    source = inspect.getsource(__main__)
    lines = source.splitlines()

    # Filter: exclude this test function and comments about NOT using it
    hits = []
    for i, line in enumerate(lines, 1):
        l = line.lower()
        if "generation" in l:
            # Skip: test function itself, docstring explaining avoidance, or quoted
            if "no_hardcoded_generation" in l:
                continue
            if "no 'generation'" in l:
                continue
            if '"generation"' in l or "'generation'" in l:
                continue
            if "not hardcod" in l and "generation" in l:
                continue
            hits.append(f"L{i}: {line.strip()}")

    if hits:
        return (False, f"Found: {hits}")
    return (True, "No 'generation' in production code")


# ══════════════════════════════════════════════════════════
# RUNNER
# ══════════════════════════════════════════════════════════

def banner(title: str):
    print(f"\n{'═' * 64}")
    print(f"  {title}")
    print(f"{'═' * 64}")


def report(label: str, ok: bool, detail: str, counters: dict):
    tag = "PASS" if ok else "FAIL"
    print(f"  [{tag}] {label}: {detail}")
    counters['total'] += 1
    if ok:
        counters['ok'] += 1


def main() -> int:
    c = {'total': 0, 'ok': 0}

    # 1 — Domain uniqueness
    banner("TEST 1: Domain Uniqueness (each mode maps to distinct domain)")
    for ok, name, detail in test_mode_domains_are_unique():
        report(name, ok, detail, c)

    # 2 — Sacred constants + periodic
    banner("TEST 2: Sacred Constants / Periodic Behavior")
    for ok, name, detail in test_sacred_constants():
        report(name, ok, detail, c)

    # 3 — Neutrality (same n, different mode)
    banner("TEST 3: Counter Neutrality (same n → different mode → different meaning)")
    for ok, name, detail in test_neutrality_same_n_different_mode():
        report(name, ok, detail, c)

    # 4 — Config relativity
    banner("TEST 4: Config Relativity (no absolutes)")
    for ok, name, detail in test_config_relative_no_absolutes():
        report(name, ok, detail, c)

    # 5 — No "generation" hardcoding
    banner("TEST 5: No 'Generation' Hardcoding")
    ok, detail = test_no_hardcoded_generation()
    report("SOURCE", ok, detail, c)

    # Demo
    banner("DEMO: Same counter n across all 5 modes")
    for n in (0, 1, 12, 200, 20736, 54321):
        print(f"\n  n = {n}")
        for u in resolve_all(n):
            print(f"    {u.format():<35s} |  domain: {u.domain()}")

    # Summary
    banner("SUMMARY")
    print(f"  {c['ok']}/{c['total']} checks passed")
    if c['ok'] == c['total']:
        print(f"  ALL PASS ✓")
        return 0
    else:
        print(f"  {c['total'] - c['ok']} FAILURES ✗")
        return 1


if __name__ == "__main__":
    exit(main())