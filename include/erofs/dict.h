#ifndef __EROFS_DICT_H
#define __EROFS_DICT_H

#include "erofs/internal.h"

struct erofsdict_item
{
    char *buffer;
    int dictsize;
    erofs_blk_t blkaddr;
	int link;
};

unsigned int erofsdict_generate(struct erofs_inode *inode,
		struct erofsdict_item **dictp, int dictcapacity,
		int fd, unsigned int segblks,
		struct erofs_buffer_head **bhp);

void erofsdict_free(struct erofsdict_item *dict, unsigned int segs);

struct small_file
{
	struct list_head list;
	__ino64_t st_ino;
	char *i_srcpath;
	__off_t st_size;
	struct erofsdict_item *dict;
};

extern int small_file_cnt;
extern struct small_file small_file_list;

int erofs_build_shared_dicts();
void erofs_free_shared_dicts();
void erofs_save_shared_dicts();

#endif