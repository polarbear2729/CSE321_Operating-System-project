#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <sys/types.h>

#define BS 4096u
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12
#pragma pack(push, 1)

typedef struct {
    // CREATE YOUR SUPERBLOCK HERE
    // ADD ALL FIELDS AS PROVIDED BY THE SPECIFICATION
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
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
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;

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
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[DIRECT_MAX];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
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
    uint32_t inode_no;
    uint8_t  type;
    char     name[58];

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

static int bitmap_test(const uint8_t* bmp, uint64_t idx){
    return (bmp[idx >> 3] >> (idx & 7u)) & 1u;
}
static void bitmap_set(uint8_t* bmp, uint64_t idx){
    bmp[idx >> 3] |= (uint8_t)(1u << (idx & 7u));
}
static int bitmap_find_first_zero(const uint8_t* bmp, uint64_t limit_bits){
    for (uint64_t i=0;i<limit_bits;i++){
        if (!bitmap_test(bmp,i)) return (int)i;
    }
    return -1;
}

static int parse_args(int argc, char** argv, const char** in_img, const char** out_img, const char** host_file){
    *in_img = *out_img = *host_file = NULL;
    for(int i=1;i<argc;i++){
        if (!strcmp(argv[i], "--input") && i+1<argc) *in_img = argv[++i];
        else if (!strcmp(argv[i], "--output") && i+1<argc) *out_img = argv[++i];
        else if (!strcmp(argv[i], "--file") && i+1<argc) *host_file = argv[++i];
        else return -1;
    }
    return (*in_img && *out_img && *host_file) ? 0 : -1;
}

static void die(const char* msg){
    fprintf(stderr, "Error: %s\n", msg);
    exit(1);
}
// ============================================================================

int main(int argc, char** argv) {
    crc32_init();
    const char *input_path, *output_path, *file_path;
    if (parse_args(argc, argv, &input_path, &output_path, &file_path)) {
        fprintf(stderr, "Usage: mkfs_adder --input in.img --output out.img --file <path>\n");
        return 1;
    }

    // -------- load input image --------
    FILE* fi = fopen(input_path, "rb");
    if (!fi){ perror("fopen input"); return 1; }
    fseeko(fi, 0, SEEK_END);
    off_t fsz = ftello(fi);
    if (fsz < (off_t)BS){ fprintf(stderr, "image too small\n"); fclose(fi); return 1; }
    fseeko(fi, 0, SEEK_SET);
    uint8_t* img = (uint8_t*)malloc((size_t)fsz);
    if (!img){ fclose(fi); die("out of memory"); }
    if (fread(img, 1, (size_t)fsz, fi)!=(size_t)fsz){ fclose(fi); free(img); die("short read"); }
    fclose(fi);

    // -------- parse superblock --------
    superblock_t* sb = (superblock_t*)(img + 0*BS);
    if (sb->magic != 0x4D565346u || sb->version != 1 || sb->block_size != BS){
        free(img); die("invalid superblock");
    }

    // region pointers
    uint8_t* inode_bmp = img + sb->inode_bitmap_start*BS;
    uint8_t* data_bmp  = img + sb->data_bitmap_start*BS;
    inode_t* inode_table = (inode_t*)(img + sb->inode_table_start*BS);
    uint8_t* data_region = img + sb->data_region_start*BS;

    // -------- load host file --------
    FILE* ff = fopen(file_path, "rb");
    if (!ff){ perror("fopen file"); free(img); return 1; }
    fseeko(ff, 0, SEEK_END);
    off_t fsz_file = ftello(ff);
    fseeko(ff, 0, SEEK_SET);

    uint64_t need_blocks = (uint64_t)ceil_div_u64((uint64_t)fsz_file, BS);
    if (need_blocks > DIRECT_MAX){
        fprintf(stderr, "File too large for MiniVSFS (needs %" PRIu64 " blocks, max %d)\n", need_blocks, DIRECT_MAX);
        fclose(ff); free(img); return 1;
    }

    // -------- find free inode --------
    int free_ino_bit = bitmap_find_first_zero(inode_bmp, sb->inode_count);
    if (free_ino_bit < 0){ fclose(ff); free(img); die("no free inode"); }
    uint32_t new_ino_no = (uint32_t)(free_ino_bit + 1); // 1-indexed

    // -------- find free data blocks --------
    uint32_t blocks_abs[DIRECT_MAX]; memset(blocks_abs, 0, sizeof(blocks_abs));
    for (uint64_t k=0;k<need_blocks;k++){
        int b = bitmap_find_first_zero(data_bmp, sb->data_region_blocks);
        if (b < 0){ fclose(ff); free(img); die("no free data blocks"); }
        bitmap_set(data_bmp, (uint64_t)b);
        blocks_abs[k] = (uint32_t)(sb->data_region_start + (uint64_t)b);
    }

    // -------- write file data to data region --------
    for (uint64_t k=0;k<need_blocks;k++){
        uint8_t* blk = img + (uint64_t)blocks_abs[k] * BS;
        size_t to_read = (k == need_blocks-1) ? (size_t)( (uint64_t)fsz_file - k*BS ) : BS;
        size_t got = fread(blk, 1, to_read, ff);
        if (got != to_read){ fclose(ff); free(img); die("short read from host file"); }
        if (to_read < BS) memset(blk + to_read, 0, BS - to_read);
    }
    fclose(ff);

    // -------- create new inode --------
    inode_t* new_ino = &inode_table[free_ino_bit];
    memset(new_ino, 0, sizeof(*new_ino));
    new_ino->mode = 0100000; // regular file
    new_ino->links = 1;
    new_ino->uid = 0; new_ino->gid = 0;
    new_ino->size_bytes = (uint64_t)fsz_file;
    new_ino->atime = new_ino->mtime = new_ino->ctime = (uint64_t)time(NULL);
    for (int i=0;i<DIRECT_MAX;i++) new_ino->direct[i]=0;
    for (uint64_t k=0;k<need_blocks;k++) new_ino->direct[k] = blocks_abs[k];
    new_ino->proj_id = 12;
    new_ino->reserved_0 = new_ino->reserved_1 = new_ino->reserved_2 = 0;
    new_ino->uid16_gid16 = 0;
    new_ino->xattr_ptr = 0;
    inode_crc_finalize(new_ino);
    bitmap_set(inode_bmp, (uint64_t)free_ino_bit);

    // -------- update root directory --------
    inode_t* root = &inode_table[ROOT_INO-1];
    if (root->mode != 0040000) { free(img); die("root inode not a directory"); }

    // locate a free dirent slot across existing blocks
    uint64_t dir_entries = root->size_bytes / sizeof(dirent64_t);
    int placed = 0;
    for (uint64_t idx=0; idx<dir_entries && !placed; idx++){
        uint64_t blk_index = (idx * sizeof(dirent64_t)) / BS;
        uint64_t off_in_blk = (idx * sizeof(dirent64_t)) % BS;
        uint32_t abs_block = root->direct[blk_index];
        if (abs_block == 0) break;
        dirent64_t* slot = (dirent64_t*)(img + (uint64_t)abs_block*BS + off_in_blk);
        if (slot->inode_no == 0){
            // fill here
            memset(slot, 0, sizeof(*slot));
            slot->inode_no = new_ino_no;
            slot->type = 1;
            // basename of file_path
            const char* base = strrchr(file_path, '/'); base = base ? base+1 : file_path;
            size_t n = strlen(base); if (n > 58) n = 58;
            memcpy(slot->name, base, n);
            dirent_checksum_finalize(slot);
            placed = 1;
            break;
        }
    }

    // if not placed, allocate a new block for root
    if (!placed){
        // find first empty direct slot
        int di = -1;
        for(int i=0;i<DIRECT_MAX;i++) if (root->direct[i]==0){ di=i; break; }
        if (di<0){ free(img); die("root directory has no free direct block"); }

        // allocate a new data block
        int b = bitmap_find_first_zero(data_bmp, sb->data_region_blocks);
        if (b<0){ free(img); die("no free data blocks for root directory expansion"); }
        bitmap_set(data_bmp, (uint64_t)b);
        uint32_t new_abs = (uint32_t)(sb->data_region_start + (uint64_t)b);
        root->direct[di] = new_abs;
        root->size_bytes += BS;

        // write entry at start of new block
        uint8_t* blk = img + (uint64_t)new_abs*BS;
        memset(blk, 0, BS);
        dirent64_t* slot = (dirent64_t*)blk;
        slot->inode_no = new_ino_no;
        slot->type = 1;
        const char* base = strrchr(file_path, '/'); base = base ? base+1 : file_path;
        size_t n = strlen(base); if (n > 58) n = 58;
        memcpy(slot->name, base, n);
        dirent_checksum_finalize(slot);
        placed = 1;
    }

    // root inode checksum update (contents changed)
    inode_crc_finalize(root);

    // Optional: update superblock mtime and checksum
    sb->mtime_epoch = (uint64_t)time(NULL);
    superblock_crc_finalize(sb);

    // -------- write output --------
    FILE* fo = fopen(output_path, "wb");
    if (!fo){ perror("fopen output"); free(img); return 1; }
    if (fwrite(img, 1, (size_t)fsz, fo) != (size_t)fsz){ fprintf(stderr,"short write\n"); fclose(fo); free(img); return 1; }
    fclose(fo);
    free(img);
    return 0;
}
