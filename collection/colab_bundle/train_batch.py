"""
train_batch.py — PPO on POGLS batch env
Colab setup:
  !gcc -O2 -shared -fPIC -I. pogls_env.c -o pogls_env.so
  !python train_batch.py

Speed: ~6-7M env steps/s (x64 batch)
"""

import ctypes, numpy as np, time, os
import torch, torch.nn as nn, torch.optim as optim

# ── Load POGLS shared lib ────────────────────────────────
_SO = os.environ.get("POGLS_SO", "./pogls_env.so")
lib = ctypes.CDLL(_SO)

lib.pogls_env_verify.restype        = ctypes.c_int
lib.pogls_env_create.restype        = ctypes.c_void_p
lib.pogls_env_create.argtypes       = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
lib.pogls_env_create_batch.argtypes = [ctypes.POINTER(ctypes.c_void_p),
    ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
lib.pogls_env_step_batch.argtypes   = [ctypes.POINTER(ctypes.c_void_p),
    ctypes.POINTER(ctypes.c_int), ctypes.c_int, ctypes.POINTER(ctypes.c_float)]
lib.pogls_env_reset_batch.argtypes  = [ctypes.POINTER(ctypes.c_void_p),
    ctypes.c_int, ctypes.POINTER(ctypes.c_float)]
lib.pogls_env_free_batch.argtypes   = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]

assert lib.pogls_env_verify() == 0, "POGLS verify failed"

# ── Config ───────────────────────────────────────────────
CFG = dict(
    n_envs        = 64,
    rollout_steps = 512,      # steps per env per rollout
    total_steps   = 5_000_000,
    n_epochs      = 4,
    batch_size    = 512,
    lr            = 3e-4,
    gamma         = 0.99,
    gae_lambda    = 0.95,
    clip_eps      = 0.2,
    vf_coef       = 0.5,
    ent_coef      = 0.01,
    max_ep_steps  = 512,
    log_every     = 10,
    data_sz       = 20736 * 64,
)

OBS_DIM   = 8
N_ACTIONS = 4

# ── Batch env wrapper ────────────────────────────────────
class PoglsBatchEnv:
    def __init__(self, n_envs, data_sz, max_steps):
        self.n  = n_envs
        self.data = np.random.randint(0, 256, data_sz, dtype=np.uint8)
        self.envs = (ctypes.c_void_p * n_envs)()
        lib.pogls_env_create_batch(
            self.envs, n_envs,
            self.data.ctypes.data_as(ctypes.c_char_p),
            data_sz, max_steps
        )
        self._act = (ctypes.c_int   * n_envs)()
        self._out = (ctypes.c_float * (n_envs * 11))()
        self._obs = (ctypes.c_float * (n_envs * 8))()

    def reset(self):
        lib.pogls_env_reset_batch(self.envs, self.n, self._obs)
        return np.frombuffer(self._obs, dtype=np.float32).reshape(self.n, OBS_DIM).copy()

    def step(self, actions: np.ndarray):
        for i in range(self.n): self._act[i] = int(actions[i])
        lib.pogls_env_step_batch(self.envs, self._act, self.n, self._out)
        arr   = np.frombuffer(self._out, dtype=np.float32).reshape(self.n, 11)
        obs   = arr[:, :8].copy()
        rews  = arr[:, 8].copy()
        dones = arr[:, 9].copy().astype(bool)
        return obs, rews, dones

    def close(self):
        lib.pogls_env_free_batch(self.envs, self.n)

# ── Policy ───────────────────────────────────────────────
class Policy(nn.Module):
    def __init__(self):
        super().__init__()
        self.shared = nn.Sequential(
            nn.Linear(OBS_DIM, 128), nn.Tanh(),
            nn.Linear(128, 64),      nn.Tanh(),
        )
        self.actor  = nn.Linear(64, N_ACTIONS)
        self.critic = nn.Linear(64, 1)

    def forward(self, x):
        h = self.shared(x)
        return self.actor(h), self.critic(h).squeeze(-1)

    def get_action(self, x):
        logits, value = self(x)
        dist   = torch.distributions.Categorical(logits=logits)
        action = dist.sample()
        return action, dist.log_prob(action), dist.entropy(), value

    def evaluate(self, x, actions):
        logits, value = self(x)
        dist = torch.distributions.Categorical(logits=logits)
        return dist.log_prob(actions), dist.entropy(), value

# ── GAE ──────────────────────────────────────────────────
def gae(rews, vals, dones, last_vals, gamma, lam):
    # rews,vals,dones: [T, N]  last_vals: [N]
    T, N = rews.shape
    adv = np.zeros_like(rews)
    g   = np.zeros(N, dtype=np.float32)
    for t in reversed(range(T)):
        nxt  = last_vals if t == T-1 else vals[t+1]
        mask = 1.0 - dones[t]
        delta = rews[t] + gamma * nxt * mask - vals[t]
        g     = delta + gamma * lam * mask * g
        adv[t] = g
    return adv, adv + vals

