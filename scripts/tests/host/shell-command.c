/*
 * Host check for scripts/tests/shell-command.sh: the real rorthox_parse_command() and
 * rorthox_system_run() (programs/stdlib/src/rorthox.c), built with the host C compiler. The
 * script builds rorthox.c and string.c on their own and puts "user_" in front of every symbol
 * (objcopy --prefix-symbols), so nothing clashes with the host C library.
 *
 * The allocation syscalls are stand-ins: user_rorthox_malloc hands out slots of a fixed arena and
 * can fail at a chosen call, user_rorthox_free marks a slot free and fills it with a pattern (a
 * free of anything else, or of a slot twice, is a bad free). user_rorthox_system records the
 * words of the list it gets, checks that every node is still allocated and returns a chosen
 * result.
 *
 * Words are separated by ASCII spaces only, and a command has 1 to RORTHOX_MAX_COMMAND_ARGUMENTS
 * words (the program name included):
 *  - parse: no words, 1 word, MAX-1, MAX, MAX+1 and many more words, repeated, leading and
 *    trailing spaces, a tab inside a word (also in a MAX and a MAX+1 word command): the list
 *    holds exactly the words, or there is no list and nothing stays allocated; a command above
 *    the limit makes at most MAX nodes on the way.
 *  - parse with allocation failure at each node of a 1, 2, 3 and MAX word command: no list,
 *    nothing stays allocated, no bad free (no prefix of the command survives).
 *  - repeated parses, also after a refused command: the same list each time.
 *  - system_run: the syscall gets the whole list once, the list is freed after it returns and
 *    its result is returned; no words, too many words or an allocation failure at any node
 *    never reach the syscall, leave nothing allocated and return a negative value. Many runs in
 *    a row leave nothing allocated.
 * Each case runs in its own process, so a crash fails only that case.
 *
 * Prints "shell-command host check: <failed> of <cases> cases failed".
 */
#include "host.h"
#include "rorthox-config.h"

struct command_argument
{
    char argument[RORTHOX_COMMAND_ARGUMENT_SIZE];
    struct command_argument *next;
};

struct command_argument *user_rorthox_parse_command(const char *command, int max);
int user_rorthox_system_run(const char *command);

int strcmp(const char *a, const char *b);
void *memset(void *s, int c, unsigned long n);
unsigned long strlen(const char *s);
char *strcpy(char *dest, const char *src);

#define MAX RORTHOX_MAX_COMMAND_ARGUMENTS
#define SLOTS 1024
#define SLOT 1024
#define POISON 0xDD
#define NOT_REACHED 10

enum
{
    UNUSED,
    LIVE,
    FREED,
};

static unsigned char arena[SLOTS][SLOT] __attribute__((aligned(16)));
static int slot_state[SLOTS];
static int next_slot;
static int malloc_calls;
static int fail_at;
static int failed_once;
static int bad_frees;
static int bad_sizes;

static int system_calls;
static int system_result;
static int system_dead_nodes;
static int system_count;
static char system_words[MAX + 8][64];

void *user_rorthox_malloc(unsigned long size)
{
    malloc_calls++;
    if (fail_at && malloc_calls == fail_at)
    {
        failed_once = 1;
        return 0;
    }
    if (size != sizeof(struct command_argument))
    {
        bad_sizes++;
    }
    if (size > SLOT || next_slot >= SLOTS)
    {
        return 0;
    }
    slot_state[next_slot] = LIVE;
    memset(arena[next_slot], 0xAA, SLOT);
    return arena[next_slot++];
}

void *user_rorthox_free(void *ptr)
{
    if (!ptr)
    {
        return 0;
    }
    unsigned long offset = (unsigned char *)ptr - &arena[0][0];
    if ((unsigned char *)ptr < &arena[0][0] || offset >= sizeof(arena) || offset % SLOT ||
        slot_state[offset / SLOT] != LIVE)
    {
        bad_frees++;
        return 0;
    }
    slot_state[offset / SLOT] = FREED;
    memset(ptr, POISON, SLOT);
    return 0;
}

static int is_live(const void *ptr)
{
    unsigned long offset = (const unsigned char *)ptr - &arena[0][0];
    return (const unsigned char *)ptr >= &arena[0][0] && offset < sizeof(arena) && offset % SLOT == 0 &&
           slot_state[offset / SLOT] == LIVE;
}

static int live_count(void)
{
    int n = 0;
    for (int i = 0; i < next_slot; i++)
    {
        n += slot_state[i] == LIVE;
    }
    return n;
}

