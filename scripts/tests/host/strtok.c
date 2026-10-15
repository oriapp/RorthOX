/*
 * Host check for scripts/tests/strtok.sh: the real user strtok (programs/stdlib/src/string.c),
 * built with the host C compiler. The script builds string.c on its own and puts "user_" in
 * front of every symbol (objcopy --prefix-symbols), so user_strtok cannot clash with the host C
 * library's strtok.
 *
 * strtok must start over when it gets a new string, even if the last string isn't finished
 * (K25: it kept going through the old string).
 *  1. Fixed steps: a new string after one token of "old tail", after a one-token string, after
 *     an empty string; an empty string while another one isn't finished; leading, repeated and
 *     trailing delimiters; two delimiter characters; calls after the end.
 *  2. Many random calls on random strings (new string or NULL, random delimiters), compared with
 *     a small model of strtok: same token start (which string, which offset) and the same bytes
 *     in every string after every call.
 *
 * Prints "strtok host check: <failed> of <checks> checks failed".
 */
#include "host.h"

char *user_strtok(char *str, const char *delimiters);

int strcmp(const char *a, const char *b);
char *strcpy(char *dest, const char *src);
int memcmp(const void *a, const void *b, unsigned long n);
void *memset(void *s, int c, unsigned long n);

static int checks;
static int failed;

static void expect(int ok, const char *what, int step, long got, long want)
{
    checks++;
    if (!ok)
    {
        failed++;
        if (failed <= 20)
        {
            printf("  FAIL %s %d: got %ld, expected %ld\n", what, step, got, want);
        }
    }
}

// One call: the token must start at want (0: no token) and read want_text.
static void token(int step, char *got, char *want, const char *want_text)
{
    int ok = got == want && (!want || strcmp(got, want_text) == 0);
    // got: the token's distance from want, -1 for no token, 1000 for a token where none was expected
    expect(ok, "step", step, !got ? -1 : want ? (long)(got - want) : 1000, 0);
}

static void fixed_steps(void)
{
    char old[] = "old tail";
    char words[] = "new words";
    char single[] = "single";
    char next[] = "next one";
    char empty[] = "";
    char after_empty[] = "x y";
    char unfinished[] = "aa bb";
    char spaced[] = "  lead  and   repeat ";
    char mixed[] = ",a,;b;;c;";
    char again[] = "again";
    char only_delims[] = " ,; ";
    char last[] = "z";

    token(1, user_strtok(old, " "), old, "old");
    token(2, user_strtok(words, " "), words, "new");
    token(3, user_strtok(0, " "), words + 4, "words");
    token(4, user_strtok(0, " "), 0, 0);
    token(5, user_strtok(single, " "), single, "single");
    token(6, user_strtok(next, " "), next, "next");
    token(7, user_strtok(0, " "), next + 5, "one");
    token(8, user_strtok(0, " "), 0, 0);
    token(9, user_strtok(empty, " "), 0, 0);
    token(10, user_strtok(0, " "), 0, 0);
    token(11, user_strtok(after_empty, " "), after_empty, "x");
    token(12, user_strtok(0, " "), after_empty + 2, "y");
    token(13, user_strtok(0, " "), 0, 0);
    token(14, user_strtok(unfinished, " "), unfinished, "aa");
    token(15, user_strtok(empty, " "), 0, 0);
    token(16, user_strtok(0, " "), 0, 0);
    token(17, user_strtok(spaced, " "), spaced + 2, "lead");
    token(18, user_strtok(0, " "), spaced + 8, "and");
    token(19, user_strtok(0, " "), spaced + 14, "repeat");
    token(20, user_strtok(0, " "), 0, 0);
    token(21, user_strtok(mixed, ",;"), mixed + 1, "a");
    token(22, user_strtok(0, ",;"), mixed + 4, "b");
    token(23, user_strtok(0, ",;"), mixed + 7, "c");
    token(24, user_strtok(0, ",;"), 0, 0);
    token(25, user_strtok(0, ",;"), 0, 0);
    token(26, user_strtok(again, " "), again, "again");
    token(27, user_strtok(0, " "), 0, 0);
    // Only delimiters while "unfinished" has a token left, then a one-character string.
    strcpy(unfinished, "aa bb");
    token(28, user_strtok(unfinished, " "), unfinished, "aa");
    token(29, user_strtok(only_delims, " ,;"), 0, 0);
    token(30, user_strtok(0, " "), 0, 0);
    token(31, user_strtok(last, " "), last, "z");
    token(32, user_strtok(0, " "), 0, 0);
    // The bytes the tokens left behind: every delimiter that ended a token is now 0.
    expect(memcmp(words, "new\0words", 10) == 0, "bytes of \"new words\"", 0, 0, 0);
    expect(memcmp(spaced, "  lead\0 and\0  repeat\0", 22) == 0, "bytes of the spaced string", 0, 0, 0);
    expect(memcmp(mixed, ",a\0;b\0;c\0", 10) == 0, "bytes of the mixed string", 0, 0, 0);
    printf("fixed steps: %d checks\n", checks);
}

