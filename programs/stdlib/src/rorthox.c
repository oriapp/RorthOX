#include "rorthox.h"
#include "string.h"

struct command_argument *rorthox_parse_command(const char *command, int max)
{
    struct command_argument *root_command = 0;
    char scommand[1025];
    if (max >= (int) sizeof(scommand))
    {
        return 0;
    }

    strncpy(scommand, command, sizeof(scommand));
    char *token = strtok(scommand, " ");
    if (!token)
    {
        goto out;
    }

    root_command = rorthox_malloc(sizeof(struct command_argument));
    if (!root_command)
    {
        goto out;
    }

    strncpy(root_command->argument, token, sizeof(root_command->argument));
    root_command->next = 0;

    struct command_argument *current = root_command;
    token = strtok(NULL, " ");
    while (token != 0)
    {
        struct command_argument *new_command = rorthox_malloc(sizeof(struct command_argument));
        if (!new_command)
        {
            break; // or goto out?
        }

        strncpy(new_command->argument, token, sizeof(new_command->argument));
        new_command->next = 0x00;
        current->next = new_command;
        current = new_command;
        token = strtok(NULL, " "); // next token
    }

out:
    return root_command;
}

int rorthox_getkeyblock()
{
    int val = 0;
    do
    {
        val = rorthox_getkey();
    } while (val == 0);
    return val;
}

void rorthox_terminal_readline(char *out, int max, bool output_while_typing)
{
    if (!out || max <= 0)
    {
        return;
    }

    int length = 0;
    int position = 0;
    int capacity = max - 1;
    if (output_while_typing && capacity > RORTHOX_READLINE_ECHO_LIMIT)
    {
        capacity = RORTHOX_READLINE_ECHO_LIMIT;
    }
    out[0] = 0;

    while (length < capacity)
    {
        int key = rorthox_getkeyblock();
        if (key == 13)
        {
            break;
        }

        int next_position = position;
        switch (key)
        {
        case RORTHOX_KEY_LEFT:
            if (position > 0)
                next_position--;
            break;
        case RORTHOX_KEY_RIGHT:
            if (position < length)
                next_position++;
            break;
        case RORTHOX_KEY_HOME:
            next_position = 0;
            break;
        case RORTHOX_KEY_END:
            next_position = length;
            break;
        }
        if (next_position != position)
        {
            if (output_while_typing)
            {
                rorthox_terminal_move_cursor(next_position - position);
            }
            position = next_position;
        }

        if (key == 0x08 || key == RORTHOX_KEY_DELETE)
        {
            if (key == 0x08)
            {
                if (position == 0)
                    continue;
                if (output_while_typing)
                    rorthox_terminal_move_cursor(-1);
                position--;
            }
            if (position == length)
                continue;

            for (int i = position; i < length; i++)
            {
                out[i] = out[i + 1];
            }
            length--;
            if (output_while_typing)
            {
                for (int i = position; i < length; i++)
                    rorthox_putchar(out[i]);
                rorthox_putchar(' ');
                rorthox_terminal_move_cursor(position - length - 1);
            }
            continue;
        }

        // LF is not a single-cell key; unsupported special keys are not text.
        if (key <= 0 || key > 255 || key == '\n')
        {
            continue;
        }
        for (int i = length; i > position; i--)
        {
            out[i] = out[i - 1];
        }
        out[position] = (char)key;
        length++;
        out[length] = 0;
        if (output_while_typing)
        {
            for (int i = position; i < length; i++)
                rorthox_putchar(out[i]);
        }
        position++;
        if (output_while_typing && position < length)
        {
            rorthox_terminal_move_cursor(position - length);
        }
    }

    if (output_while_typing && position < length)
    {
        rorthox_terminal_move_cursor(length - position);
    }
}

int rorthox_system_run(const char *command)
{
    char buf[1024];
    strncpy(buf, command, sizeof(buf));
    struct command_argument *root_command_argument = rorthox_parse_command(buf, sizeof(buf));
    if (!root_command_argument)
    {
        return -1;
    }

    return rorthox_system(root_command_argument);
}