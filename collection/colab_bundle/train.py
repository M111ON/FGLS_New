"""
train.py — Minimal PPO on POGLS env (Colab-ready)

Install: pip install torch numpy
Run:     python train.py
"""

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
import time
from pogls_env import PoglsEnv, OBS_DIM, N_ACTIONS

# ── Config ───────────────────────────────────────────────
CFG = {
    "total_steps":   1_000_000,
    "rollout_steps": 2048,
    "n_epochs":      4,
    "batch_size":    256,
    "lr":            3e-4,
    "gamma":         0.99,
    "gae_lambda":    0.95,
    "clip_eps":      0.2,
    "vf_coef":       0.5,
    "ent_coef":      0.01,
    "max_steps":     512,
    "log_every":     10,   # log every N rollouts
}

# ── Policy network ───────────────────────────────────────
class Policy(nn.Module):
    def __init__(self, obs_dim=OBS_DIM, n_actions=N_ACTIONS):
        super().__init__()
        # small net — env is the bottleneck not the network
        self.shared = nn.Sequential(
            nn.Linear(obs_dim, 64), nn.Tanh(),
            nn.Linear(64, 64),      nn.Tanh(),
        )
        self.actor  = nn.Linear(64, n_actions)
        self.critic = nn.Linear(64, 1)

    def forward(self, x):
        h = self.shared(x)
        return self.actor(h), self.critic(h)

    def get_action(self, obs):
        logits, value = self(obs)
        dist   = torch.distributions.Categorical(logits=logits)
        action = dist.sample()
        return action, dist.log_prob(action), dist.entropy(), value.squeeze(-1)

    def evaluate(self, obs, actions):
        logits, value = self(obs)
        dist    = torch.distributions.Categorical(logits=logits)
        log_probs = dist.log_prob(actions)
        entropy   = dist.entropy()
        return log_probs, entropy, value.squeeze(-1)


# ── GAE ──────────────────────────────────────────────────
def compute_gae(rewards, values, dones, last_value, gamma, lam):
    n = len(rewards)
    advantages = np.zeros(n, dtype=np.float32)
    gae = 0.0
    for t in reversed(range(n)):
        nxt = last_value if t == n-1 else values[t+1]
        delta = rewards[t] + gamma * nxt * (1 - dones[t]) - values[t]
        gae   = delta + gamma * lam * (1 - dones[t]) * gae
        advantages[t] = gae
    returns = advantages + values
    return advantages, returns


# ── PPO update ───────────────────────────────────────────
def ppo_update(policy, optimizer, obs_b, act_b, logp_b, ret_b, adv_b, cfg):
    adv_b = (adv_b - adv_b.mean()) / (adv_b.std() + 1e-8)
    n = len(obs_b)
    losses = []
    for _ in range(cfg["n_epochs"]):
        idx = np.random.permutation(n)
        for start in range(0, n, cfg["batch_size"]):
            mb = idx[start:start+cfg["batch_size"]]
            obs_m  = obs_b[mb]
            act_m  = act_b[mb]
            logp_m = logp_b[mb]
            ret_m  = ret_b[mb]
            adv_m  = adv_b[mb]

            new_logp, entropy, value = policy.evaluate(obs_m, act_m)
            ratio  = (new_logp - logp_m).exp()
            clip   = ratio.clamp(1-cfg["clip_eps"], 1+cfg["clip_eps"])
            pg_loss  = -torch.min(ratio*adv_m, clip*adv_m).mean()
            vf_loss  = (value - ret_m).pow(2).mean()
            ent_loss = -entropy.mean()
            loss = pg_loss + cfg["vf_coef"]*vf_loss + cfg["ent_coef"]*ent_loss

            optimizer.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(policy.parameters(), 0.5)
            optimizer.step()
            losses.append(loss.item())
    return np.mean(losses)


# ── Main training loop ───────────────────────────────────
def train():
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"device: {device}")

    env    = PoglsEnv(max_steps=CFG["max_steps"])
    policy = Policy().to(device)
    optim_ = optim.Adam(policy.parameters(), lr=CFG["lr"])

    obs, _ = env.reset()
    total  = 0
    ep_rew = 0.0
    ep_n   = 0
    rollout_n = 0
    t_start = time.perf_counter()

    # rollout buffers
    R = CFG["rollout_steps"]
    obs_buf  = np.zeros((R, OBS_DIM), dtype=np.float32)
    act_buf  = np.zeros(R, dtype=np.int64)
    rew_buf  = np.zeros(R, dtype=np.float32)
    done_buf = np.zeros(R, dtype=np.float32)
    val_buf  = np.zeros(R, dtype=np.float32)
    logp_buf = np.zeros(R, dtype=np.float32)
    ep_rews  = []

    while total < CFG["total_steps"]:
        # ── collect rollout ──
        policy.eval()
        with torch.no_grad():
            for t in range(R):
                obs_t  = torch.tensor(obs, dtype=torch.float32, device=device).unsqueeze(0)
                action, logp, _, value = policy.get_action(obs_t)
                a = int(action.item())

                next_obs, reward, done, _, _ = env.step(a)
                ep_rew += reward

                obs_buf[t]  = obs
                act_buf[t]  = a
                rew_buf[t]  = reward
                done_buf[t] = float(done)
                val_buf[t]  = float(value.item())
                logp_buf[t] = float(logp.item())

                obs = next_obs
                total += 1
                if done:
                    ep_rews.append(ep_rew)
                    ep_rew = 0.0
                    ep_n  += 1
                    obs, _ = env.reset()

            # last value for GAE
            obs_t = torch.tensor(obs, dtype=torch.float32, device=device).unsqueeze(0)
            _, last_val = policy(obs_t)
            last_val = float(last_val.item())

        # ── compute GAE ──
        adv, ret = compute_gae(rew_buf, val_buf, done_buf, last_val,
                               CFG["gamma"], CFG["gae_lambda"])

        # ── PPO update ──
        policy.train()
        obs_t  = torch.tensor(obs_buf,  dtype=torch.float32, device=device)
        act_t  = torch.tensor(act_buf,  dtype=torch.long,    device=device)
        logp_t = torch.tensor(logp_buf, dtype=torch.float32, device=device)
        ret_t  = torch.tensor(ret,      dtype=torch.float32, device=device)
        adv_t  = torch.tensor(adv,      dtype=torch.float32, device=device)

        loss = ppo_update(policy, optim_, obs_t, act_t, logp_t, ret_t, adv_t, CFG)

        rollout_n += 1
        if rollout_n % CFG["log_every"] == 0:
            elapsed = time.perf_counter() - t_start
            sps = total / elapsed
            mean_rew = np.mean(ep_rews[-20:]) if ep_rews else 0.0
            stats = env.stats()
            print(f"step={total:>8,}  sps={sps:>8,.0f}  "
                  f"rew={mean_rew:>6.2f}  loss={loss:.4f}  "
                  f"eps={ep_n}  zones={stats['zone_resets']}")

    elapsed = time.perf_counter() - t_start
    print(f"\ndone: {total:,} steps in {elapsed:.1f}s = {total/elapsed:,.0f} steps/s")
    env.close()
    return policy


if __name__ == "__main__":
    policy = train()