int user_rorthox_system(struct command_argument *arguments)
{
    system_calls++;
    system_count = 0;
    for (struct command_argument *node = arguments; node && system_count < MAX + 8; node = node->next)
    {
        if (!is_live(node))
        {
            system_dead_nodes++;
            break;
        }
        int k = 0;
        for (; node->argument[k] && k < (int)sizeof(system_words[0]) - 1; k++)
        {
            system_words[system_count][k] = node->argument[k];
        }
        system_words[system_count][k] = 0;
        system_count++;
    }
    return system_result;
}

// Not used by the parser; rorthox.c needs them to link.
int user_rorthox_getkey()
{
    return 13;
}

void user_rorthox_putchar(char c)
{
}

int user_rorthox_terminal_move_cursor(int delta)
{
    return 0;
}

static void reset(void)
{
    memset(slot_state, 0, sizeof(slot_state));
    next_slot = 0;
    malloc_calls = 0;
    fail_at = 0;
    failed_once = 0;
    bad_frees = 0;
    bad_sizes = 0;
    system_calls = 0;
    system_result = 0;
    system_dead_nodes = 0;
    system_count = 0;
}

// The model: the words of <command>, split at ASCII spaces only.
static int model_words(const char *command, char words[][64], int room)
{
    int n = 0;
    const char *p = command;
    while (*p)
    {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        int k = 0;
        while (*p && *p != ' ')
        {
            if (n < room && k < 63)
                words[n][k++] = *p;
            p++;
        }
        if (n < room)
            words[n][k] = 0;
        n++;
    }
    return n;
}

// "blank.elf" and then <n>-1 more words "wK", joined by <gap> spaces, with <edge> spaces before and after.
static void make_command(char *out, int n, int gap, int edge)
{
    char *p = out;
    for (int i = 0; i < edge; i++)
        *p++ = ' ';
    strcpy(p, "blank.elf");
    p += strlen(p);
    for (int i = 2; i <= n; i++)
    {
        for (int g = 0; g < gap; g++)
            *p++ = ' ';
        *p++ = 'w';
        if (i >= 100)
            *p++ = '0' + i / 100;
        if (i >= 10)
            *p++ = '0' + i / 10 % 10;
        *p++ = '0' + i % 10;
    }
    for (int i = 0; i < edge; i++)
        *p++ = ' ';
    *p = 0;
}

// Like make_command(<n>, 1, 0), with a tab inside the second word: "blank.elf w2\tt w3 ...". The
// Tab key types '\t', and it is not a word separator.
static void make_tab_command(char *out, int n)
{
    char plain[1100];
    make_command(plain, n, 1, 0);
    const char *p = plain;
    char *q = out;
    while (*p && *p != ' ')
        *q++ = *p++;
    *q++ = *p++;
    while (*p && *p != ' ')
        *q++ = *p++;
    *q++ = '\t';
    *q++ = 't';
    while (*p)
        *q++ = *p++;
    *q = 0;
}

static char why[200];

static int fail(const char *what)
{
    int k = 0;
    for (; what[k] && k < (int)sizeof(why) - 1; k++)
        why[k] = what[k];
    why[k] = 0;
    return 1;
}

// Checks a parsed list against the model and frees it as its caller would.
static int check_list(const char *command, struct command_argument *list)
{
    char words[MAX + 8][64];
    int n = model_words(command, words, MAX + 8);
    if (n == 0 || n > MAX)
    {
        if (list)
            return fail(n ? "a list for a command above the word limit" : "a list for a command without words");
        if (live_count())
            return fail("nodes stay allocated after the command was refused");
        return 0;
    }
    if (!list)
        return fail("no list for a command within the word limit");
    int count = 0;
    struct command_argument *node = list;
    for (; node && count < MAX + 8; node = node->next, count++)
    {
        if (!is_live(node))
            return fail("a list node is not an allocated node");
        if (count < n && strcmp(node->argument, words[count]) != 0)
            return fail("a word differs from the command's word");
    }
    if (count != n)
        return fail("the list does not have one node per word");
    if (live_count() != n)
        return fail("more nodes allocated than the list holds");
    for (node = list; node;)
    {
        struct command_argument *next = node->next;
        user_rorthox_free(node);
        node = next;
    }
    return 0;
}

static int checks_after(void)
{
    if (bad_frees)
        return fail("a bad free (not an allocated node, or freed twice)");
    if (bad_sizes)
        return fail("an allocation of another size than one node");
    return 0;
}

