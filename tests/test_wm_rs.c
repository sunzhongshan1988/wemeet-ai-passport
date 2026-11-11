// tests/test_wm_rs.c —— Reed-Solomon 编解码的 host 测试:随机数据 + 随机错误。
#include "wm_rs.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

// 固定种子的 xorshift,保证失败可复现。
static unsigned s_seed = 0x57454D54u;
static unsigned rnd(void) {
    s_seed ^= s_seed << 13;
    s_seed ^= s_seed >> 17;
    s_seed ^= s_seed << 5;
    return s_seed;
}

// 在 msg 里制造恰好 count 个互不相同位置的错误。
static void corrupt(unsigned char *msg, int n, int count) {
    unsigned char hit[255] = {0};
    while (count > 0) {
        int p = (int)(rnd() % (unsigned)n);
        if (hit[p]) continue;
        hit[p] = 1;
        msg[p] ^= (unsigned char)(1 + rnd() % 255);
        count--;
    }
}

int main(void) {
    static const int npars[] = {4, 8, 16, 32, 60};
    for (unsigned t = 0; t < 3000; t++) {
        int npar = npars[rnd() % 5];
        int len = 1 + (int)(rnd() % (unsigned)(255 - npar));
        unsigned char data[255], msg[255];
        for (int i = 0; i < len; i++) data[i] = (unsigned char)rnd();
        memcpy(msg, data, (size_t)len);
        wm_rs_encode(data, len, npar, msg + len);
        int n = len + npar;

        // 无错:返回 0 且数据不变。
        assert(wm_rs_decode(msg, n, npar) == 0);
        assert(memcmp(msg, data, (size_t)len) == 0);

        // 纠错能力以内(含上限 npar/2):必须完全恢复,并报告正确的纠正个数。
        int errors = (int)(rnd() % (unsigned)(npar / 2 + 1));
        if (errors > n) errors = n;
        corrupt(msg, n, errors);
        assert(wm_rs_decode(msg, n, npar) == errors);
        assert(memcmp(msg, data, (size_t)len) == 0);

        // 超出能力:要么报失败,要么(极小概率)落到另一个合法码字——但绝不能声称
        // 恢复了原始数据却给出别的内容之外的"部分正确"。这里只要求不崩溃、不越界。
        memcpy(msg, data, (size_t)len);
        wm_rs_encode(data, len, npar, msg + len);
        int too_many = npar / 2 + 1 + (int)(rnd() % 4);
        if (too_many <= n) {
            corrupt(msg, n, too_many);
            (void)wm_rs_decode(msg, n, npar);
        }
    }
    // 参数越界必须拒绝。
    unsigned char tiny[8] = {0};
    assert(wm_rs_decode(tiny, 4, 4) == -1);
    assert(wm_rs_decode(tiny, 8, 1) == -1);
    puts("test_wm_rs: PASS");
    return 0;
}
