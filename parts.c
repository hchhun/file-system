#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> 
#include <arpa/inet.h>
#include <time.h>
#include <math.h>

#define BLOCK 512
#define FAT_EOF 0xFFFFFFFF
#define FAT_RES 0x00000001

typedef struct __attribute__((packed)) {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} Date;

typedef struct __attribute__((packed)) {
    char id_str[8];
    uint16_t block_size;
    uint32_t block_count;
    uint32_t fat_start;
    uint32_t fat_blocks;
    uint32_t root_start;
    uint32_t root_blocks;
} Superblock;

typedef struct __attribute__((packed)) {
    uint32_t free;
    uint32_t reserved;
    uint32_t alloc;
    uint32_t start_block;
    uint32_t size;
    uint32_t count;
} Fat;

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint32_t start;
    uint32_t block_count;
    uint32_t size;
    Date creation;
    Date modified;
    char name[31];
    uint8_t padding[6];
} Directory;    // Directory AND file struct

/*------------------------------------------------------------------------------------------------------------------------------------------------*/

// get current time
Date get_time() {
    time_t t = time(NULL);          
    struct tm *tm_info = localtime(&t); 

    Date d;
    d.year = htons(tm_info->tm_year + 1900); 
    d.month = tm_info->tm_mon + 1;    
    d.day = tm_info->tm_mday;
    d.hour = tm_info->tm_hour;
    d.minute = tm_info->tm_min;
    d.second = tm_info->tm_sec;

    return d;
}

// get file system's superblock 
void get_superblock (FILE* disk, Superblock *sb) {
    unsigned char buffer[BLOCK];
    fread(buffer, 1, BLOCK, disk);

    if (memcmp(buffer, "CSC360FS", 8) != 0) {
        fprintf(stderr, "Invalid file system: identifier string must be 'CSC360FS'\n");
        exit(1);
    } 

    sb->block_size = ntohs(*(uint16_t*)(buffer + 8));
    sb->block_count = ntohl(*(uint32_t*)(buffer + 10));
    sb->fat_start = ntohl(*(uint32_t*)(buffer + 14));
    sb->fat_blocks = ntohl(*(uint32_t*)(buffer + 18));
    sb->root_start = ntohl(*(uint32_t*)(buffer + 22));
    sb->root_blocks = ntohl(*(uint32_t*)(buffer + 26));
}

// get file system's FAT
uint32_t* get_fat (FILE *disk, Fat *f, Superblock *sb, uint32_t *buffer) {
    f->free = 0;
    f->reserved = 0;
    f->alloc = 0;

    f->start_block = sb->fat_start * sb->block_size;
    f->size = sb->fat_blocks * sb->block_size;
    f->count = (sb->fat_blocks * sb->block_size) / sizeof(uint32_t);

    uint32_t val;
    fseek(disk, f->start_block, SEEK_SET);
    fread(buffer, sizeof(uint32_t), f->count, disk);

    for (int i = 0; i < f->count; i++) {
        val = ntohl(buffer[i]);

        if (val == 0) f->free++;
        else if (val == 1) f->reserved++;
        else f->alloc++;
    }

    return buffer;
}

// creates a new directory with default metadata
Directory create_dir(char *name, uint32_t start, uint32_t size) {
    Directory new;
    new.status = 0x05;          // used + directory
    new.start = htonl(start);
    new.block_count = htonl(1);
    new.size = htonl(size);
    new.creation = get_time();
    new.modified = get_time();
    strcpy(new.name, name);
    
    return new;
}

// creates a new file with default metadata
Directory create_file(char *name, uint32_t start, uint32_t needed, uint32_t size) {
    Directory new;
    new.status = 0x03;          // used + file
    new.start = htonl(start);
    new.block_count = htonl(needed);
    new.size = htonl(size);     // same size of file copied from
    new.creation = get_time();
    new.modified = get_time();
    strcpy(new.name, name);
    
    return new;
}

/*------------------------------------------------------------------------------------------------------------------------------------------------*/

// lists all the entries in a given directory
void list_entries (Directory *buffer, uint32_t file_count) {
    Directory entry;

    for (int i = 0; i < file_count; i++) {
        entry = buffer[i];
        uint32_t status = entry.status & 0x07;

        if (status == 0) continue;
        char type = (status & 0x02) ? 'F' : 'D';

        printf("%c %10u %30s %u/%02u/%02u %02u:%02u:%02u\n", type, ntohl(entry.size), entry.name, 
            ntohs(entry.creation.year), entry.creation.month, entry.creation.day, entry.creation.hour,
            entry.creation.minute, entry.creation.second);
    }
}