static int parse_case(const char *command)
{
    reset();
    char copy[1100];
    strcpy(copy, command);
    struct command_argument *list = user_rorthox_parse_command(copy, 1024);
    char words[MAX + 8][64];
    int n = model_words(command, words, MAX + 8);
    if (n > MAX && malloc_calls > MAX)
        return fail("more than MAX nodes allocated for a command above the limit");
    return check_list(command, list) || checks_after();
}

// Allocation <at> fails while parsing <command>: no list and nothing left, or NOT_REACHED.
static int parse_fail_case(const char *command, int at)
{
    reset();
    fail_at = at;
    char copy[1100];
    strcpy(copy, command);
    struct command_argument *list = user_rorthox_parse_command(copy, 1024);
    if (!failed_once)
        return NOT_REACHED;
    if (list)
        return fail("a list after an allocation failed (a prefix of the command)");
    if (live_count())
        return fail("nodes stay allocated after an allocation failed");
    return checks_after();
}

static int parse_repeat_case(void)
{
    char big[1100];
    char ok[1100];
    reset();
    make_command(big, MAX + 1, 1, 0);
    make_command(ok, 3, 2, 1);
    for (int i = 0; i < 40; i++)
    {
        const char *command = i % 4 == 1 ? big : i % 4 == 3 ? "   " : ok;
        char copy[1100];
        strcpy(copy, command);
        int before = live_count();
        struct command_argument *list = user_rorthox_parse_command(copy, 1024);
        if (before != 0)
            return fail("nodes left from an earlier parse");
        if (check_list(command, list))
            return 1;
    }
    return checks_after();
}

static int run_case(const char *command, int result)
{
    reset();
    system_result = result;
    char words[MAX + 8][64];
    int n = model_words(command, words, MAX + 8);
    int ret = user_rorthox_system_run(command);
    if (n == 0 || n > MAX)
    {
        if (system_calls)
            return fail(n ? "the syscall ran a command above the word limit" : "the syscall ran a command without words");
        if (ret >= 0)
            return fail("no negative result for a refused command");
    }
    else
    {
        if (system_calls != 1)
            return fail("the syscall did not run exactly once");
        if (system_dead_nodes)
            return fail("the syscall got a freed node");
        if (system_count != n)
            return fail("the syscall did not get one node per word");
        for (int i = 0; i < n; i++)
        {
            if (strcmp(system_words[i], words[i]) != 0)
                return fail("the syscall got another word");
        }
        if (ret != result)
            return fail("the syscall's result is not returned");
    }
    if (live_count())
        return fail("nodes stay allocated after system_run");
    return checks_after();
}

static int run_fail_case(const char *command, int at)
{
    reset();
    fail_at = at;
    int ret = user_rorthox_system_run(command);
    if (!failed_once)
        return NOT_REACHED;
    if (system_calls)
        return fail("the syscall ran after an allocation failed (a prefix of the command)");
    if (ret >= 0)
        return fail("no negative result after an allocation failed");
    if (live_count())
        return fail("nodes stay allocated after an allocation failed");
    return checks_after();
}

static int run_repeat_case(void)
{
    char big[1100];
    char ok[1100];
    char full[1100];
    reset();
    make_command(big, MAX + 1, 1, 0);
    make_command(ok, 2, 1, 0);
    make_command(full, MAX, 3, 2);
    int expected_calls = 0;
    for (int i = 0; i < 60; i++)
    {
        const char *command = i % 4 == 0 ? ok : i % 4 == 1 ? big : i % 4 == 2 ? "" : full;
        char unused[1][64];
        int n = model_words(command, unused, 0);
        system_result = i;
        int ret = user_rorthox_system_run(command);
        if (n >= 1 && n <= MAX)
        {
            expected_calls++;
            if (ret != i)
                return fail("a repeated run did not return the syscall's result");
        }
        else if (ret >= 0)
            return fail("a repeated refused run did not return a negative value");
        if (system_calls != expected_calls)
            return fail("repeated runs reached the syscall a wrong number of times");
        if (live_count())
            return fail("nodes stay allocated after a repeated run");
    }
    return checks_after();
}

static int cases;
static int failed;
static char current[120];