// The model: standard strtok.
static char *model_state;

static int is_delimiter(char c, const char *delimiters)
{
    for (; *delimiters; delimiters++)
    {
        if (c == *delimiters)
        {
            return 1;
        }
    }
    return 0;
}

static char *model_strtok(char *str, const char *delimiters)
{
    if (str)
    {
        model_state = str;
    }
    if (!model_state)
    {
        return 0;
    }
    char *p = model_state;
    while (*p && is_delimiter(*p, delimiters))
    {
        p++;
    }
    if (!*p)
    {
        model_state = 0;
        return 0;
    }
    char *start = p;
    while (*p && !is_delimiter(*p, delimiters))
    {
        p++;
    }
    if (*p)
    {
        *p = 0;
        model_state = p + 1;
    }
    else
    {
        model_state = p;
    }
    return start;
}

#define SLOTS 64
#define SLOT_SIZE 16
static char user_slots[SLOTS][SLOT_SIZE];
static char model_slots[SLOTS][SLOT_SIZE];

static unsigned int seed = 12345;

static unsigned int next_random(void)
{
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) & 0x7fff;
}

// Where a token starts: slot * SLOT_SIZE + offset, -1 for none, -2 for outside every slot.
static long where(char *p, char slots[SLOTS][SLOT_SIZE])
{
    if (!p)
    {
        return -1;
    }
    if (p < &slots[0][0] || p >= &slots[SLOTS - 1][SLOT_SIZE])
    {
        return -2;
    }
    return p - &slots[0][0];
}

static void random_calls(void)
{
    static const char *const delimiter_sets[] = {" ", ",;", " ,", ";", " ,;"};
    static const char alphabet[] = "ab ,;";
    int before = checks;
    int calls = 0;

    for (int run = 0; run < 400; run++)
    {
        memset(user_slots, 0, sizeof(user_slots));
        memset(model_slots, 0, sizeof(model_slots));
        int used = 0;
        for (int step = 0; step < 48; step++)
        {
            const char *delimiters = delimiter_sets[next_random() % 5];
            char *user_str = 0;
            char *model_str = 0;
            if (step == 0 || (next_random() % 3 == 0 && used < SLOTS))
            {
                int len = next_random() % (SLOT_SIZE - 1);
                for (int i = 0; i < len; i++)
                {
                    user_slots[used][i] = alphabet[next_random() % 5];
                }
                user_slots[used][len] = 0;
                strcpy(model_slots[used], user_slots[used]);
                user_str = user_slots[used];
                model_str = model_slots[used];
                used++;
            }
            long got = where(user_strtok(user_str, delimiters), user_slots);
            long want = where(model_strtok(model_str, delimiters), model_slots);
            calls++;
            expect(got == want, "random call (token start)", calls, got, want);
            expect(memcmp(user_slots, model_slots, sizeof(user_slots)) == 0, "random call (string bytes)", calls, 0,
                   0);
        }
    }
    printf("random calls: %d calls, %d checks\n", calls, checks - before);
}

int main(void)
{
    fixed_steps();
    random_calls();
    printf("strtok host check: %d of %d checks failed\n", failed, checks);
    fflush(0);
    return failed ? 1 : 0;
}
