#include "index.h"
#include "tree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// 🔥 REQUIRED (since no object.h exists)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);


// ─── PROVIDED ─────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}


// ─── LOAD ─────────────────────────────────────────────

int index_load(Index *index) {
    FILE *f = fopen(INDEX_FILE, "r");
    index->count = 0;

    if (!f) return 0;

    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime;

        int scanned = sscanf(line, "%o %64s %llu %u %511[^\n]",
            &e->mode, hex, &mtime, &e->size, e->path);

        if (scanned != 5) continue;

        e->mtime_sec = (uint64_t)mtime;

        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);
    return 0;
}


// ─── SAVE ─────────────────────────────────────────────

static int compare_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path,
                  ((IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    Index sorted = *index;
    qsort(sorted.entries, sorted.count,
          sizeof(IndexEntry), compare_entries);

    char hex[HASH_HEX_SIZE + 1];

    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];

        hash_to_hex(&e->hash, hex);

        fprintf(f, "%o %s %llu %u %s\n",
            e->mode, hex,
            (unsigned long long)e->mtime_sec,
            e->size, e->path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp, INDEX_FILE);
}


// ─── ADD (FIXED) ─────────────────────────────────────

int index_add(Index *index, const char *path) {

    struct stat st;
    if (stat(path, &st) != 0) return -1;

    if (!S_ISREG(st.st_mode)) {
        // 🚫 DO NOT try to read directories (THIS CAUSES YOUR SEGFAULT)
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }

    rewind(f);

    void *data = malloc(size > 0 ? size : 1);
    if (!data) {
        fclose(f);
        return -1;
    }

    if (size > 0 && fread(data, 1, size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return -1;
    }

    fclose(f);

    ObjectID hash;
    if (object_write(OBJ_BLOB, data, size, &hash) != 0) {
        free(data);
        return -1;
    }

    free(data);

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }

    if (st.st_mode & S_IXUSR)
        e->mode = 0100755;
    else
        e->mode = 0100644;

    e->hash = hash;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size = (uint32_t)st.st_size;

    strncpy(e->path, path, sizeof(e->path));
    e->path[sizeof(e->path) - 1] = '\0';

    return index_save(index);
}