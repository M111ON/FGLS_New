"""
pogls_vqvae_text.py — VQ-POGLS + Sentence Embeddings
Wire real text → geometry codebook → traverse

Colab setup:
  !pip install torch sentence-transformers
  !python pogls_vqvae_text.py

Or copy pogls_vqvae.py to same folder, then:
  from pogls_vqvae import VqPogls, GEO_TABLE, N_CODES
"""

import torch
import torch.nn.functional as F
import time

# ── Geometry constants (mirror pogls_vqvae.py) ───────────
N_CODES   = 240
OBS_DIM   =   8
N_ZONES   =  12
N_ACTIONS =   4
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

def build_geo_table():
    positions = [(i * 37) % 720 for i in range(N_CODES)]
    table = []
    for enc in positions:
        zone    = enc // 60
        pair    = [0,1,2,3,4,5,0,1,2,3,4,5][zone]
        pole    = [0,0,0,0,0,0,1,1,1,1,1,1][zone]
        partner = [9,10,11,6,7,8,3,4,5,0,1,2][zone]
        frame   = enc % 32
        cell    = enc % 10
        table.append([zone/12., pair/6., float(pole),
                      enc/720., frame/32., cell/10.,
                      partner/12., ((enc*37)%720)/720.])
    return torch.tensor(table, dtype=torch.float32)

GEO_TABLE = build_geo_table().to(DEVICE)

# ── Paste VqPogls classes here OR import ─────────────────
# from pogls_vqvae import VqPogls
# (copy-paste GeoCodebook, Encoder, Decoder, VqPogls from pogls_vqvae.py)

import torch.nn as nn

class GeoCodebook(nn.Module):
    def __init__(self, code_dim, n_codes=N_CODES, decay=0.95, restart_threshold=1.0):
        super().__init__()
        self.n_codes = n_codes; self.code_dim = code_dim
        self.decay = decay; self.threshold = restart_threshold
        with torch.no_grad():
            proj = nn.Linear(OBS_DIM, code_dim, bias=False)
            nn.init.orthogonal_(proj.weight)
            init_codes = proj(GEO_TABLE.cpu())[:n_codes]
        self.register_buffer('codes',     init_codes.clone())
        self.register_buffer("ema_count", torch.full((n_codes,), 2.0))
        self.register_buffer('ema_sum',   init_codes.clone())
        self.register_buffer('geo_idx',   torch.arange(n_codes))

    def forward(self, z):
        B = z.shape[0]
        dist = (z.pow(2).sum(1, keepdim=True)
                - 2 * z @ self.codes.T
                + self.codes.pow(2).sum(1, keepdim=True).T)
        indices = dist.argmin(dim=1)
        z_q     = self.codes[indices]
        loss    = F.mse_loss(z_q.detach(), z)
        z_q_st  = z + (z_q - z).detach()
        if self.training:
            with torch.no_grad():
                one_hot = F.one_hot(indices, self.n_codes).float()
                count   = one_hot.sum(0)
                sum_z   = one_hot.T @ z
                self.ema_count = self.decay * self.ema_count + (1-self.decay) * count
                self.ema_sum   = self.decay * self.ema_sum   + (1-self.decay) * sum_z
                n = self.ema_count.unsqueeze(1).clamp(min=1e-5)
                self.codes = self.ema_sum / n
                dead = (self.ema_count < self.threshold)
                n_dead = dead.sum().item()
                if n_dead > 0:
                    rand_idx = torch.randint(0, B, (int(n_dead),), device=z.device)
                    self.codes[dead] = z[rand_idx].detach()
                    self.ema_sum[dead]   = z[rand_idx].detach()
                    self.ema_count[dead] = 1.0
        usage = (self.ema_count > self.threshold).float().mean().item()
        metrics = {"usage": usage, "n_dead": int((self.ema_count < self.threshold).sum())}
        return z_q_st, indices, loss, metrics

    def traverse(self, indices, mode=0):
        if mode == 0: return (indices + 1)   % self.n_codes
        if mode == 1: return (indices + self.n_codes // 2) % self.n_codes
        if mode == 2:
            encs  = (indices * 37) % 720
            zones = encs // 60
            partner_zones = torch.tensor(
                [9,10,11,6,7,8,3,4,5,0,1,2], device=indices.device)[zones]
            new_encs = partner_zones * 60 + (encs % 60)
            return (new_encs * 37) % self.n_codes
        if mode == 3:
            encs  = (indices * 37) % 720
            zones = encs // 60
            return (zones * 20) % self.n_codes
        return indices

class Encoder(nn.Module):
    def __init__(self, input_dim, code_dim, hidden=256):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,    hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,    code_dim),
        )
    def forward(self, x): return self.net(x)