# ── PPO update ───────────────────────────────────────────
def ppo_update(policy, optimizer, obs, acts, logps, rets, advs, cfg, device):
    advs = (advs - advs.mean()) / (advs.std() + 1e-8)
    T, N = obs.shape[:2]
    obs_f  = torch.tensor(obs.reshape(-1, OBS_DIM), dtype=torch.float32, device=device)
    act_f  = torch.tensor(acts.reshape(-1),          dtype=torch.long,    device=device)
    logp_f = torch.tensor(logps.reshape(-1),         dtype=torch.float32, device=device)
    ret_f  = torch.tensor(rets.reshape(-1),          dtype=torch.float32, device=device)
    adv_f  = torch.tensor(advs.reshape(-1),          dtype=torch.float32, device=device)

    n_total = T * N
    losses  = []
    for _ in range(cfg["n_epochs"]):
        idx = np.random.permutation(n_total)
        for s in range(0, n_total, cfg["batch_size"]):
            mb = idx[s:s+cfg["batch_size"]]
            lp, ent, val = policy.evaluate(obs_f[mb], act_f[mb])
            ratio    = (lp - logp_f[mb]).exp()
            clipped  = ratio.clamp(1-cfg["clip_eps"], 1+cfg["clip_eps"])
            pg_loss  = -torch.min(ratio*adv_f[mb], clipped*adv_f[mb]).mean()
            vf_loss  = (val - ret_f[mb]).pow(2).mean()
            loss     = pg_loss + cfg["vf_coef"]*vf_loss - cfg["ent_coef"]*ent.mean()
            optimizer.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(policy.parameters(), 0.5)
            optimizer.step()
            losses.append(loss.item())
    return np.mean(losses)

# ── Main ─────────────────────────────────────────────────
def train():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    n      = CFG["n_envs"]
    T      = CFG["rollout_steps"]
    print(f"device={device}  n_envs={n}  rollout={T}  "
          f"batch={n*T} samples/rollout")

    env    = PoglsBatchEnv(n, CFG["data_sz"], CFG["max_ep_steps"])
    policy = Policy().to(device)
    optim_ = optim.Adam(policy.parameters(), lr=CFG["lr"])

    # rollout buffers [T, N]
    obs_buf  = np.zeros((T, n, OBS_DIM), dtype=np.float32)
    act_buf  = np.zeros((T, n),          dtype=np.int64)
    rew_buf  = np.zeros((T, n),          dtype=np.float32)
    done_buf = np.zeros((T, n),          dtype=np.float32)
    val_buf  = np.zeros((T, n),          dtype=np.float32)
    logp_buf = np.zeros((T, n),          dtype=np.float32)

    obs       = env.reset()
    total     = 0
    rollout_n = 0
    ep_rews   = []
    ep_buf    = np.zeros(n, dtype=np.float32)
    t_start   = time.perf_counter()
    env_steps = 0

    while total < CFG["total_steps"]:
        # ── collect rollout ──────────────────────────────
        policy.eval()
        t_env = 0.0
        with torch.no_grad():
            for t in range(T):
                obs_t  = torch.tensor(obs, dtype=torch.float32, device=device)
                acts, logps, _, vals = policy.get_action(obs_t)
                acts_np = acts.cpu().numpy()

                te0 = time.perf_counter()
                next_obs, rews, dones = env.step(acts_np)
                t_env += time.perf_counter() - te0

                obs_buf[t]  = obs
                act_buf[t]  = acts_np
                rew_buf[t]  = rews
                done_buf[t] = dones.astype(np.float32)
                val_buf[t]  = vals.cpu().numpy()
                logp_buf[t] = logps.cpu().numpy()

                ep_buf += rews
                for i in np.where(dones)[0]:
                    ep_rews.append(float(ep_buf[i]))
                    ep_buf[i] = 0.0

                obs    = next_obs
                total += n
                env_steps += n

            # last value
            obs_t    = torch.tensor(obs, dtype=torch.float32, device=device)
            _, last_v = policy(obs_t)
            last_vals = last_v.cpu().numpy()

        # ── GAE + PPO update ─────────────────────────────
        adv, ret = gae(rew_buf, val_buf, done_buf, last_vals,
                       CFG["gamma"], CFG["gae_lambda"])
        policy.train()
        loss = ppo_update(policy, optim_,
                          obs_buf, act_buf, logp_buf, ret, adv, CFG, device)

        rollout_n += 1
        if rollout_n % CFG["log_every"] == 0:
            elapsed  = time.perf_counter() - t_start
            sps      = env_steps / elapsed
            env_pct  = 100.0 * t_env / (elapsed / rollout_n * CFG["log_every"])
            mean_rew = np.mean(ep_rews[-50:]) if ep_rews else 0.0
            print(f"step={total:>9,}  sps={sps:>8,.0f}  "
                  f"rew={mean_rew:>6.2f}  loss={loss:.4f}  "
                  f"env_time={env_pct:.1f}%  eps={len(ep_rews)}")

    elapsed = time.perf_counter() - t_start
    print(f"\ndone: {total:,} steps in {elapsed:.1f}s = {total/elapsed:,.0f} steps/s")
    env.close()
    return policy

if __name__ == "__main__":
    policy = train()
