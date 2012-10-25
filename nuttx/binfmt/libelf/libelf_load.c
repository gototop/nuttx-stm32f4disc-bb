/****************************************************************************
 * binfmt/libelf/libelf_load.c
 *
 *   Copyright (C) 2012 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <elf.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/binfmt/elf.h>

/****************************************************************************
 * Pre-Processor Definitions
 ****************************************************************************/

#define ELF_ALIGN_MASK   ((1 << CONFIG_ELF_ALIGN_LOG2) - 1)
#define ELF_ALIGNUP(a)   (((unsigned long)(a) + ELF_ALIGN_MASK) & ~ELF_ALIGN_MASK)
#define ELF_ALIGNDOWN(a) ((unsigned long)(a) & ~ELF_ALIGN_MASK)


#ifndef MAX
#define MAX(x,y) ((x) > (y) ? (x) : (y))
#endif

/****************************************************************************
 * Private Constant Data
 ****************************************************************************/

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: elf_filelen
 *
 * Description:
 *  Get the size of the ELF file
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

static inline int elf_filelen(FAR struct elf_loadinfo_s *loadinfo)
{
  struct stat buf;
  int ret;

  /* Get the file stats */

  ret = fstat(loadinfo->filfd, &buf);
  if (ret < 0)
    {
      int errval = errno;
      bdbg("Failed to fstat file: %d\n", errval);
      return -errval;
    }

  /* Verify that it is a regular file */

  if (!S_ISREG(buf.st_mode))
    {
      bdbg("Not a regular file.  mode: %d\n", buf.st_mode);
      return -errval;
    }

  /* TODO:  Verify that the file is readable */

  /* Return the size of the file in the loadinfo structure */

  loadinfo->filelen = buf.st_size;
  return OK;
}

