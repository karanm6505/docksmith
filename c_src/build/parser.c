#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *VALID_CMDS[] = {
    "FROM", "COPY", "RUN", "WORKDIR", "ENV", "CMD", NULL
};

static int is_valid_cmd(const char *cmd) {
    for (int i = 0; VALID_CMDS[i]; i++)
        if (strcmp(cmd, VALID_CMDS[i]) == 0) return 1;
    return 0;
}

static void str_toupper(char *s) {
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static char *str_trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

void instruction_list_free(InstructionList *il) {
    free(il->items);
    il->items = NULL;
    il->count = 0;
}

InstructionList parse_docksmithfile(const char *path) {
    InstructionList result = {0};

    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return result; }

    int cap = 0;
    char line_buf[8192];
    int line_num = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        line_num++;
        char *line = str_trim(line_buf);

        /* skip blank lines and comments */
        if (line[0] == '\0' || line[0] == '#') continue;

        /* handle line continuations */
        char full_line[8192];
        snprintf(full_line, sizeof(full_line), "%s", line);
        while (strlen(full_line) > 0 &&
               full_line[strlen(full_line)-1] == '\\') {
            full_line[strlen(full_line)-1] = ' ';
            if (!fgets(line_buf, sizeof(line_buf), f)) break;
            line_num++;
            char *cont = str_trim(line_buf);
            strncat(full_line, cont, sizeof(full_line) - strlen(full_line) - 1);
        }

        /* split into command and args */
        char *space = strchr(full_line, ' ');
        char cmd_buf[64] = {0};
        char args_buf[4096] = {0};
        if (space) {
            size_t clen = (size_t)(space - full_line);
            if (clen >= sizeof(cmd_buf)) clen = sizeof(cmd_buf) - 1;
            memcpy(cmd_buf, full_line, clen);
            char *args_start = str_trim(space + 1);
            snprintf(args_buf, sizeof(args_buf), "%s", args_start);
        } else {
            snprintf(cmd_buf, sizeof(cmd_buf), "%s", full_line);
        }
        str_toupper(cmd_buf);

        if (!is_valid_cmd(cmd_buf)) {
            fprintf(stderr, "line %d: unrecognised instruction %s\n", line_num, cmd_buf);
            fclose(f);
            free(result.items);
            result.items = NULL; result.count = 0;
            return result;
        }

        if (strcmp(cmd_buf, "FROM") == 0 && result.count > 0) {
            fprintf(stderr, "line %d: FROM must be the first instruction\n", line_num);
            fclose(f);
            free(result.items);
            result.items = NULL; result.count = 0;
            return result;
        }

        /* grow the list */
        if (result.count >= cap) {
            cap = cap ? cap * 2 : 8;
            result.items = realloc(result.items, cap * sizeof(Instruction));
        }

        result.items[result.count].line = line_num;
        snprintf(result.items[result.count].command,
                 sizeof(result.items[0].command), "%s", cmd_buf);
        snprintf(result.items[result.count].args,
                 sizeof(result.items[0].args), "%s", args_buf);
        result.count++;
    }
    fclose(f);

    if (result.count == 0) {
        fprintf(stderr, "Docksmithfile is empty\n");
        return result;
    }
    if (strcmp(result.items[0].command, "FROM") != 0) {
        fprintf(stderr, "line %d: first instruction must be FROM, got %s\n",
                result.items[0].line, result.items[0].command);
        free(result.items); result.items = NULL; result.count = 0;
    }
    return result;
}
