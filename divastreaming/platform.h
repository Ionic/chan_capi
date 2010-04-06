/*
 *
  Copyright (c) Dialogic (R) 2009 - 2010
 *
  This source file is supplied for the use with
  Eicon Networks range of DIVA Server Adapters.
 *
  Dialogic (R) File Revision :    1.9
 *
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.
 *
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY OF ANY KIND WHATSOEVER INCLUDING ANY
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.
 *
  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#ifndef __DIVA_OS_STREAMING_PLATFORM_H__
#define __DIVA_OS_STREAMING_PLATFORM_H__

#define DIVA_USERMODE 1

#if !defined(__i386__)
#define READ_WORD(w) ( ((byte *)(w))[0] + \
                      (((byte *)(w))[1]<<8) )

#define READ_DWORD(w) ( ((byte *)(w))[0] + \
                       (((byte *)(w))[1]<<8) + \
                       (((byte *)(w))[2]<<16) + \
                       (((byte *)(w))[3]<<24) )

#define WRITE_WORD(b,w) do{ ((byte*)(b))[0]=(byte)(w); \
                          ((byte*)(b))[1]=(byte)((w)>>8); }while(0)

#define WRITE_DWORD(b,w) do{ ((byte*)(b))[0]=(byte)(w); \
                           ((byte*)(b))[1]=(byte)((w)>>8); \
                           ((byte*)(b))[2]=(byte)((w)>>16); \
                           ((byte*)(b))[3]=(byte)((w)>>24); }while(0)
#else
#define READ_WORD(w) (*(word *)(w))
#define READ_DWORD(w) (*(dword *)(w))
#define WRITE_WORD(b,w) do{ *(word *)(b)=(w); }while(0)
#define WRITE_DWORD(b,w) do{ *(dword *)(b)=(w); }while(0)
#endif

#define	byte   unsigned char 
#define	word   unsigned short
#define	dword  unsigned long 
#define int32  signed int

#ifndef likely
#define likely(__x__)   (!!(__x__))
#endif
#ifndef unlikely
#define unlikely(__x__) (!!(__x__))
#endif

#endif

