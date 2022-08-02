// SPDX-License-Identifier: GPL-2.0+
#include <stdlib.h>
#define ZDICT_STATIC_LINKING_ONLY
#include <zdict.h>
#include <sys/stat.h>
#include "erofs/dict.h"
#include "erofs/io.h"
#include "erofs/print.h"
#include "erofs/cache.h"

int small_file_cnt = 0;
struct small_file small_file_list;

unsigned int erofsdict_generate(struct erofs_inode *inode,
								struct erofsdict_item **dictp, int dictcapacity,
								int fd, unsigned int segblks,
								struct erofs_buffer_head **bhp)
{
	u64 segmentsize = blknr_to_addr(segblks);
	unsigned int segs = DIV_ROUND_UP(inode->i_size, segmentsize);
	u8 *samplebuffer;
	struct erofsdict_item *dict;
	struct erofs_buffer_head *bh;
	size_t insize;
	unsigned int i;
	erofs_blk_t dictindex[segs];

	samplebuffer = (u8 *)malloc(segmentsize);
	if (!samplebuffer)
		return 0;

	dict = calloc(segs, sizeof(struct erofsdict_item));
	if (!dict)
	{
		free(samplebuffer);
		return 0;
	}

	/* allocate dictionary buffer */
	bh = erofs_balloc(DATA, 0, 0, 0);
	if (IS_ERR(bh))
	{
		free(dict);
		free(samplebuffer);
		return 0;
	}

	erofs_mapbh(bh->block);
	bh->op = &erofs_skip_write_bhops;

	erofs_dbg("Generating dictionary segments for %s", inode->i_srcpath);

	if (S_ISREG(inode->i_mode) && (inode->i_size < segmentsize))
	{
		struct small_file *pos;
		erofs_blk_t blkaddr;
		int ret;

		list_for_each_entry(pos, &small_file_list.list, list)
		{
			if (inode->i_ino[1] == pos->st_ino)
			{
				dict = pos->dict;
				if (!dict->blkaddr)
				{
					blkaddr = erofs_blknr(erofs_btell(bh, true));
					ret = dev_write(dict->buffer, blknr_to_addr(blkaddr),
									dict->dictsize);

					ret = erofs_bh_balloon(bh, dict->dictsize);
					DBG_BUGON(ret != EROFS_BLKSIZ);
					dict->blkaddr = blkaddr;
				}
				dictindex[0]=dict->blkaddr;
				break;
			}
		}
		i = 1;
		goto exit;
	}

	for (i = 0; (insize = read(fd, samplebuffer, segmentsize)) > 0; ++i)
	{
		erofs_blk_t blkaddr;
		int ret;
		size_t samplesizes[1024], dictsize;
		unsigned int nsamples;

		if (i >= segs)
			break;

		dict[i].blkaddr = 0; /* no dictionary */
		DBG_BUGON(dict[i].buffer);
		dict[i].buffer = malloc(dictcapacity);
		if (!dict[i].buffer)
			continue;

		for (nsamples = 0; nsamples < 32; ++nsamples)
			samplesizes[nsamples] = insize / 32;

		dictsize = ZDICT_trainFromBuffer(dict[i].buffer,
										 dictcapacity, samplebuffer,
										 samplesizes, nsamples);

		if (ZDICT_isError(dictsize))
		{
			free(dict[i].buffer);
			dict[i].buffer = NULL;
			continue;
		}
		dict[i].dictsize = roundup(dictsize, EROFS_BLKSIZ);

		blkaddr = erofs_blknr(erofs_btell(bh, true));
		ret = dev_write(dict[i].buffer, blknr_to_addr(blkaddr),
						dict[i].dictsize);
		if (ret)
			continue;

		ret = erofs_bh_balloon(bh, dict[i].dictsize);
		DBG_BUGON(ret != EROFS_BLKSIZ);

		dict->blkaddr = blkaddr;
		erofs_dbg("Generated %lu bytes for dictionary segment %u @ blkaddr %u",
				  dictsize | 0UL, i, blkaddr);
		dictindex[i]=dict->blkaddr;
	}
exit:
	for(int j=0;j<i;j++)
	{
		erofs_dbg("dictindex[%d]: %d", j, dictindex[j]);
	}