// Runs one case in its own process; returns its result (0, 1 or NOT_REACHED).
static int run(int (*body)(void *), void *arg, const char *name)
{
    fflush(0);
    pid_t pid = fork();
    if (pid < 0)
    {
        perror("fork");
        exit(2);
    }
    if (pid == 0)
    {
        int rc = body(arg);
        if (rc == 1)
            printf("  FAIL %s: %s\n", name, why);
        fflush(0);
        _exit(rc);
    }
    int status;
    waitpid(pid, &status, 0);
    int rc;
    if (WIFEXITED(status))
        rc = WEXITSTATUS(status);
    else
    {
        printf("  FAIL %s: crashed (signal %d)\n", name, WTERMSIG(status));
        rc = 1;
    }
    if (rc != NOT_REACHED)
    {
        cases++;
        failed += rc != 0;
    }
    return rc;
}

struct job
{
    const char *command;
    int n;
    int result;
};

static int parse_body(void *a)
{
    return parse_case(((struct job *)a)->command);
}
static int parse_fail_body(void *a)
{
    return parse_fail_case(((struct job *)a)->command, ((struct job *)a)->n);
}
static int run_body(void *a)
{
    return run_case(((struct job *)a)->command, ((struct job *)a)->result);
}
static int run_fail_body(void *a)
{
    return run_fail_case(((struct job *)a)->command, ((struct job *)a)->n);
}
static int parse_repeat_body(void *a)
{
    return parse_repeat_case();
}
static int run_repeat_body(void *a)
{
    return run_repeat_case();
}

static void label(const char *kind, const char *command, int n)
{
    int k = 0;
    for (const char *p = kind; *p && k < 40; p++)
        current[k++] = *p;
    current[k++] = ' ';
    current[k++] = '"';
    for (const char *p = command; *p && k < 100; p++)
        current[k++] = *p == '\t' ? '~' : *p;
    if (strlen(command) > 100 - 43)
    {
        current[k++] = '.';
        current[k++] = '.';
    }
    current[k++] = '"';
    if (n)
    {
        current[k++] = ' ';
        current[k++] = '@';
        if (n >= 10)
            current[k++] = '0' + n / 10 % 10;
        current[k++] = '0' + n % 10;
    }
    current[k] = 0;
}

int main(void)
{
    static char commands[16][1100];
    const char *fixed[] = {
        "", " ", "     ", "blank.elf", "  blank.elf  ", "blank.elf a b", "blank.elf   a  b   ", "a\tb c",
    };
    int nfixed = sizeof(fixed) / sizeof(fixed[0]);
    int c = 0;
    make_command(commands[c++], MAX - 1, 1, 0);
    make_command(commands[c++], MAX, 1, 0);
    make_command(commands[c++], MAX + 1, 1, 0);
    make_command(commands[c++], MAX + 2, 1, 0);
    make_command(commands[c++], MAX, 3, 4);
    make_command(commands[c++], MAX + 1, 3, 4);
    make_command(commands[c++], 200, 1, 0);
    make_tab_command(commands[c++], MAX);
    make_tab_command(commands[c++], MAX + 1);
    int ncommands = c;

    struct job job;
    for (int pass = 0; pass < 2; pass++)
    {
        int (*body)(void *) = pass ? run_body : parse_body;
        for (int i = 0; i < nfixed + ncommands; i++)
        {
            job.command = i < nfixed ? fixed[i] : commands[i - nfixed];
            job.result = pass ? 7 - i : 0;
            label(pass ? "system_run" : "parse", job.command, 0);
            run(body, &job, current);
        }
    }

    // Allocation failure at each node: 1, 2, 3 and MAX words.
    char fail_commands[4][1100];
    make_command(fail_commands[0], 1, 1, 0);
    make_command(fail_commands[1], 2, 1, 0);
    make_command(fail_commands[2], 3, 2, 1);
    make_command(fail_commands[3], MAX, 1, 0);
    int reached = 0;
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < 4; i++)
        {
            job.command = fail_commands[i];
            for (int at = 1; at <= MAX + 2; at++)
            {
                job.n = at;
                label(pass ? "system_run, allocation fails" : "parse, allocation fails", job.command, at);
                if (run(pass ? run_fail_body : parse_fail_body, &job, current) == NOT_REACHED)
                    break;
                reached++;
            }
        }
    }
    // Every node's allocation must have been reached: (1 + 2 + 3 + MAX) for parse and for system_run.
    cases++;
    if (reached < 2 * (1 + 2 + 3 + MAX))
    {
        printf("  FAIL allocation failures: only %d of %d node allocations reached\n", reached, 2 * (6 + MAX));
        failed++;
    }

    run(parse_repeat_body, 0, "parse, 40 calls in a row");
    run(run_repeat_body, 0, "system_run, 60 runs in a row");

    printf("shell-command host check: %d of %d cases failed\n", failed, cases);
    return failed ? 1 : 0;
}