// copies a file from the disk image to the local directory
void copy_file (FILE *disk, Directory input, char *output, Superblock *sb) {
    FILE *out = fopen(output, "wb");
    if(!out) {
        perror("Error opening output file");
        exit(1);
    }

    uint32_t write_size;
    uint32_t cur_block = ntohl(input.start);
    uint32_t byte_left = ntohl(input.size);
    uint32_t block_size = sb->block_size;
    char buffer[block_size];

    Fat f;
    uint32_t fat_buf[sb->fat_blocks * block_size];
    get_fat(disk, &f, sb, fat_buf);

    while (cur_block != FAT_EOF && byte_left > 0) {             // traverse through FAT
        if (byte_left > block_size) write_size = block_size;    // write size is relative to size of leftover bytes
        else write_size = byte_left;                        

        fseek(disk, cur_block * block_size, SEEK_SET);
        fread(buffer, 1, write_size, disk);                     // get from input
        fwrite(buffer, 1, write_size, out);                     // write to output

        cur_block = ntohl(fat_buf[cur_block]);                  // move to next FAT block
        byte_left -= write_size;
    }

    fclose(out);
}


// loads (copies) a file from the local directory to the disk image
void load_file (FILE *file, FILE *disk, uint32_t file_start, uint32_t size, Superblock *sb) {
    uint32_t write_size;
    uint32_t cur_block = file_start;
    uint32_t byte_left = size;
    uint32_t block_size = sb->block_size;
    char buffer[block_size];

    Fat f;
    uint32_t fat_buf[sb->fat_blocks * sb->block_size];
    get_fat(disk, &f, sb, fat_buf);

    fseek(file, 0, SEEK_SET);

    while (cur_block != FAT_EOF && byte_left > 0) {             // traverse through allocated FAT blocks
        if (byte_left > block_size) write_size = block_size;    // write size is relative to size of leftover bytes
        else write_size = byte_left;

        fread(buffer, 1, write_size, file);                     // get from input
        fseek(disk, cur_block * block_size, SEEK_SET);
        fwrite(buffer, 1, write_size, disk);                    // write to disk image

        cur_block = ntohl(fat_buf[cur_block]);                  // move to next FAT block
        byte_left -= write_size;
    }
}


// checks for sufficient space in the FAT 
// if sufficient, allocate space and return the start block of the file
uint32_t alloc_fat(FILE *disk, int needed, uint32_t *fat_buf, Fat *f, int dir_count) {
    uint32_t fat_count = f->count;
    uint32_t file_start = FAT_EOF;  // start of file
    uint32_t prev_block = FAT_EOF; 
    int allocated = 0;

    uint32_t i = 0;
    while (i < fat_count && allocated <= needed) {
        if (ntohl(fat_buf[i]) == 0) {
            if (file_start == FAT_EOF) file_start = i;
            if (prev_block != FAT_EOF) fat_buf[prev_block] = i;
            prev_block = i;
            allocated++;
        }
        i++;
    }

    int dir_space = allocated + dir_count;  // additional space for directories created in the path
    
    // aborts operation if insufficient, FAT unaffected
    if (allocated < needed || prev_block > fat_count-1 || dir_space > f->free) {
        fprintf(stderr, "Error: Insufficient space\n");
        exit(1);
    }

    fat_buf[prev_block] = FAT_EOF;

    // writes new FAT info to disk
    fseek(disk, f->start_block, SEEK_SET);
    fwrite(fat_buf, sizeof(uint32_t), fat_count, disk);
    return file_start;
}


