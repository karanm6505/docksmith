#pragma once

typedef struct {
    int  line;
    char command[32];
    char args[4096];
} Instruction;

typedef struct {
    Instruction *items;
    int          count;
} InstructionList;

void instruction_list_free(InstructionList *il);

/* Parse: parse the Docksmithfile at path.
   Returns a heap-allocated InstructionList. items=NULL on error (message printed). */
InstructionList parse_docksmithfile(const char *path);