class Decoder(nn.Module):
    def __init__(self, output_dim, code_dim, hidden=256):
        super().__init__()
        self.from_code = nn.Sequential(
            nn.Linear(code_dim, hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,   hidden), nn.LayerNorm(hidden), nn.GELU(),
            nn.Linear(hidden,   output_dim),
        )
        self.from_geo = nn.Sequential(
            nn.Linear(OBS_DIM, hidden // 2), nn.GELU(),
            nn.Linear(hidden // 2, output_dim),
        )
    def forward(self, z_q, geo_coords):
        return self.from_code(z_q) + 0.1 * self.from_geo(geo_coords)

class VqPogls(nn.Module):
    def __init__(self, dim, code_dim=64, hidden=256, n_codes=N_CODES):
        super().__init__()
        self.encoder  = Encoder(dim, code_dim, hidden)
        self.codebook = GeoCodebook(code_dim, n_codes=n_codes)
        self.decoder  = Decoder(dim, code_dim, hidden)
        self.dim = dim; self.code_dim = code_dim

    def forward(self, x):
        z               = self.encoder(x)
        z_q, idx, cl, m = self.codebook(z)
        geo             = GEO_TABLE[idx]
        x_hat           = self.decoder(z_q, geo)
        return x_hat, idx, cl, m, z   # return z for diversity loss

    def loss(self, x, x_hat, commit_loss, idx, z, beta=0.25, lam_trav=0.05, lam_div=0.2):
        recon  = F.mse_loss(x_hat, x)

        # diversity loss: push encoder outputs apart
        z_n      = F.normalize(z, dim=-1)
        sim      = (z_n @ z_n.T)
        mask     = ~torch.eye(sim.shape[0], dtype=torch.bool, device=z.device)
        div_loss = sim[mask].clamp(min=0).mean()   # only penalize positive similarity

        # traverse consistency
        anchor = x_hat.detach()
        trav_loss = torch.tensor(0.0, device=x.device)
        for mode in range(N_ACTIONS):
            nxt     = self.traverse(idx, mode)
            z_q_nxt = self.codebook.codes[nxt]
            geo_nxt = GEO_TABLE[nxt]
            dec_nxt = self.decoder(z_q_nxt, geo_nxt)
            trav_loss = trav_loss + F.mse_loss(dec_nxt, anchor)
        trav_loss = trav_loss / N_ACTIONS

        total = recon + beta * commit_loss + lam_trav * trav_loss + lam_div * div_loss
        return total, recon.item(), commit_loss.item(), trav_loss.item(), div_loss.item()

    def encode(self, x):
        with torch.no_grad():
            z = self.encoder(x)
            _, idx, _, _ = self.codebook(z)
        return idx

    def decode(self, idx):
        with torch.no_grad():
            z_q = self.codebook.codes[idx]
            geo = GEO_TABLE[idx]
            return self.decoder(z_q, geo)

    def traverse(self, idx, mode=0):
        return self.codebook.traverse(idx, mode)

# ═══════════════════════════════════════════════════════════
# TEXT PIPELINE
# ═══════════════════════════════════════════════════════════

# Sample sentences — replace with your own dataset
SENTENCES = [
    # Topic A: Technology (20)
    "Machine learning transforms how computers learn from data.",
    "Neural networks mimic the structure of the human brain.",
    "Deep learning requires large amounts of training data.",
    "Transformers revolutionized natural language processing.",
    "GPUs accelerate matrix operations in AI training.",
    "Convolutional networks excel at image recognition tasks.",
    "Reinforcement learning trains agents through reward signals.",
    "Attention mechanisms allow models to focus on relevant input.",
    "Embeddings represent words as dense numerical vectors.",
    "Backpropagation computes gradients through the network layers.",
    "Batch normalization stabilizes training of deep networks.",
    "Dropout regularization prevents overfitting in neural nets.",
    "Transfer learning reuses pretrained model weights.",
    "Quantization reduces model size with minimal accuracy loss.",
    "Edge computing brings AI inference closer to the data source.",
    "Federated learning trains models without centralizing data.",
    "Sparse attention reduces the quadratic cost of transformers.",
    "Knowledge distillation compresses large models into smaller ones.",
    "Self-supervised learning uses unlabeled data for pretraining.",
    "Mixture of experts routes inputs to specialized subnetworks.",
    # Topic B: Nature (20)
    "The ocean covers more than 70 percent of Earth's surface.",
    "Rainforests produce a significant portion of Earth's oxygen.",
    "Coral reefs are among the most biodiverse ecosystems.",
    "Migratory birds navigate using Earth's magnetic field.",
    "Plants convert sunlight into energy through photosynthesis.",
    "Glaciers store a large fraction of the world's freshwater.",
    "Mangrove forests protect coastlines from storm surges.",
    "Bees pollinate a third of the food crops humans consume.",
    "Wolves reshape ecosystems by regulating prey populations.",
    "Deep sea vents host life without sunlight or photosynthesis.",
    "Mycorrhizal networks connect tree roots underground.",
    "Seasonal migrations move billions of animals each year.",
    "Soil microbes recycle nutrients essential for plant growth.",
    "Tidal rhythms govern the behavior of coastal species.",
    "Fire is a natural part of many forest ecosystems.",
    "Permafrost stores ancient carbon accumulated over millennia.",
    "River deltas are among the most fertile regions on Earth.",
    "Biodiversity increases ecosystem resilience to disturbance.",
    "Camouflage evolves as an arms race between predator and prey.",
    "Wetlands filter pollutants and store floodwater naturally.",
    # Topic C: History (20)
    "Ancient civilizations built complex irrigation systems.",
    "The printing press accelerated the spread of knowledge.",
    "Trade routes connected distant cultures for millennia.",
    "Written language emerged independently in multiple regions.",
    "Architectural monuments reflect the values of their era.",
    "The agricultural revolution enabled permanent settlements.",
    "Bronze tools replaced stone and transformed early societies.",
    "Maritime exploration expanded European knowledge of the world.",
    "The industrial revolution mechanized production at scale.",
    "Empires rose and fell along strategic river valleys.",
    "Philosophical schools in Greece shaped Western thought.",
    "The Silk Road exchanged goods and ideas across continents.",
    "Colonial expansion reshaped economies and populations globally.",
    "The Renaissance revived classical art and scientific inquiry.",
    "Democratic ideals spread through revolutions in the 18th century.",
    "Railways compressed distances and unified national markets.",
    "World wars redrew borders and created international institutions.",
    "The Cold War divided the world into competing ideological blocs.",
    "Decolonization created dozens of new nation states after 1945.",
    "The internet compressed communication to near-instantaneous speed.",
    # Topic D: Daily life (20)
    "Morning routines help people start the day with focus.",
    "Cooking at home is often healthier than eating out.",
    "Regular exercise improves both physical and mental health.",
    "Sleep quality affects cognitive performance significantly.",
    "Social connections contribute to long-term well-being.",
    "Meal planning reduces food waste and saves money.",
    "Walking outdoors lowers stress and improves mood.",
    "Reading before bed improves sleep quality for many people.",
    "Staying hydrated supports concentration and energy levels.",
    "Decluttering living spaces reduces mental load.",
    "Learning a new skill keeps the brain adaptable.",
    "Journaling helps process emotions and clarify thinking.",
    "Time spent in nature restores attention and reduces anxiety.",
    "Consistent sleep schedules regulate circadian rhythms.",
    "Mindful breathing activates the parasympathetic nervous system.",
    "Small habits compound into significant changes over time.",
    "Gratitude practice shifts focus toward positive experiences.",
    "Digital detox periods improve focus and presence.",
    "Strong friendships buffer against the effects of stress.",
    "Purpose-driven work increases motivation and satisfaction.",
    # Topic E: Science (20)
    "Quantum mechanics describes the behavior of subatomic particles.",
    "Relativity links space, time, mass, and energy.",
    "DNA encodes the instructions for building living organisms.",
    "Evolution explains the diversity of life through natural selection.",
    "Entropy measures the disorder of a thermodynamic system.",
    "Black holes form when mass collapses beyond a critical density.",
    "The periodic table organizes elements by atomic number.",
    "Plate tectonics explains the movement of Earth's crust.",
    "Vaccines train the immune system to recognize pathogens.",
    "The speed of light is constant in all inertial frames.",
    "Neutrinos pass through matter almost without interaction.",
    "CRISPR enables precise editing of genetic sequences.",
    "The Big Bang theory describes the origin of the observable universe.",
    "Stem cells can differentiate into many specialized cell types.",
    "Antibiotics disrupt bacterial cell wall synthesis.",
    "Nuclear fusion powers stars by combining hydrogen into helium.",
    "Superposition allows quantum systems to exist in multiple states.",
    "The double helix structure of DNA was confirmed in 1953.",
    "Dark matter influences galactic rotation without emitting light.",
    "Epigenetics studies heritable changes beyond the DNA sequence.",
]

def run_text_pipeline():
    print("=== VQ-POGLS Text Pipeline ===\n")

    # ── 1. Embed sentences ───────────────────────────────
    print("Loading sentence embeddings...")
    from sentence_transformers import SentenceTransformer
    embedder = SentenceTransformer('all-MiniLM-L6-v2')   # dim=384
    embeddings = embedder.encode(SENTENCES, convert_to_tensor=True,
                                  show_progress_bar=False)
    embeddings = embeddings.to(DEVICE)
    DIM = embeddings.shape[1]   # 384
    print(f"Embedded {len(SENTENCES)} sentences → dim={DIM}\n")

    # ── 2. Train VQ-POGLS on embeddings ─────────────────
    CODE_DIM = 64
    N_CODES_OVERRIDE = max(20, len(SENTENCES) // 2)  # scale to data size
    BATCH    = 16     # small — only 20 sentences
    EPOCHS   = 600
    LR       = 1e-3

    model = VqPogls(DIM, CODE_DIM, n_codes=N_CODES_OVERRIDE).to(DEVICE)
    opt   = torch.optim.Adam(
        list(model.encoder.parameters()) + list(model.decoder.parameters()), lr=LR)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, EPOCHS)

    print(f"Training VqPogls  dim={DIM}  code_dim={CODE_DIM}  "
          f"n_codes={N_CODES_OVERRIDE}  epochs={EPOCHS}")
    t0 = time.perf_counter()
    for ep in range(EPOCHS):
        idx_b = torch.randint(0, len(embeddings), (BATCH,))
        x     = embeddings[idx_b]
        x_hat, idx, cl, metrics, z = model(x)
        loss, recon, commit, trav, div = model.loss(x, x_hat, cl, idx, z)
        opt.zero_grad(); loss.backward()
        nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step(); sched.step()

        if (ep+1) % 150 == 0:
            print(f"  ep={ep+1:>4}  loss={loss.item():.4f}  "
                  f"recon={recon:.4f}  trav={trav:.4f}  div={div:.3f}  "
                  f"usage={metrics['usage']:.1%}")

    print(f"Trained in {time.perf_counter()-t0:.1f}s\n")

    # ── 3. Encode all sentences → geometry addresses ─────
    model.eval()
    all_idx = model.encode(embeddings)   # [N] geometry positions

    print("=== Sentence → Geometry Address ===")
    for i, (sent, pos) in enumerate(zip(SENTENCES, all_idx.tolist())):
        enc  = (pos * 37) % 720
        zone = enc // 60
        print(f"  [{i:>2}] zone={zone}  pos={pos:>3}  \"{sent[:55]}\"")

    # ── 4. Traverse: find related sentences ──────────────
    print("\n=== Traverse: CHIRAL = jump to related zone ===")
    QUERY_IDX = 0   # "Machine learning transforms..."
    q_pos     = all_idx[QUERY_IDX].unsqueeze(0)

    for mode, name in enumerate(["ORBITAL","CHIRAL","CROSS","HUB"]):
        nxt_pos = model.traverse(q_pos, mode)[0].item()
        # find nearest sentence to traversed position
        nxt_geo  = GEO_TABLE[nxt_pos].unsqueeze(0)
        all_geo  = GEO_TABLE[all_idx]
        dists    = (all_geo - nxt_geo).pow(2).sum(-1)
        nearest  = dists.argmin().item()
        print(f"  {name:7s} pos {q_pos[0].item():>3}→{nxt_pos:>3}  "
              f"nearest: \"{SENTENCES[nearest][:50]}\"")

    # ── 5. Zone clustering: which zone = which topic? ────
    print("\n=== Zone Distribution (should cluster by topic) ===")
    zone_sentences = {i: [] for i in range(N_ZONES)}
    for i, pos in enumerate(all_idx.tolist()):
        enc  = (pos * 37) % 720
        zone = enc // 60
        zone_sentences[zone].append(i)

    for zone, idxs in zone_sentences.items():
        if idxs:
            labels = [SENTENCES[i][:35] for i in idxs]
            print(f"  zone {zone:>2}: {labels}")

    # ── 6. Save model ────────────────────────────────────
    torch.save(model.state_dict(), "vqpogls_text.pt")
    print("\nModel saved → vqpogls_text.pt")
    print("\n✓ Text pipeline complete")

    return model, all_idx

if __name__ == "__main__":
    model, indices = run_text_pipeline()
