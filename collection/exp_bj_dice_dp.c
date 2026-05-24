/*
 * exp_bj_dice_dp.c — Blackjack Dice V8.9 Exact Value Iteration
 *
 * State space: (player_sum 2-21) × (house_visible 1-6) = 120 states
 * Actions:     Stand / Hit / Counter (if sum ≥ 15, -$50 fee)
 *
 * Payouts (gross, before bets):
 *   Standard win:  +$200  (player > house, or house bust)
 *   Counter bonus: +$350  (counter + final sum == 21)
 *   Loss:          -$100  (player bust, or player < house)
 *
 * House plays after player stands:
 *   reveal hidden die → +1d6 until ≥ 17 (bust if > 21)
 *
 * Assumptions:
 *   - Multiple hits allowed (standard BJ)
 *   - Counter ends player phase immediately
 *   - House hits with +1d6 per step (not 2d6)
 *   - Counter without perfect 21 that still beats house = $200 win
 *   - Net EV = payout - bet (bet already deducted) - counter_fee (if used)
 *
 * === NO TRAINING NEEDED ===
 * DP converges in < 50 iterations (~milliseconds)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ─── Game Parameters ─────────────────────────────────────────── */
#define DICE_SIDES       6
#define BJ_TARGET       21
#define HOUSE_STAND     17
#define BET            100
#define WIN_PAY        200
#define COUNTER_BONUS  350
#define COUNTER_LOSS_PEN 150
#define P_START_DICE     4
#define HIT_DICE         2
#define COUNTER_DICE     1
#define H_HIDDEN_DICE    1
#define H_HIT_DICE       2

#define MIN_P_SUM    2
#define MAX_P_SUM    BJ_TARGET
#define MIN_H_VIS    1
#define MAX_H_VIS    6
#define MIN_H_START  2         /* 2d6 = 2-12 */
#define MAX_H_START 12
#define MAX_FINAL   30         /* enough for house hit chains */

/* ─── Precomputed Distributions ───────────────────────────────── */
static double d_hit_dist[13];            /* HIT_DICE dice sum prob */
static double h_dist[MAX_H_START+1][MAX_FINAL+1]; /* house start → final sum prob */
static double pb_dist[25];               /* P_START_DICE d6 initial sum prob */

static void init_hit_dist(void) {
    memset(d_hit_dist, 0, sizeof(d_hit_dist));
    int N = HIT_DICE;
    int dp[5][13] = {{0}}; /* max 4 dice × 12 sum */
    dp[0][0] = 1;
    for (int n = 1; n <= N; n++)
        for (int s = 0; s <= 6*n; s++)
            for (int d = 1; d <= DICE_SIDES && d <= s; d++)
                dp[n][s] += dp[n-1][s-d];
    double total = 0;
    for (int s = N; s <= 6*N; s++) { d_hit_dist[s] = dp[N][s]; total += dp[N][s]; }
    for (int s = N; s <= 6*N; s++) d_hit_dist[s] /= total;
}

static double h_inc_dist[13];          /* H_HIT_DICE dice distribution */
/* recursive helper: house hit distribution (increment by h_inc_dist) */
static void house_dist_recur(int cur, double *out, double prob) {
    if (cur > BJ_TARGET) { out[MAX_FINAL] += prob; return; }
    if (cur >= HOUSE_STAND) { out[cur] += prob; return; }
    for (int s = H_HIT_DICE; s <= H_HIT_DICE * DICE_SIDES; s++)
        house_dist_recur(cur + s, out, prob * h_inc_dist[s]);
}

static void init_h_inc(void) {
    memset(h_inc_dist, 0, sizeof(h_inc_dist));
    int dp[5][13] = {{0}}; dp[0][0] = 1;
    for (int n = 1; n <= H_HIT_DICE; n++)
        for (int s = 0; s <= 6*n; s++)
            for (int d = 1; d <= DICE_SIDES && d <= s; d++)
                dp[n][s] += dp[n-1][s-d];
    double total = 0;
    for (int s = H_HIT_DICE; s <= 6*H_HIT_DICE; s++) { h_inc_dist[s] = dp[H_HIT_DICE][s]; total += dp[H_HIT_DICE][s]; }
    for (int s = H_HIT_DICE; s <= 6*H_HIT_DICE; s++) h_inc_dist[s] /= total;
}

static void init_house(void) {
    init_h_inc();
    for (int s = MIN_H_START; s <= MAX_H_START; s++) {
        memset(h_dist[s], 0, sizeof(h_dist[s]));
        house_dist_recur(s, h_dist[s], 1.0);
    }
}

