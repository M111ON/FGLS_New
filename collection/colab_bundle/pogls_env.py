"""
pogls_env.py — POGLS Gym-compatible Environment (ctypes wrapper)

Usage in Colab:
    !gcc -O2 -shared -fPIC -I. pogls_env.c -o pogls_env.so
    from pogls_env import PoglsEnv
    env = PoglsEnv()
    obs, _ = env.reset()
    obs, reward, done, trunc, info = env.step(0)
"""

import ctypes, os, numpy as np

# ── load shared lib ──────────────────────────────────────
_LIB_PATH = os.environ.get("POGLS_SO", os.path.join(os.path.dirname(os.path.abspath(__file__)) if "__file__" in dir() else ".", "pogls_env.so"))
_lib = ctypes.CDLL(_LIB_PATH)

# ── C function signatures ────────────────────────────────
_lib.pogls_env_create.restype  = ctypes.c_void_p
_lib.pogls_env_create.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
_lib.pogls_env_free.argtypes   = [ctypes.c_void_p]
_lib.pogls_env_reset.restype   = ctypes.c_uint64
_lib.pogls_env_reset.argtypes  = [ctypes.c_void_p]
_lib.pogls_env_step.argtypes   = [ctypes.c_void_p, ctypes.c_int,
                                   ctypes.POINTER(ctypes.c_uint32)]
_lib.pogls_env_obs.argtypes    = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_float)]
_lib.pogls_env_stats.argtypes  = [ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_uint32)]
_lib.pogls_env_verify.restype  = ctypes.c_int

# ── Constants ────────────────────────────────────────────
ENV_CHUNK_SZ = 64
ENV_DATA_SZ  = 20736 * ENV_CHUNK_SZ   # ~1.3MB
OBS_DIM      = 8                       # float32 observation
N_ACTIONS    = 4                       # ORBITAL/CHIRAL/CROSS/HUB
ACTION_NAMES = ["ORBITAL", "CHIRAL", "CROSS", "HUB"]


class PoglsEnv:
    """
    Gym-compatible POGLS environment.

    Observation: float32[8]
      [zone/12, pair/6, pole, isect/64, rubik_frame/32,
       icosa_cell/10, enc/720, steps/max_steps]

    Actions: 0=ORBITAL(walk) 1=CHIRAL(jump) 2=CROSS(hop) 3=HUB(free)

    Reward: geometry-driven
      isect==0  → +3.0  (boundary found)
      isect<8   → +2.0  (fibo territory)
      isect<16  → +1.0  (structured)
      isect≥16  → -0.5  (residual zone)
      + position bonuses

    Episode ends: zone boundary (isect==0) OR max_steps
    """

    metadata = {"render_modes": []}
    observation_space_shape = (OBS_DIM,)
    action_space_n = N_ACTIONS

    def __init__(self, data: np.ndarray = None, max_steps: int = 512):
        assert _lib.pogls_env_verify() == 0, "POGLS verify failed"

        # generate or accept data
        if data is None:
            rng = np.random.default_rng(42)
            data = rng.integers(0, 256, ENV_DATA_SZ, dtype=np.uint8)
        self._data = np.ascontiguousarray(data[:ENV_DATA_SZ], dtype=np.uint8)
        self._max_steps = max_steps

        self._env = _lib.pogls_env_create(
            self._data.ctypes.data_as(ctypes.c_char_p),
            ctypes.c_uint32(ENV_DATA_SZ),
            ctypes.c_uint32(max_steps)
        )
        # reusable buffers (avoid alloc per step)
        self._out4  = (ctypes.c_uint32 * 4)()
        self._obs8  = (ctypes.c_float  * OBS_DIM)()
        self._stat4 = (ctypes.c_uint32 * 4)()

    def reset(self, seed=None):
        _lib.pogls_env_reset(self._env)
        obs = self._get_obs()
        return obs, {}

    def step(self, action: int):
        _lib.pogls_env_step(self._env, int(action), self._out4)
        reward = ctypes.cast(
            ctypes.byref(self._out4, 8), ctypes.POINTER(ctypes.c_float))[0]
        done   = bool(self._out4[3])
        obs    = self._get_obs()
        return obs, float(reward), done, False, {}

    def _get_obs(self) -> np.ndarray:
        _lib.pogls_env_obs(self._env, self._obs8)
        return np.array(self._obs8, dtype=np.float32)

    def stats(self) -> dict:
        _lib.pogls_env_stats(self._env, self._stat4)
        return {
            "steps":       int(self._stat4[0]),
            "zone_resets": int(self._stat4[1]),
            "jumps":       int(self._stat4[2]),
            "walks":       int(self._stat4[3]),
        }

    def close(self):
        if self._env:
            _lib.pogls_env_free(self._env)
            self._env = None

    def __del__(self):
        self.close()


# ── quick sanity test ────────────────────────────────────
if __name__ == "__main__":
    import time
    env = PoglsEnv(max_steps=512)
    obs, _ = env.reset()
    print(f"obs shape : {obs.shape}  dtype={obs.dtype}")
    print(f"obs       : {np.round(obs, 3)}")

    # speed test
    N = 100_000
    t0 = time.perf_counter()
    obs, _ = env.reset()
    total_reward = 0.0
    for i in range(N):
        action = i % N_ACTIONS
        obs, r, done, _, _ = env.step(action)
        total_reward += r
        if done:
            obs, _ = env.reset()
    dt = time.perf_counter() - t0

    print(f"\n{N:,} steps in {dt:.3f}s = {N/dt:,.0f} steps/s")
    print(f"total_reward = {total_reward:.1f}")
    print(f"stats: {env.stats()}")
    env.close()
