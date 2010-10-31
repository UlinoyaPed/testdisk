/*

    File: phbs.c

    Copyright (C) 1998-2009 Christophe GRENIER <grenier@cgsecurity.org>

    This software is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write the Free Software Foundation, Inc., 51
    Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>	/* unlink, ftruncate */
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <ctype.h>      /* tolower */
#include "types.h"
#include "common.h"
#include "intrf.h"
#ifdef HAVE_NCURSES
#include "intrfn.h"
#endif
#include <errno.h>
#ifdef HAVE_WINDEF_H
#include <windef.h>
#endif
#ifdef HAVE_WINBASE_H
#include <stdarg.h>
#include <winbase.h>
#endif
#include "dir.h"
#include "fat_dir.h"
#include "list.h"
#include "lang.h"
#include "filegen.h"
#include "photorec.h"
#include "sessionp.h"
#include "phrecn.h"
#include "log.h"
#include "log_part.h"
#include "file_tar.h"
#include "phcfg.h"
#include "pblocksize.h"
#include "askloc.h"
#include "fat_unformat.h"
#include "pnext.h"
#include "phbf.h"
#include "phnc.h"
#include "phbs.h"
#include "file_found.h"

#define READ_SIZE 1024*512
extern const file_hint_t file_hint_tar;
extern file_check_list_t file_check_list;

static inline void file_recovery_cpy(file_recovery_t *dst, file_recovery_t *src)
{
  memcpy(dst, src, sizeof(*dst));
  dst->location.list.prev=&dst->location.list;
  dst->location.list.next=&dst->location.list;
}

int photorec_find_blocksize(disk_t *disk_car, partition_t *partition, const int verbose, const int interface, file_stat_t *file_stats, unsigned int *file_nbr, unsigned int *blocksize, alloc_data_t *list_search_space, const time_t real_start_time)
{
  uint64_t offset=0;
  unsigned char *buffer_start;
  unsigned char *buffer_olddata;
  unsigned char *buffer;
  time_t start_time;
  time_t previous_time;
  unsigned int buffer_size;
  const unsigned int read_size=((*blocksize)>65536?(*blocksize):65536);
  alloc_data_t *current_search_space;
  file_recovery_t file_recovery;
  reset_file_recovery(&file_recovery);
  file_recovery.blocksize=*blocksize;
  buffer_size=(*blocksize)+READ_SIZE;
  buffer_start=(unsigned char *)MALLOC(buffer_size);
  buffer_olddata=buffer_start;
  buffer=buffer_olddata+(*blocksize);
  start_time=time(NULL);
  previous_time=start_time;
  memset(buffer_olddata,0,(*blocksize));
  current_search_space=td_list_entry(list_search_space->list.next, alloc_data_t, list);
  if(current_search_space!=list_search_space)
    offset=current_search_space->start;
  if(verbose>0)
    info_list_search_space(list_search_space, current_search_space, disk_car->sector_size, 0, verbose);
  disk_car->pread(disk_car, buffer, READ_SIZE, offset);
  while(current_search_space!=list_search_space)
  {
    uint64_t old_offset=offset;
    {
      file_recovery_t file_recovery_new;
      if(file_recovery.file_stat!=NULL &&
          file_recovery.file_stat->file_hint->min_header_distance > 0 &&
          file_recovery.file_size<=file_recovery.file_stat->file_hint->min_header_distance)
      {
      }
      else if(file_recovery.file_stat!=NULL && file_recovery.file_stat->file_hint==&file_hint_tar &&
          header_check_tar(buffer-0x200,0x200,0,&file_recovery,&file_recovery_new))
      { /* Currently saving a tar, do not check the data for know header */
      }
      else
      {
	struct td_list_head *tmpl;
        file_recovery_new.file_stat=NULL;
	td_list_for_each(tmpl, &file_check_list.list)
	{
	  struct td_list_head *tmp;
	  const file_check_list_t *pos=td_list_entry(tmpl, file_check_list_t, list);
	  td_list_for_each(tmp, &pos->file_checks[buffer[pos->offset]].list)
	  {
	    const file_check_t *file_check=td_list_entry(tmp, file_check_t, list);
	    if((file_check->length==0 || memcmp(buffer + file_check->offset, file_check->value, file_check->length)==0) &&
		file_check->header_check(buffer, read_size, 1, &file_recovery, &file_recovery_new)!=0)
	    {
	      file_recovery_new.file_stat=file_check->file_stat;
	      break;
	    }
	  }
	  if(file_recovery_new.file_stat!=NULL)
	    break;
	}
        if(file_recovery_new.file_stat!=NULL && file_recovery_new.file_stat->file_hint!=NULL)
	{
	  /* A new file begins, backup file offset */
	  current_search_space=file_found(current_search_space, offset, file_recovery_new.file_stat);
	  (*file_nbr)++;
	  file_recovery_cpy(&file_recovery, &file_recovery_new);
	}
      }
    }
    /* Check for data EOF */
    if(file_recovery.file_stat!=NULL)
    {
      int res=1;
      if(file_recovery.data_check!=NULL)
	res=file_recovery.data_check(buffer_olddata,2*(*blocksize),&file_recovery);
      file_recovery.file_size+=*blocksize;
      file_recovery.file_size_on_disk+=*blocksize;
      if(res==2)
      {
	/* EOF found */
	reset_file_recovery(&file_recovery);
      }
    }
    /* Check for maximum filesize */
    if(file_recovery.file_stat!=NULL && file_recovery.file_stat->file_hint->max_filesize>0 && file_recovery.file_size>=file_recovery.file_stat->file_hint->max_filesize)
    {
      reset_file_recovery(&file_recovery);
    }

    if(*file_nbr>=10)
    {
      current_search_space=list_search_space;
    }
    else
      get_next_sector(list_search_space, &current_search_space,&offset,*blocksize);
    if(current_search_space==list_search_space)
    {
      /* End of disk found => EOF */
      reset_file_recovery(&file_recovery);
    }
    buffer_olddata+=*blocksize;
    buffer+=*blocksize;
    if( old_offset+*blocksize!=offset ||
        buffer+read_size>buffer_start+buffer_size)
    {
      memcpy(buffer_start,buffer_olddata,*blocksize);
      buffer_olddata=buffer_start;
      buffer=buffer_olddata+*blocksize;
      if(verbose>1)
      {
        log_verbose("Reading sector %10lu/%lu\n",(unsigned long)((offset-partition->part_offset)/disk_car->sector_size),(unsigned long)((partition->part_size-1)/disk_car->sector_size));
      }
      if(disk_car->pread(disk_car, buffer, READ_SIZE, offset) != READ_SIZE)
      {
#ifdef HAVE_NCURSES
        if(interface!=0)
        {
          wmove(stdscr,11,0);
          wclrtoeol(stdscr);
          wprintw(stdscr,"Error reading sector %10lu\n",
              (unsigned long)((offset-partition->part_offset)/disk_car->sector_size));
        }
#endif
      }
#ifdef HAVE_NCURSES
      if(interface!=0)
      {
        time_t current_time;
        current_time=time(NULL);
        if(current_time>previous_time)
        {
          previous_time=current_time;
          if(photorec_progressbar(stdscr, 0, STATUS_FIND_OFFSET, offset, disk_car, partition, *file_nbr, current_time-real_start_time, file_stats))
	  {
	    log_info("PhotoRec has been stopped\n");
	    current_search_space=list_search_space;
	  }
	}
      }
#endif
    }
  } /* end while(current_search_space!=list_search_space) */
  free(buffer_start);
  return 0;
}