// creates a path of directories leading up to the target file
// for mode 2 (i.e. diskput)
void create_path (FILE *disk, char *argv[], Superblock *sb, Fat *f, uint32_t *fat_buf, char *filename, int count) {
    FILE *file = fopen(argv[2], "rb");  // file to be copied from
    if (file == NULL) {
        fprintf(stderr, "Source file %s not found.\n", argv[2]);
        exit(1);
    }

    fseek(file, 0, SEEK_END);
    uint32_t size = ftell(file);
    int needed = ceil(size / sb->block_size);

    uint32_t file_start = alloc_fat(disk, needed, fat_buf, f, count);   // check for space, allocate

    uint32_t fat_count = f->count;
    uint32_t block_size = sb->block_size;

    uint32_t new_block = FAT_EOF;
    uint32_t dir_start_block = sb->root_start * block_size;            
    uint32_t dir_size = sb->root_blocks * block_size;
    uint32_t file_count = dir_size / sizeof(Directory);

    int found;
    int available;
    char *path = argv[3];

    Directory entry;
    Directory *buffer = malloc(dir_size);
    fseek(disk, dir_start_block, SEEK_SET);
    fread(buffer, sizeof(Directory), file_count, disk);
    
    char *token = strtok(path, "/");
    while (token != NULL) {
        if (strcmp(token, filename) == 0) break;
        found = 0;                                                       // reset for each descent
        available = -1;

        for (int i = 0; i < file_count; i++) {
            entry = buffer[i];
            uint32_t status = entry.status & 0x07;
            if (status == 0 && available == -1) available = i;      
            
            if (strcmp(entry.name, token) == 0) {                       // if directory matches, descend 
                found = 1;
                dir_start_block = ntohl(entry.start) * block_size;
                dir_size = ntohl(entry.block_count) * block_size;
                file_count = dir_size / sizeof(Directory);

                buffer = realloc(buffer, dir_size);                     // realloc to inside of this directory
                fseek(disk, dir_start_block, SEEK_SET);             
                fread(buffer, sizeof(Directory), file_count, disk);
                break;
            }
        }

        if (!found && available != -1) {                                // create new directory if not found
            for (uint32_t i = 0; i < fat_count; i++) {                  // FAT space already checked in alloc_fat
                if (ntohl(fat_buf[i]) == 0) { 
                    new_block = i;
                    fat_buf[i] = htonl(FAT_RES);                   
                    break;
                }
            }

            void *empty = calloc(1, block_size);                        // init empty placeholder
            fseek(disk, new_block * block_size, SEEK_SET);              // and write to disk
            fwrite(empty, 1, block_size, disk);
            free(empty);

            Directory new = create_dir(token, new_block, block_size);
            fseek(disk, dir_start_block + available * sizeof(Directory), SEEK_SET);
            fwrite(&new, sizeof(Directory), 1, disk);                   // write directory to disk

            dir_start_block = new_block * block_size;
            dir_size = block_size;
            file_count = dir_size / sizeof(Directory);

            buffer = realloc(buffer, dir_size);
            if (buffer == NULL) {
                free(buffer);
                perror("realloc failed\n");
                exit(1);
            }

            fseek(disk, dir_start_block, SEEK_SET);                     
            fread(buffer, sizeof(Directory), file_count, disk);         // descend into the new directory
        }
        
        token = strtok(NULL, "/");
    }

    available = -1;                                                     // recheck free location (for file)
    for (int i = 0; i < file_count; i++) {
        if ((buffer[i].status & 0x07) == 0) {  // empty slot
            available = i;
            break;
        }
    }

    load_file(file, disk, file_start, size, sb);                        // load the file contents
    Directory new = create_file(filename, file_start, needed, size);
    fseek(disk, dir_start_block + (available * sizeof(Directory)), SEEK_SET);
    fwrite(&new, sizeof(Directory), 1, disk);                           // write new loaded file to disk

    free(buffer);
    fclose(file);
}