/****************************************************************************
 * Name: elf_readfile
 *
 * Description:
 *   Allocate memory for the file and read the section data into the
 *   allocated memory.
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

static int elf_readfile(FAR struct elf_loadinfo_s *loadinfo, FAR void *buffer,
                        off_t offset, size_t nbytes)
{
  FAR uint8_t *buffer;
  ssize_t bytesread;
  off_t result;

  /* Seek to the start of the section header table */

  result = lseek(loadinfo->filfd, offset, SEEK_SET);
  if (result == (off_t)-1)
    {
      int errval = errno;
      bdbg("Seel to %ld failed: %d\n", (long)offset, errval);
      return -errval;
    }

  /* Now load the file data into memory */

  buffer = (FAR uint8_t *)loadinfo->shdrs;
  while (shdrsize > 0)
    {
      bytesread = read(loadinfo->filfd, buffer, shdrsize);
      if (bytes < 0)
        {
          int errval = errno;

          /* EINTR just means that we received a signal */

          if (errno != EINTR)
            {
              bdbg("read() failed: %d\n", errval);
              return -errval;
            }
        }
      else if (bytes == 0)
        {
          bdbg("Unexpected end of file\n");
          return -ENODATA;
        }
      else
        {
          buffer   += bytesread;
          shdrsize -= bytesread;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: elf_loadshdrs
 *
 * Description:
 *   Loads section headers into memory.
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

static inline int elf_loadshdrs(FAR struct elf_loadinfo_s *loadinfo)
{
  size_t shdrsize;
  int ret;

  DEBUGASSERT(loadinfo->shdrs == NULL);

  /* Verify that there are sections */

  if (loadinfo->e_shum < 1)
    {
      bdbg("No section(?)\n");
      return -EINVAL;
    }

  /* Get the total size of the section header table */

  shdrsize = (size_t)loadinfo->ehdr.e_shentsize * (size_t)loadinfo->e_shum;
  if(loadinfo->e_shoff + shdrsize > loadinfo->filelen)
    {
      bdbg("Insufficent space in file for section header table\n");
      return -ESPIPE;
    }

  /* Allocate memory to hold a working copy of the sector header table */

  loadinfo->shdrs = (FAR Elf32_Shdr*)kmalloc(shdrsize);
  if (!loadinfo->shdrs)
    {
      bdbg("Failed to allocate the section header table. Size: %ld\n", (long)shdrsize);
      return -ENOMEM;
    }

  /* Read the section header table into memory */

  ret = elf_readfile(loadinfo, loadinfo->shdrs, loadinfo->e_shoff, shdrsize);
  if (ret < 0)
    {
      bdbg("Failed to read section header table: %d\n", ret);
      goto errout_with_alloc;
    }

  return OK;

errout_with_alloc:
  kfree(loadinfo->shdrs);
  loadinfo->shdrs = 0;
  return ret;
}

/****************************************************************************
 * Name: elf_allocsize
 *
 * Description:
 *   Calculate total memory allocation for the ELF file.
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

static void elf_allocsize(struct load_info *loadinfo)
{
  size_t allocsize;
  int i;

  /* Accumulate the size each section into memory that is marked SHF_ALLOC */

  allocsize = 0;
  for (i = 0; i < loadinfo->ehdr.e_shnum; i++)
    {
      FAR Elf32_Shdr *shdr = &loadinfo->shdrs[i];

      /* SHF_ALLOC indicates that the section requires memory during
       * execution.
       */

      if ((shdr->sh_flags & SHF_ALLOC) != 0)
        {
          allocsize += ELF_ALIGNUP(shdr->sh_size);
        }
    }

  /* Save the allocation size */

  loadinfo->allocize = allocsize;
}

/****************************************************************************
 * Name: elf_loadfile
 *
 * Description:
 *   Allocate memory for the file and read the section data into the
 *   allocated memory.
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

static inline int elf_loadfile(FAR struct load_info *loadinfo)
{
  FAR uint8_t *dest;
  int i;

  /* Allocate (and zero) memory for the ELF file. */
  
  loadinfo->alloc = kzalloc(loadinfo->allocsize);
  if (!loadinfo->alloc)
    {
      return -ENOMEM;
    }

  /* Read each section into memory that is marked SHF_ALLOC + SHT_NOBITS */

  bvdbg("Loaded sections:\n");
  dest = (FAR uint8_t*)loadinfo->alloc;

  for (i = 0; i < loadinfo->ehdr.e_shnum; i++)
    {
      FAR Elf32_Shdr *shdr = &loadinfo->shdrs[i];

      /* SHF_ALLOC indicates that the section requires memory during
       * execution */

      if ((shdr->sh_flags & SHF_ALLOC) == 0)
        {
          continue;
        }

      /* SHT_NOBITS indicates that there is no data in the file for the
       * section.
       */

      if (shdr->sh_type != SHT_NOBITS)
        {
          /* Read the section data from sh_offset to dest */

          ret = elf_readfile(loadinfo, dest, shdr->sh_offset, shdr->sh_size);
          if (ret < 0)
            {
              bdbg("Failed to read section %d: %d\n", i, ret);
              goto errout_with_alloc;
            }
        }

      /* Update sh_addr to point to copy in image. */

      shdr->sh_addr = (uintptr_t)dest;
      bvdbg("%d. 0x%lx %s\n", (long)shdr->sh_addr, loadinfo->secstrings + shdr->sh_name);

      /* Setup the memory pointer for the next time through the loop */

      dest += ELF_ALIGNUP(shdr->sh_size);
    }

  return OK;

errout_with_alloc:
  kfree(loadinfo->alloc);
  loadinfo->alloc = 0;
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: elf_load
 *
 * Description:
 *   Loads the binary into memory, allocating memory, performing relocations
 *   and inializing the data and bss segments.
 *
 * Returned Value:
 *   0 (OK) is returned on success and a negated errno is returned on
 *   failure.
 *
 ****************************************************************************/

int elf_load(FAR struct elf_loadinfo_s *loadinfo)
{
# warning "Missing logic"
  return -ENOSYS;
}
