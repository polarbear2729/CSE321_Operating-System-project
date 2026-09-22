// Build: gcc -O2 -std=c17 -Wall -Wextra mkfs_minivsfs.c -o mkfs_builder
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>
#include <stdarg.h>

#define BS 4096u               // block size
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

uint64_t g_random_seed = 0; // This should be replaced by seed value from the CLI.

// below contains some basic structures you need for your project
// you are free to create more structures as you require

#pragma pack(push, 1)
typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    uint32_t magic;               // 0x4D565346
    uint32_t version;             // 1
    uint32_t block_size;          // 4096
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;          // 1
    uint64_t mtime_epoch;         // build time (Unix epoch)
    uint32_t flags;               // 0

    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint32_t checksum;            // crc32(superblock[0..4091])
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR INODE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint16_t mode;                // file/dir
    uint16_t links;               // link count
    uint32_t uid;                 // 0
    uint32_t gid;                 // 0
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];  // 12 direct pointers (absolute block numbers)
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;             // group id = 12
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    // THIS FIELD SHOULD STAY AT THE END
    // ALL OTHER FIELDS SHOULD BE ABOVE THIS
    uint64_t inode_crc;   // low 4 bytes store crc32 of bytes [0..119]; high 4 bytes 0

} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    // CREATE YOUR DIRECTORY ENTRY STRUCTURE HERE
    // IF CREATED CORRECTLY, THE STATIC_ASSERT ERROR SHOULD BE GONE
    uint32_t inode_no;            // 0 if free
    uint8_t  type;                // 1=file, 2=dir
    char     name[58];            // not necessarily null-terminated

    uint8_t  checksum; // XOR of bytes 0..62
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");


// ==========================DO NOT CHANGE THIS PORTION=========================
// These functions are there for your help. You should refer to the specifications to see how you can use them.
// ====================================CRC32====================================
uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
// ====================================CRC32====================================

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    // zero crc area before computing
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c; // low 4 bytes carry the crc
}

// WARNING: CALL THIS ONLY AFTER ALL OTHER SUPERBLOCK ELEMENTS HAVE BEEN FINALIZED
void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];   // covers ino(4) + type(1) + name(58)
    de->checksum = x;
}

// ========================= Helper utilities (you may add) ====================
static uint64_t ceil_div_u64(uint64_t a, uint64_t b){ return (a + b - 1) / b; }

