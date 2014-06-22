#include <stdio.h>
#include <string.h>

#include "dj.h"
#include "md5.h"

int action_list(uint32_t inode, char *path, uint64_t pos, uint64_t file_len, char *data, uint64_t data_len, void **private)
{
    if (pos == 0)
        printf("%s\n", path);
    return 0;
}

int action_cat_info(uint32_t inode, char *path, uint64_t pos, uint64_t file_len, char *data, uint64_t data_len, void **private)
{
    printf("\n\n============== test cb inode %u, pos %lu, len %lu, path %s ==============\n\n", inode, pos, data_len, path);
    char str_data[data_len+1];
    memcpy(str_data, data, data_len);
    str_data[data_len+1] = '\0';
    printf("%s", str_data);
    return 0;
}

int action_cat(uint32_t inode, char *path, uint64_t pos, uint64_t file_len, char *data, uint64_t data_len, void **private)
{
    char str_data[data_len+1];
    memcpy(str_data, data, data_len);
    str_data[data_len+1] = '\0';
    printf("%s", str_data);
    return 0;
}

int action_info(uint32_t inode, char *path, uint64_t pos, uint64_t file_len, char *data, uint64_t data_len, void **private)
{
    printf("test cb inode %u, pos %lu, len %lu, path %s\n", inode, pos, data_len, path);
    return 0;
}

int action_none(uint32_t inode, char *path, uint64_t pos, uint64_t file_len, char *data, uint64_t data_len, void **private)
{
    return 0;
}

void usage(char *prog_name)
{
    fprintf(stderr, "Usage: %s [-cat|-info|-md5|-crc] [-direct] DEVICE DIRECTORY\n", prog_name);
    exit(1);
}

enum action {
    ACTION_MD5,
    ACTION_CAT,
    ACTION_INFO,
    ACTION_CAT_INFO,
    ACTION_CRC,
    ACTION_LIST,
    ACTION_NONE
};

int main(int argc, char **argv)
{
    if (argc < 3)
        usage(argv[0]);

    initialize_dj(argv[0]);

    enum action action = ACTION_NONE;
    int flags = 0;
    int device_index = 0;
    int dir_index = 0;

    int max_inodes = 100;
    int max_blocks = 128000;
    int coalesce_distance = 1;
    int inodes_opt = 0;
    int blocks_opt = 0;
    int coalesce_opt = 0;

    for (int i = 0; i < argc; i++)
    {
        if (!strcmp(argv[i], "-md5"))
            action = ACTION_MD5;
        else if (!strcmp(argv[i], "-cat"))
            action = ACTION_CAT;
        else if (!strcmp(argv[i], "-info"))
            action = ACTION_INFO;
        else if (!strcmp(argv[i], "-cat_info"))
            action = ACTION_CAT_INFO;
        else if (!strcmp(argv[i], "-crc"))
            action = ACTION_CRC;
        else if (!strcmp(argv[i], "-list"))
            action = ACTION_LIST;
        else if (!strcmp(argv[i], "-direct"))
            flags |= ITERATE_OPT_DIRECT;
        else if (!strcmp(argv[i], "-profile"))
            flags |= ITERATE_OPT_PROFILE;
        else if (!strcmp(argv[i], "-i"))
            inodes_opt = 1;
        else if (!strcmp(argv[i], "-b"))
            blocks_opt = 1;
        else if (!strcmp(argv[i], "-c"))
            coalesce_opt = 1;
        else if (inodes_opt)
        {
            max_inodes = atoi(argv[i]);
            inodes_opt = 0;
        }
        else if (blocks_opt)
        {
            max_blocks = atoi(argv[i]);
            blocks_opt = 0;
        }
        else if (coalesce_opt)
        {
            coalesce_distance = atoi(argv[i]);
            coalesce_opt = 0;
        }
        else if (device_index == 0)
            device_index = i;
        else if (dir_index == 0)
            dir_index = i;
        else
        {
            fprintf(stderr, "Unrecognized option %s\n", argv[i]);
            usage(argv[0]);
        }
    }

    if (device_index == 0)
    {
        fprintf(stderr, "Please specify device file\n");
        usage(argv[0]);
    }
    else if (device_index == 0)
    {
        fprintf(stderr, "Please specify directory on device\n");
        usage(argv[0]);
    }

    char *device = argv[device_index];
    char *dir = argv[dir_index];

    switch (action)
    {
        case ACTION_MD5: iterate_dir(device, dir, file_md5, max_inodes, max_blocks, coalesce_distance, flags); break;
        case ACTION_CAT: iterate_dir(device, dir, action_cat, max_inodes, max_blocks, coalesce_distance, flags); break;
        case ACTION_INFO: iterate_dir(device, dir, action_info, max_inodes, max_blocks, coalesce_distance, flags); break;
        case ACTION_CAT_INFO: iterate_dir(device, dir, action_cat_info, max_inodes, max_blocks, coalesce_distance,  flags); break;
        case ACTION_CRC: fprintf(stderr, "CRC not yet implemented\n"); exit(1); break;
        case ACTION_LIST: iterate_dir(device, dir, action_list, max_inodes, max_blocks, coalesce_distance, flags); break;
        case ACTION_NONE: iterate_dir(device, dir, action_none, max_inodes, max_blocks, coalesce_distance, flags); break;
    }
    return 0;
}