static void init_pb(void) {
    memset(pb_dist, 0, sizeof(pb_dist));
    int N = P_START_DICE;
    /* recurrence: dp[n][s] = number of ways sum s with n dice */
    int dp[9][25] = {{0}}; /* max 8 dice × 24 sum */
    dp[0][0] = 1;
    for (int n = 1; n <= N; n++)
        for (int s = 0; s <= 6*n; s++)
            for (int d = 1; d <= DICE_SIDES && d <= s; d++)
                dp[n][s] += dp[n-1][s-d];
    double total = 0;
    for (int s = N; s <= 6*N; s++) {
        pb_dist[s] = dp[N][s];
        total += pb_dist[s];
    }
    for (int s = N; s <= 6*N; s++) pb_dist[s] /= total;
}

/* ─── House Outcome Distribution ──────────────────────────────── */
/* Given house start sum, returns P(player_win / push / loss) vs player_sum */
static double house_win_prob(int h_start, int p_sum) {
    double pw = 0;
    for (int hf = HOUSE_STAND; hf <= BJ_TARGET; hf++)
        if (h_dist[h_start][hf] > 0 && p_sum > hf)
            pw += h_dist[h_start][hf];
    pw += h_dist[h_start][MAX_FINAL]; /* house bust = player win */
    return pw;
}
static double house_push_prob(int h_start, int p_sum) {
    double pp = 0;
    for (int hf = HOUSE_STAND; hf <= BJ_TARGET; hf++)
        if (h_dist[h_start][hf] > 0 && p_sum == hf)
            pp += h_dist[h_start][hf];
    return pp;
}
static double house_loss_prob(int h_start, int p_sum) {
    double pl = 0;
    for (int hf = HOUSE_STAND; hf <= BJ_TARGET; hf++)
        if (h_dist[h_start][hf] > 0 && p_sum < hf)
            pl += h_dist[h_start][hf];
    return pl;
}

/* ─── Index Helpers ──────────────────────────────────────────── */
/* flat index: idx = (p_sum - MIN_P_SUM) * N_H + (hv - MIN_H_VIS) */
#define N_P (MAX_P_SUM - MIN_P_SUM + 1)
#define N_H (MAX_H_VIS - MIN_H_VIS + 1)
#define N_S (N_P * N_H)

/* ─── Action EV Functions ────────────────────────────────────── */
/*
 * V(p,hv) = expected total cash flow from state (p,hv) onward.
 * Initial bet (-BET) is NOT included in V; it is added in house_edge().
 *
 * Payouts (from state perspective, initial bet already deducted):
 *   Stand win: +WIN_PAY  (200)  → recover bet + profit
 *   Stand push: +BET      (100) → recover bet only
 *   Stand loss: 0               → bet already gone
 *   Hit bust:   0               → bet already gone
 */
static double ev_stand(int p, int hv) {
    double ev = 0;
    for (int hh = 1; hh <= DICE_SIDES; hh++) {
        int hs = hv + hh;
        double pw = house_win_prob(hs, p);
        double pp = house_push_prob(hs, p);
        double pl = house_loss_prob(hs, p);
        ev += (1.0 / DICE_SIDES) * (pw * WIN_PAY + pp * BET + pl * 0);
    }
    return ev;
}

/* Hit: +HIT_DICE d6, if bust → 0, else continue from V[p'] */
static double ev_hit(int p, int hv, double *V) {
    double ev = 0;
    for (int s = HIT_DICE; s <= HIT_DICE * DICE_SIDES; s++) {
        int pn = p + s;
        if (pn > BJ_TARGET)
            ev += d_hit_dist[s] * 0;
        else
            ev += d_hit_dist[s] * V[(pn - MIN_P_SUM) * N_H + (hv - MIN_H_VIS)];
    }
    return ev;
}

/* Counter: no upfront fee, but +$50 penalty on loss/bust only.
 * Payout (from state, bet already deducted):
 *   Perfect 21: +350
 *   Win <21:    +200 (standard win)
 *   Push:       +100 (bet back)
 *   Bust/loss:   -50 (penalty on top of lost bet)
 */
/* extra penalty on counter loss */