static void bitmap_set(uint8_t* bmp, uint64_t idx){
    bmp[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}

static int bitmap_test(const uint8_t* bmp, uint64_t idx){
    return (bmp[idx >> 3] >> (idx & 7u)) & 1u;
}

static int parse_u64(const char* s, uint64_t* out){
    char* end=NULL;
    errno=0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || !end || *end!='\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static void die(const char* msg){
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}

static void dief(const char* fmt, ...){
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

#include <stdarg.h>
// ============================================================================

int main(int argc, char** argv) {
    crc32_init();

    const char* out_image = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;

    // -------------------------- CLI parsing --------------------------
    for (int i=1;i<argc;i++){
        if (!strcmp(argv[i], "--image") && i+1<argc){ out_image = argv[++i]; }
        else if (!strcmp(argv[i], "--size-kib") && i+1<argc){ if(parse_u64(argv[++i], &size_kib)) die("--size-kib must be an integer"); }
        else if (!strcmp(argv[i], "--inodes") && i+1<argc){ if(parse_u64(argv[++i], &inode_count)) die("--inodes must be an integer"); }
        else if (!strcmp(argv[i], "--seed") && i+1<argc){ if(parse_u64(argv[++i], &g_random_seed)) die("--seed must be an integer"); }
        else dief("unknown or incomplete arg: %s", argv[i]);
    }
    if (!out_image) die("missing --image");
    if (size_kib < 180 || size_kib > 4096 || (size_kib % 4)!=0) die("--size-kib must be in [180..4096] and a multiple of 4");
    if (inode_count < 128 || inode_count > 512) die("--inodes must be in [128..512]");

    uint64_t total_blocks = (size_kib * 1024u) / BS;
    // inode table blocks
    uint64_t inode_table_bytes = inode_count * INODE_SIZE;
    uint64_t inode_table_blocks = ceil_div_u64(inode_table_bytes, BS);

    // Layout per spec
    uint64_t inode_bitmap_start = 1;
    uint64_t inode_bitmap_blocks = 1;
    uint64_t data_bitmap_start  = 2;
    uint64_t data_bitmap_blocks = 1;
    uint64_t inode_table_start  = 3;
    uint64_t data_region_start  = inode_table_start + inode_table_blocks;

    if (data_region_start >= total_blocks) die("image too small for metadata");
    uint64_t data_region_blocks = total_blocks - data_region_start;
    if (data_region_blocks == 0) die("no data region blocks available");

    // --------------- Allocate zeroed image buffer ---------------
    uint64_t image_bytes = total_blocks * BS;
    uint8_t* img = (uint8_t*)calloc(1, image_bytes);
    if (!img) die("out of memory");

    // --------------- Pointers to regions ---------------
    superblock_t* sb = (superblock_t*)(img + 0*BS);
    uint8_t* inode_bmp = img + inode_bitmap_start*BS;
    uint8_t* data_bmp  = img + data_bitmap_start*BS;
    inode_t* inode_table = (inode_t*)(img + inode_table_start*BS);
    uint8_t* data_region = img + data_region_start*BS;

    // --------------- Build superblock ---------------
    memset(sb, 0, sizeof(*sb));
    sb->magic = 0x4D565346u; // 'MVFS'
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inode_count;
    sb->inode_bitmap_start = inode_bitmap_start;
    sb->inode_bitmap_blocks = inode_bitmap_blocks;
    sb->data_bitmap_start = data_bitmap_start;
    sb->data_bitmap_blocks = data_bitmap_blocks;
    sb->inode_table_start = inode_table_start;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = data_region_start;
    sb->data_region_blocks = data_region_blocks;
    sb->root_inode = ROOT_INO;
    sb->mtime_epoch = (uint64_t)time(NULL);
    sb->flags = 0;
    superblock_crc_finalize(sb);

    // --------------- Initialize bitmaps ---------------
    // Mark inode #1 allocated
    bitmap_set(inode_bmp, 0); // bit 0 -> inode 1

    // Allocate first data block in data region to root directory
    bitmap_set(data_bmp, 0); // bit 0 -> data_region_start

    // --------------- Build root inode (inode #1) ---------------
    inode_t* root = &inode_table[0];
    memset(root, 0, sizeof(*root));
    root->mode = 0040000; // directory
    root->links = 2;      // "." and ".."
    root->uid = 0; root->gid = 0;
    root->size_bytes = BS; // one block of directory entries
    root->atime = root->mtime = root->ctime = (uint64_t)time(NULL);
    for (int i=0;i<DIRECT_MAX;i++) root->direct[i]=0;
    root->direct[0] = (uint32_t)data_region_start; // absolute block number
    root->reserved_0 = root->reserved_1 = root->reserved_2 = 0;
    root->proj_id = 12;   // group id
    root->uid16_gid16 = 0;
    root->xattr_ptr = 0;
    inode_crc_finalize(root);

    // --------------- Write "." and ".." directory entries ---------------
    dirent64_t de;
    uint8_t* blk0 = data_region + 0*BS;
    memset(blk0, 0, BS);

    memset(&de, 0, sizeof(de));
    de.inode_no = ROOT_INO;
    de.type = 2;
    memset(de.name, 0, sizeof(de.name));
    de.name[0] = '.';
    dirent_checksum_finalize(&de);
    memcpy(blk0 + 0*sizeof(dirent64_t), &de, sizeof(de));

    memset(&de, 0, sizeof(de));
    de.inode_no = ROOT_INO;
    de.type = 2;
    memset(de.name, 0, sizeof(de.name));
    de.name[0] = '.';
    de.name[1] = '.';
    dirent_checksum_finalize(&de);
    memcpy(blk0 + 1*sizeof(dirent64_t), &de, sizeof(de));

    // --------------- Serialize to disk ---------------
    FILE* f = fopen(out_image, "wb");
    if (!f){ perror("fopen"); free(img); return 1; }
    size_t wr = fwrite(img, 1, image_bytes, f);
    if (wr != image_bytes){ fprintf(stderr, "short write\n"); fclose(f); free(img); return 1; }
    fclose(f);
    free(img);

    return 0;
}
