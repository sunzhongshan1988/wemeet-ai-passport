// main/wm_rs.c —— GF(2^8) Reed-Solomon 编码与译码(Berlekamp-Massey + Chien + Forney)。
// 约定见 wm_rs.h。多项式一律"低次在前":p[i] 是 x^i 的系数。
#include "wm_rs.h"

#include <stdbool.h>
#include <string.h>

// 指数表开到 512 项,gf_mul 里两个对数相加后不用再取模。
static uint8_t s_exp[512];
static uint8_t s_log[256];
static bool s_ready;

// 首次使用时生成对数/指数表。只写入确定值,多任务同时首次调用也只是重复写同样内容。
static void gf_init(void) {
    if (s_ready) return;
    int x = 1;
    for (int i = 0; i < 255; i++) {
        s_exp[i] = (uint8_t)x;
        s_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;   // 本原多项式 x^8+x^4+x^3+x^2+1
    }
    for (int i = 255; i < 512; i++) s_exp[i] = s_exp[i - 255];
    s_ready = true;
}

static inline uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return s_exp[s_log[a] + s_log[b]];
}

// a / b,b 不得为 0(调用方保证)。
static inline uint8_t gf_div(uint8_t a, uint8_t b) {
    if (a == 0) return 0;
    return s_exp[s_log[a] + 255 - s_log[b]];
}

// α 的任意整数次幂(可为负)。
static inline uint8_t gf_pow_alpha(int e) {
    e %= 255;
    if (e < 0) e += 255;
    return s_exp[e];
}

void wm_rs_encode(const uint8_t *data, int len, int npar, uint8_t *parity) {
    gf_init();
    // 生成多项式 g(x) = Π (x - α^i),i = 0..npar-1。这里按"高次在前"存放,gen[0] = 1。
    uint8_t gen[WM_RS_MAX_PARITY + 1];
    memset(gen, 0, sizeof(gen));
    gen[0] = 1;
    for (int i = 0; i < npar; i++) {
        // 乘以 (x + α^i):从高位向低位更新,避免覆盖还没用到的系数。
        for (int j = i + 1; j > 0; j--) {
            gen[j] = (uint8_t)(gen[j] ^ gf_mul(gen[j - 1], s_exp[i]));
        }
    }
    // LFSR 长除法:parity = data(x) * x^npar mod g(x)。
    memset(parity, 0, (size_t)npar);
    for (int i = 0; i < len; i++) {
        uint8_t fb = (uint8_t)(data[i] ^ parity[0]);
        memmove(parity, parity + 1, (size_t)(npar - 1));
        parity[npar - 1] = 0;
        if (fb) {
            for (int j = 0; j < npar; j++) {
                parity[j] = (uint8_t)(parity[j] ^ gf_mul(fb, gen[j + 1]));
            }
        }
    }
}

int wm_rs_decode(uint8_t *msg, int n, int npar) {
    gf_init();
    if (npar < 2 || npar > WM_RS_MAX_PARITY || n <= npar || n > 255) return -1;

    // 1) 伴随式 S_i = r(α^i)。msg[0] 是最高次项(x^(n-1) 的系数),用 Horner 求值。
    uint8_t synd[WM_RS_MAX_PARITY];
    bool clean = true;
    for (int i = 0; i < npar; i++) {
        uint8_t s = 0;
        for (int j = 0; j < n; j++) s = (uint8_t)(gf_mul(s, s_exp[i]) ^ msg[j]);
        synd[i] = s;
        if (s) clean = false;
    }
    if (clean) return 0;

    // 2) Berlekamp-Massey 求错误位置多项式 Λ(x),低次在前,Λ[0] = 1。
    uint8_t lam[WM_RS_MAX_PARITY + 1], prev[WM_RS_MAX_PARITY + 1], tmp[WM_RS_MAX_PARITY + 1];
    memset(lam, 0, sizeof(lam));
    memset(prev, 0, sizeof(prev));
    lam[0] = prev[0] = 1;
    int L = 0;        // 当前 LFSR 长度 = 已假定的错误个数
    int m = 1;        // 距上次长度变化经过的步数
    uint8_t b = 1;    // 上次长度变化时的差值
    for (int r = 0; r < npar; r++) {
        uint8_t delta = synd[r];
        for (int i = 1; i <= L; i++) delta = (uint8_t)(delta ^ gf_mul(lam[i], synd[r - i]));
        if (delta == 0) {
            m++;
            continue;
        }
        memcpy(tmp, lam, sizeof(lam));
        uint8_t coef = gf_div(delta, b);
        for (int i = 0; i + m <= npar; i++) {
            lam[i + m] = (uint8_t)(lam[i + m] ^ gf_mul(coef, prev[i]));
        }
        if (2 * L <= r) {
            L = r + 1 - L;
            memcpy(prev, tmp, sizeof(prev));
            b = delta;
            m = 1;
        } else {
            m++;
        }
    }
    if (L == 0 || 2 * L > npar) return -1;

    // 3) Chien 搜索:Λ(α^-p) = 0 说明位置 p(x^p 的系数,即 msg[n-1-p])出错。
    uint8_t pos[WM_RS_MAX_PARITY / 2];
    int found = 0;
    for (int p = 0; p < n; p++) {
        uint8_t xinv = gf_pow_alpha(-p);
        uint8_t v = 0;
        for (int i = L; i >= 0; i--) v = (uint8_t)(gf_mul(v, xinv) ^ lam[i]);
        if (v == 0) {
            if (found >= L) return -1;
            pos[found++] = (uint8_t)p;
        }
    }
    // 根的个数与 Λ 的次数不符 → 错误数超出能力,不能相信任何"纠正"。
    if (found != L) return -1;

    // 4) Forney:Ω(x) = S(x)Λ(x) mod x^npar;误差值 e = X · Ω(X^-1) / Λ'(X^-1)(fcr = 0)。
    uint8_t omega[WM_RS_MAX_PARITY];
    for (int i = 0; i < npar; i++) {
        uint8_t v = 0;
        for (int j = 0; j <= i && j <= L; j++) v = (uint8_t)(v ^ gf_mul(lam[j], synd[i - j]));
        omega[i] = v;
    }
    for (int k = 0; k < found; k++) {
        int p = pos[k];
        uint8_t x = gf_pow_alpha(p);
        uint8_t xinv = gf_pow_alpha(-p);
        uint8_t num = 0;
        for (int i = npar - 1; i >= 0; i--) num = (uint8_t)(gf_mul(num, xinv) ^ omega[i]);
        // 形式导数:特征 2 下偶次项消失,Λ'(x) = Σ_{i 奇} Λ_i x^(i-1)。
        uint8_t den = 0;
        uint8_t xinv2 = gf_mul(xinv, xinv);
        uint8_t pw = 1;
        for (int i = 1; i <= L; i += 2) {
            den = (uint8_t)(den ^ gf_mul(lam[i], pw));
            pw = gf_mul(pw, xinv2);
        }
        if (den == 0) return -1;
        msg[n - 1 - p] = (uint8_t)(msg[n - 1 - p] ^ gf_mul(x, gf_div(num, den)));
    }

    // 5) 复核:纠正后伴随式必须全零,否则是越界"误纠"。
    for (int i = 0; i < npar; i++) {
        uint8_t s = 0;
        for (int j = 0; j < n; j++) s = (uint8_t)(gf_mul(s, s_exp[i]) ^ msg[j]);
        if (s) return -1;
    }
    return found;
}