// traverses the directories currently in the disk image
// for disklist (mode 0) | diskget (mode 1) | diskput (mode 2)
int traverse (FILE *disk, char *argv[], int mode) {
    Fat f;
    Superblock sb;
    get_superblock(disk, &sb);
    uint32_t fat_buf[sb.fat_blocks * sb.block_size];
    get_fat(disk, &f, &sb, fat_buf);
    
    uint32_t fat_count = f.count;
    uint32_t dir_start_block = sb.root_start * sb.block_size;
    uint32_t dir_size = sb.root_blocks * sb.block_size;
    uint32_t file_count = dir_size / sizeof(Directory);
    int count = -1;
    
    int found;
    Directory entry;
    Directory *buffer = malloc(dir_size);
    fseek(disk, dir_start_block, SEEK_SET);
    fread(buffer, sizeof(Directory), file_count, disk);

    char *length = argv[2];
    int len = strlen(length);
    char path[512];
    char error_path[512];

    if (mode == 2) {
        strcpy(path, argv[3]);
        strcpy(error_path, argv[3]);
    }
    else {
        strcpy(path, argv[2]);
        strcpy(error_path, argv[2]);
    }

    char filename[256];
    int available;
    
    if (mode == 0 && path[0] != '/') {
        free(buffer);
        fprintf(stderr, "Usage: ./disklist <disk image> /<directory>\n");
        exit(1);
    }

    char *token = strtok(path, "/");
    if (token == NULL && mode == 0) {
        list_entries(buffer, file_count);
        free(buffer);
        return 0;
    }
    while (token != NULL) {
        count++;                                        // directory depth    
        found = 0;
        available = -1;
        strcpy(filename, token);                        // save last filename

        for (int i = 0; i < file_count; i++) {
            entry = buffer[i];  
            uint32_t status = entry.status & 0x07;

            if (status == 0 && available == -1) {
                available = i;
                if (mode != 2) continue;                // skip empty dir if not for diskput
            }
            
            if (strcmp(entry.name, token) == 0) {       // if directory matches, descent
                found = 1;  
                dir_start_block = ntohl(entry.start) * sb.block_size;
                dir_size = ntohl(entry.block_count) * sb.block_size;
                file_count = dir_size / sizeof(Directory);

                buffer = realloc(buffer, dir_size);
                fseek(disk, dir_start_block, SEEK_SET);
                fread(buffer, sizeof(Directory), file_count, disk);
                break;
            }
        }

        if (mode == 2 && available == -1 && !found) {   // aborts if diskput with no space
            fprintf(stderr, "Error: Insufficient space\n");
            exit(1);
        }
        
        token = strtok(NULL, "/");
    }


    if (mode == 0) {        // disklist
        if (!found) {
            fprintf(stderr, "Directory not found\n");
            free(buffer);
            exit(1);
        }
        else if (entry.status & 0x07 & 0x02) {          // cannot traverse a file
            fprintf(stderr, "Entry is a file\n");  
            free(buffer);
            exit(1);
        }

        list_entries(buffer, file_count);
    }
    else if (mode == 1) {   // diskget
        if (!found) {
            int f_len = strlen(filename);
            error_path[len - f_len - 1] = '\0';

            if (strcmp(error_path, "") == 0) strcpy(error_path, "/");
            else if (error_path[0] != '/') {
                fprintf(stderr, "Usage: ./diskget <disk image> /<directory> <output file>\n");
                free(buffer);
                exit(1);
            }

            fprintf(stderr, "Requested file %s not found in %s.\n", filename, error_path);
            free(buffer);
            exit(1);
        }

        copy_file(disk, entry, argv[3], &sb);
    }
    else if (mode == 2) {   // diskput
        if (available == -1 && !found) {
            fprintf(stderr, "Error: Insufficient space\n");
            exit(1);
        }

        create_path(disk, argv, &sb, &f, fat_buf, filename, count);     // directory space check done
    }                                                                   // now create actual path
    
    free(buffer);
}

/*------------------------------------------------------------------------------------------------------------------------------------------------*/

// main function for diskinfo
void diskinfo_main (int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./diskinfo <disk image>\n");
        exit(1);
    }

    FILE *disk = fopen(argv[1], "rb");
    if (disk == NULL) {
        perror("Error opening disk image");
        exit(1);
    }

    Superblock sb;
    Fat f;
    get_superblock(disk, &sb);

    uint32_t buffer[sb.fat_blocks * sb.block_size];
    get_fat(disk, &f, &sb, buffer);

    printf("Super block information:\n");
    printf("Block size: %u\n", sb.block_size);
    printf("Block count: %u\n", sb.block_count);
    printf("FAT starts: %u\n", sb.fat_start);
    printf("FAT blocks: %u\n", sb.fat_blocks);
    printf("Root directory start: %u\n", sb.root_start);
    printf("Root directory blocks: %u\n", sb.root_blocks);

    printf("\nFAT information:\n");
    printf("Free Blocks: %u\n", f.free);
    printf("Reserved Blocks: %u\n", f.reserved);
    printf("Allocated Blocks: %u\n", f.alloc);

    fclose(disk);
}


// main function for disklist
void disklist_main (int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: ./disklist <disk image> /<directory>\n");
        exit(1);
    }

    FILE *disk = fopen(argv[1], "rb");
    if (disk == NULL) {
        perror("Error opening disk image");
        exit(1);
    }

    traverse(disk, argv, 0);
    fclose(disk);
}


// main function for diskget
void diskget_main (int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: ./diskget <disk image> /<directory> <output file>\n");
        exit(1);
    }

    FILE *disk = fopen(argv[1], "rb");
    if (disk == NULL) {
        perror("Error opening disk image");
        exit(1);
    }

    traverse(disk, argv, 1);
    fclose(disk);
}


// main function for diskput
void diskput_main (int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: ./diskget <disk image> /<directory> <output file>\n");
        exit(1);
    }

    FILE *disk = fopen(argv[1], "r+b");
    if (disk == NULL) {
        perror("Error opening disk image");
        exit(1);
    }

    traverse(disk, argv, 2);

    fclose(disk);
}


int main(int argc, char *argv[])
{
    #ifdef DISKINFO
        diskinfo_main(argc, argv);
    #elif defined(DISKLIST)
        disklist_main(argc, argv);
    #elif defined(DISKGET)
        diskget_main(argc, argv);
    #elif defined(DISKPUT)
        diskput_main(argc, argv);
    #else
        #error "No main function specified"
    #endif
    
    return 0;
}