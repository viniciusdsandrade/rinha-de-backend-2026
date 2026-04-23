#include "rinha.h"

#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "uso: prepare_refs_c <references.json.gz> <references.bin> <labels.bin>\n");
        return 1;
    }

    char error[256] = {0};
    ReferenceSet refs;
    if (!refs_load_gzip_json(argv[1], &refs, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }

    if (!refs_write_binary(&refs, argv[2], argv[3], error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        refs_free(&refs);
        return 1;
    }

    fprintf(stderr, "prepared %zu referências em %s e %s\n", refs.rows, argv[2], argv[3]);
    refs_free(&refs);
    return 0;
}