	lseek(fd, 0, SEEK_SET);
	free(samplebuffer);
	*dictp = dict;
	*bhp = bh;
	return i;
}

int erofs_build_shared_dicts()
{
	u64 segmentsize = blknr_to_addr(cfg.c_dictsegblks);
	u8 *samplebuffer;
	struct erofsdict_item *dict;
	size_t insize;
	struct small_file *pos;
	int fd;
	int ret;
	size_t samplesizes[1024], dictsize;
	unsigned int nsamples;

	samplebuffer = (u8 *)malloc(segmentsize);
	if (!samplebuffer)
		return 0;

	insize = 0;
	list_for_each_entry(pos, &small_file_list.list, list)
	{
		if (insize == 0)
		{
			dict = (struct erofsdict_item *)malloc(sizeof(struct erofsdict_item));
			if (!dict)
			{
				free(samplebuffer);
				return 0;
			}
			dict->link = 0;
			dict->blkaddr = 0;
			DBG_BUGON(dict->buffer);
			dict->buffer = malloc(cfg.c_dictcapacity);
		}
		if (insize < segmentsize)
		{
			fd = open(pos->i_srcpath, O_RDONLY | O_BINARY);
			if (fd < 0)
			{
				erofs_err("Failed to build shared dicts:open %s",
						  pos->i_srcpath);
				return fd;
			}

			ret = read(fd, samplebuffer + insize, segmentsize - insize);
			if (ret < 0)
			{
				erofs_err("Failed to build shared dicts:read %s",
						  pos->i_srcpath);
				return ret;
			}

			insize += ret;
			lseek(fd, 0, SEEK_SET);
			close(fd);

			pos->dict = dict;
			dict->link++;

			if (insize >= segmentsize || pos->list.next == &small_file_list.list)
			{
				for (nsamples = 0; nsamples < 32; ++nsamples)
					samplesizes[nsamples] = insize / 32;

				dictsize = ZDICT_trainFromBuffer(dict->buffer,
												 cfg.c_dictcapacity, samplebuffer,
												 samplesizes, nsamples);

				if (ZDICT_isError(dictsize))
				{
					free(dict->buffer);
					dict->buffer = NULL;
					return EINVAL;
				}
				dict->dictsize = roundup(dictsize, EROFS_BLKSIZ);
				if (pos->list.next == &small_file_list.list)
					goto exit;

				insize = 0;
			}
		}
	}
exit:
	free(samplebuffer);
	return 0;
}

void erofs_save_shared_dicts()
{
	struct small_file *pos;
	int fd;

	fd = open("./dict-buffer", O_APPEND);
	list_for_each_entry(pos, &small_file_list.list, list)
	{
		write(fd, pos->dict->buffer, blknr_to_addr(pos->dict->dictsize));
	}
	close(fd);
}

void erofsdict_free(struct erofsdict_item *dict, unsigned int segs)
{
	unsigned int i;

	for (i = 0; i < segs; ++i)
	{
		if (dict[i].buffer)
		{
			DBG_BUGON(!dict[i].dictsize);
			free(dict[i].buffer);
		}
	}
	if (dict)
	{
		free(dict);
	}
}

void erofs_free_shared_dicts()
{
	struct small_file *pos, *n;
	FILE *fd;

	fd = fopen("./dict-buffer", "w");
	list_for_each_entry_safe(pos, n, &small_file_list.list, list)
	{
		if (pos->dict)
		{
			if (pos->dict->link > 1)
				pos->dict->link--;
			else
			{
				fwrite(pos->dict->buffer, cfg.c_dictcapacity, 1, fd);
				free(pos->dict->buffer);
				free(pos->dict);
			}
		}
		free(pos->i_srcpath);
		list_del(&pos->list);
		free(pos);
	}
	fclose(fd);
}