static double ev_counter(int p, int hv) {
    double ev = 0;  /* no upfront fee */
    for (int hh = 1; hh <= DICE_SIDES; hh++) {
        double phh = 1.0 / DICE_SIDES;
        int hs = hv + hh;

        /* house final ≤ 21 */
        for (int hf = HOUSE_STAND; hf <= BJ_TARGET; hf++) {
            double phf = h_dist[hs][hf];
            if (phf == 0) continue;
            for (int de = 1; de <= DICE_SIDES; de++) {
                int pn = p + de;
                if (pn > BJ_TARGET)
                    ev += phh * phf * (1.0/DICE_SIDES) * (-COUNTER_LOSS_PEN);
                else if (pn == BJ_TARGET)
                    ev += phh * phf * (1.0/DICE_SIDES) * (COUNTER_BONUS);
                else if (pn > hf)
                    ev += phh * phf * (1.0/DICE_SIDES) * (WIN_PAY);
                else if (pn == hf)
                    ev += phh * phf * (1.0/DICE_SIDES) * (BET);
                else
                    ev += phh * phf * (1.0/DICE_SIDES) * (-COUNTER_LOSS_PEN);
            }
        }
        /* house bust */
        double phb = h_dist[hs][MAX_FINAL];
        if (phb > 0) {
            for (int de = 1; de <= DICE_SIDES; de++) {
                int pn = p + de;
                if (pn > BJ_TARGET)
                    ev += phh * phb * (1.0/DICE_SIDES) * (-COUNTER_LOSS_PEN);
                else if (pn == BJ_TARGET)
                    ev += phh * phb * (1.0/DICE_SIDES) * (COUNTER_BONUS);
                else
                    ev += phh * phb * (1.0/DICE_SIDES) * (WIN_PAY);
            }
        }
    }
    return ev;
}

/* ─── Value Iteration ─────────────────────────────────────────── */

static void value_iter(double theta, int max_iter, double *V, int *pol) {
    /* init V = 0 */
    memset(V, 0, N_S * sizeof(double));
    memset(pol, 0, N_S * sizeof(int));

    for (int iter = 0; iter < max_iter; iter++) {
        double delta = 0;
        for (int ps = MIN_P_SUM; ps <= MAX_P_SUM; ps++) {
            for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
                int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
                double v0 = V[idx];

                double es = ev_stand(ps, hv);
                double eh = ev_hit(ps, hv, V);

                double best = es;
                int ba = 0; /* 0=stand */

                if (eh > best) { best = eh; ba = 1; }

                if (ps >= 15) {
                    double ec = ev_counter(ps, hv);
                    if (ec > best) { best = ec; ba = 2; }
                }

                V[idx] = best;
                pol[idx] = ba;
                double d = fabs(best - v0);
                if (d > delta) delta = d;
            }
        }
        if (iter < 5 || iter % 10 == 1)
            printf("  iter %4d  delta = %.8f\n", iter, delta);
        if (delta < theta) { printf("  ✓ converged at iter %d\n", iter); break; }
    }
}

/* ─── Output ─────────────────────────────────────────────────── */
static const char *act_name(int a) {
    return a == 0 ? "S" : (a == 1 ? "H" : "C");
}

static void print_policy(double *V, int *pol) {
    printf("\n");
    printf("┌─────────── Optimal Policy ───────────────┐\n");
    printf("│           House Visible Die               │\n");
    printf("│ p_sum ");
    for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++)
        printf("  hv=%d  ", hv);
    printf("│\n");
    printf("├───────");
    for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++)
        printf("────────");
    printf("┤\n");

    for (int ps = MAX_P_SUM; ps >= MIN_P_SUM; ps--) {
        printf("│ %3d   ", ps);
        for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
            int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
            int a = pol[idx];
            if (a == 2) printf("  C(%+4.0f)", V[idx]);
            else if (a == 1) printf("  H(%+4.0f)", V[idx]);
            else printf("  S(%+4.0f)", V[idx]);
        }
        printf("│\n");
    }
    printf("└───────");
    for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++)
        printf("────────");
    printf("┘\n");
    printf("  S=Stand  H=Hit  C=Counter\n");
    printf("  EV in units of $ (bet=%d, penalty on counter loss=%d)\n\n", BET, COUNTER_LOSS_PEN);
}

static void print_compact(double *V, int *pol) {
    printf("Compact: p_sum → action(EV) per hv=1..6\n\n");
    for (int ps = MAX_P_SUM; ps >= MIN_P_SUM; ps--) {
        printf("p=%2d ", ps);
        for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
            int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
            printf("%s(%+.0f) ", act_name(pol[idx]), V[idx]);
        }
        printf("\n");
    }
}

/* ─── Monte Carlo Verification ────────────────────────────────── */
/* Full recursive game rollout following the optimal policy.
 * Returns cash flow from this state onward (initial -BET NOT included). */
