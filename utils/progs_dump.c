/*
 * Simple progs.dat analyzer - dump builtins used
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint32_t version;
    uint32_t crc;
    uint32_t ofs_statements;
    uint32_t numstatements;
    uint32_t ofs_globaldefs;
    uint32_t numglobaldefs;
    uint32_t ofs_fielddefs;
    uint32_t numfielddefs;
    uint32_t ofs_functions;
    uint32_t numfunctions;
    uint32_t ofs_strings;
    uint32_t numstrings;
    uint32_t ofs_globals;
    uint32_t numglobals;
    uint32_t entityfields;
} dprograms_t;

typedef struct {
    int32_t first_statement;  // negative numbers are builtins
    int32_t parm_start;
    int32_t locals;
    int32_t profile;
    int32_t s_name;
    int32_t s_file;
    int32_t numparms;
    uint8_t parm_size[8];
} dfunction_t;

int main(int argc, char *argv[]) {
    FILE *f;
    dprograms_t header;
    dfunction_t *funcs;
    char *strings;
    int i;
    int max_builtin = 0;
    
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <progs.dat>\n", argv[0]);
        return 1;
    }
    
    f = fopen(argv[1], "rb");
    if (!f) {
        perror("fopen");
        return 1;
    }
    
    if (fread(&header, sizeof(header), 1, f) != 1) {
        perror("fread header");
        fclose(f);
        return 1;
    }
    
    printf("progs.dat analysis for: %s\n", argv[1]);
    printf("Version: %u\n", header.version);
    printf("CRC: %u\n", header.crc);
    printf("Functions: %u\n", header.numfunctions);
    printf("Strings: %u (at offset %u)\n", header.numstrings, header.ofs_strings);
    printf("\n");
    
    /* Read string table */
    strings = malloc(header.numstrings);
    if (!strings) {
        fprintf(stderr, "Failed to allocate string table\n");
        fclose(f);
        return 1;
    }
    fseek(f, header.ofs_strings, SEEK_SET);
    if (fread(strings, header.numstrings, 1, f) != 1) {
        perror("fread strings");
        free(strings);
        fclose(f);
        return 1;
    }
    
    /* Read functions */
    funcs = malloc(sizeof(dfunction_t) * header.numfunctions);
    if (!funcs) {
        fprintf(stderr, "Failed to allocate functions\n");
        free(strings);
        fclose(f);
        return 1;
    }
    fseek(f, header.ofs_functions, SEEK_SET);
    if (fread(funcs, sizeof(dfunction_t), header.numfunctions, f) != (size_t)header.numfunctions) {
        perror("fread functions");
        free(funcs);
        free(strings);
        fclose(f);
        return 1;
    }
    
    /* Find builtins */
    printf("=== BUILTINS USED ===\n");
    for (i = 0; i < (int)header.numfunctions; i++) {
        if (funcs[i].first_statement < 0) {
            int builtin_num = -funcs[i].first_statement;
            const char *name = strings + funcs[i].s_name;
            printf("  [%3d] %s\n", builtin_num, name);
            if (builtin_num > max_builtin)
                max_builtin = builtin_num;
        }
    }
    
    printf("\nHighest builtin number used: %d\n", max_builtin);
    
    free(funcs);
    free(strings);
    fclose(f);
    return 0;
}