static double mc_play_game(int p, int hv, int *pol) {
    int hh = 1 + rand() % 6;
    int hs = hv + hh;
    int pn = p;
    int counter_used = 0;

    /* player decision loop */
    while (1) {
        int idx = (pn - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
        int action = pol[idx];
        if (action == 0) break; /* stand */
        if (action == 2 && pn >= 15) {
            counter_used = 1;
            break;
        }
        /* hit: roll HIT_DICE dice */
        int r = 0;
        for (int _i = 0; _i < HIT_DICE; _i++) r += 1 + rand() % 6;
        pn += r;
        if (pn > BJ_TARGET) return 0; /* bust, no further cash */
    }

    /* house plays: +H_HIT_DICE d6 until ≥ HOUSE_STAND */
    int hf = hs;
    while (hf < HOUSE_STAND) {
        int inc = 0;
        for (int _i = 0; _i < H_HIT_DICE; _i++) inc += 1 + rand() % 6;
        hf += inc;
    }

    if (counter_used) {
        int de = 1 + rand() % 6;
        pn += de;
        if (pn > BJ_TARGET) return -COUNTER_LOSS_PEN;
        if (pn == BJ_TARGET) return COUNTER_BONUS;
        if (hf > BJ_TARGET || pn > hf) return WIN_PAY;
        if (pn == hf) return BET;
        return -COUNTER_LOSS_PEN;
    }

    if (hf > BJ_TARGET || pn > hf) return WIN_PAY;
    if (pn == hf) return BET;
    return 0; /* loss, no penalty */
}

static void verify_mc(double *V, int *pol, int n) {
    printf("─── MC Verification (%d games per state, recursive policy) ───\n", n);
    double max_err = 0;
    for (int ps = MIN_P_SUM; ps <= MAX_P_SUM; ps++) {
        for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
            int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
            long long total = 0;
            for (int t = 0; t < n; t++)
                total += mc_play_game(ps, hv, pol);
            double mc = (double)total / n;
            double dp = V[idx];
            double err = fabs(mc - dp);
            if (err > max_err) max_err = err;
            if (err > 3.0) {
                printf("  Δ at (p=%2d, hv=%d): DP=%+7.2f  MC=%+7.2f  |Δ|=%.2f\n",
                       ps, hv, dp, mc, err);
            }
        }
    }
    printf("  max |Δ| = %.2f  (DP vs %d-game MC)\n\n", max_err, n);
}

/* ─── Overall House Edge ─────────────────────────────────────── */
static void house_edge(double *V) {
    double ev = 0;
    for (int ps = P_START_DICE; ps <= P_START_DICE * DICE_SIDES; ps++) {
        double pp = pb_dist[ps];
        if (pp == 0) continue;
        if (ps > BJ_TARGET) {
            ev += pp * (-BET); /* initial bust: -100 + 0 */
            continue;
        }
        for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
            for (int hh = 1; hh <= DICE_SIDES; hh++) {
                double ph = (1.0 / DICE_SIDES) * (1.0 / DICE_SIDES);
                int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
                ev += pp * ph * (-BET + V[idx]); /* initial bet + state value */
            }
        }
    }
    printf("─── Overall Game EV ───\n");
    printf("  Player advantage (optimal):  %+.4f  (%+.2f%%)\n", ev, ev / BET * 100);
    printf("  House edge (optimal):        %+.4f  (%+.2f%%)\n", -ev, -ev / BET * 100);
}

/* ─── Suboptimal Strategy Benchmarks ──────────────────────────── */
/* For always-hit, we need the optimal V to evaluate recursive hitting.
   After standing, the game follows the same state transition. We use
   a helper that computes the EV of "hit until threshold" by solving
   a simplified DP where the only action is to keep hitting. */
static void value_iter_hit_only(double *V) {
    memset(V, 0, N_S * sizeof(double));
    for (int iter = 0; iter < 500; iter++) {
        double delta = 0;
        for (int ps = MIN_P_SUM; ps <= MAX_P_SUM; ps++) {
            for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
                int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
                double v0 = V[idx];
                V[idx] = ev_hit(ps, hv, V);
                /* but if eh shows it's worse to keep hitting, let stand win:
                   this is a "forced hit" policy — player always hits */
                double d = fabs(V[idx] - v0);
                if (d > delta) delta = d;
            }
        }
        if (delta < 1e-9) break;
    }
}

static void benchmark_suboptimal(double *V_opt) {
    /* Precompute always-hit DP */
    double *V_hit = malloc(N_S * sizeof(double));
    value_iter_hit_only(V_hit);

    printf("─── Suboptimal Strategy Benchmarks ───\n");

    /* Helper: compute total EV for a strategy that maps state → cash flow */
    /* Strategy fn: returns cash flow from state (without initial -BET) */
    #define P_INIT(hv, hh) (1.0 / 36.0)

    double ev_s = 0, ev_h = 0, ev_c = 0, ev_o = 0;
    for (int ps = P_START_DICE; ps <= P_START_DICE * DICE_SIDES; ps++) {
        double pp = pb_dist[ps]; if (pp == 0) continue;
        if (ps > BJ_TARGET) {
            ev_s += pp * (-BET);
            ev_h += pp * (-BET);
            ev_c += pp * (-BET);
            ev_o += pp * (-BET);
            continue;
        }
        for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
            for (int hh = 1; hh <= DICE_SIDES; hh++) {
                double ph = P_INIT(hv, hh);
                /* Always Stand */
                ev_s += pp * ph * (-BET + ev_stand(ps, hv));
                /* Always Hit */
                ev_h += pp * ph * (-BET + V_hit[(ps-MIN_P_SUM)*N_H+(hv-MIN_H_VIS)]);
                /* Always Counter (if ≥15) */
                double cf = (ps >= 15) ? ev_counter(ps, hv) : ev_stand(ps, hv);
                ev_c += pp * ph * (-BET + cf);
                /* Optimal */
                ev_o += pp * ph * (-BET + V_opt[(ps-MIN_P_SUM)*N_H+(hv-MIN_H_VIS)]);
            }
        }
    }

    printf("  %-24s  EV = %+.4f  (house edge = %+.2f%%)\n",
           "Always Stand", ev_s, -ev_s / BET * 100);
    printf("  %-24s  EV = %+.4f  (house edge = %+.2f%%)\n",
           "Always Hit", ev_h, -ev_h / BET * 100);
    printf("  %-24s  EV = %+.4f  (house edge = %+.2f%%)\n",
           "Always Counter (≥15)", ev_c, -ev_c / BET * 100);
    printf("  %-24s  EV = %+.4f  (house edge = %+.2f%%)\n",
           "Optimal (DP)", ev_o, -ev_o / BET * 100);
    printf("\n");

    free(V_hit);
}

/* ─── Hit Policy: threshold analysis ──────────────────────────── */
/* Derive stand threshold: at what p_sum does the optimal policy
   switch from hit to stand, per hv? */
static void stand_thresholds(int *pol) {
    printf("─── Stand Thresholds ───\n");
    printf("  hv | stand when p_sum ≥\n");
    for (int hv = MIN_H_VIS; hv <= MAX_H_VIS; hv++) {
        int thresh = BJ_TARGET + 1;
        for (int ps = MIN_P_SUM; ps <= MAX_P_SUM; ps++) {
            int idx = (ps - MIN_P_SUM) * N_H + (hv - MIN_H_VIS);
            if (pol[idx] != 1) { /* not hit = stand or counter */
                thresh = ps;
                break;
            }
        }
        printf("  %2d | %d\n", hv, thresh);
    }
}

/* ─── Main ────────────────────────────────────────────────────── */
int main(void) {
    printf("═══════════════════════════════════════════════════\n");
    printf("  Blackjack Dice V8.9 — Exact Value Iteration\n");
    printf("═══════════════════════════════════════════════════\n\n");

    /* init */
    init_hit_dist();
    init_house();
    init_pb();

    printf("  States: %d (p_sum %d-%d × hv 1-6)\n", N_S, MIN_P_SUM, MAX_P_SUM);
    printf("  Actions: Stand / Hit / Counter (≥15, +$%d on loss only)\n\n", COUNTER_LOSS_PEN);

    /* DP */
    double *V = malloc(N_S * sizeof(double));
    int *pol = malloc(N_S * sizeof(int));
    if (!V || !pol) { fprintf(stderr, "malloc fail\n"); return 1; }

    printf("─── Value Iteration ───\n");
    value_iter(1e-9, 500, V, pol);

    /* Output */
    print_policy(V, pol);
    print_compact(V, pol);
    stand_thresholds(pol);
    printf("\n");

    /* MC verification */
    srand((unsigned)time(NULL));
    verify_mc(V, pol, 100000);

    /* Overall house edge */
    house_edge(V);
    printf("\n");

    /* Suboptimal benchmarks */
    benchmark_suboptimal(V);

    free(V);
    free(pol);
    return 0;
}
