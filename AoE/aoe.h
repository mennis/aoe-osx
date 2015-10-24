/*
 *  aoe.h
 *  AoEInterface
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __AOE_H__
#define __AOE_H__

#include <libkern/OSTypes.h>
#include <sys/types.h>

__BEGIN_DECLS
#include <net/ethernet.h>
__END_DECLS


#define CONFIG_STR_GET						0
#define CONFIG_STR_EXACT_MATCH				1
#define CONFIG_STR_PREFIX_MATCH				2
#define CONFIG_STR_SET						3
#define CONFIG_STR_FORCE_SET				4

#define AOE_ATA_COMMAND						0
#define AOE_CFG_COMMAND						1

#define AOE_ERROR_BAD_COMMAND				1
#define AOE_ERROR_BAD_ARG					2
#define AOE_ERROR_DEVICE_UNAVALABLE			3
#define AOE_ERROR_CONFIG_STR_PRESENT		4
#define AOE_ERROR_BAD_VERSION				5

#define AOE_AFLAGS_E						0x40
#define AOE_AFLAGS_D						0x10
#define AOE_AFLAGS_A						0x02
#define AOE_AFLAGS_W						0x01

#define AOE_FLAG_RESPONSE					0x8
#define AOE_FLAG_ERROR						0x4

#define AOE_SUPPORTED_VER					1

#define ETHERTYPE_AOE						0x88a2

#define MAX_CONFIG_STRING_LENGTH			1024
#define MAX_SHELFS							(0xFFFF+1)
#define MAX_SLOTS							(0xFF+1)

#define	BYTES_IN_AOE_HEADER					(sizeof(aoe_atahdr_full)+sizeof(struct ether_header))

#define SHELF_BROADCAST						0xFFFF
#define SLOT_BROADCAST						0xFF

#define MAX_RETRANSMIT_BEFORE_OFFLINE		2

#define DEFAULT_MAX_TRANSFER_SIZE			(256*1024)		// bytes = 256k = 512 segments


// For a description of PIO ATA modes, see: http://en.wikipedia.org/wiki/Programmed_input/output
// I can't see (in any open source code) how this is used besides checking ATA controller vs device compatibility
// The current AoE driver now uses DMA modes as it allows us to increase the size of the blocks we transmit
#define AOE_SUPPORTED_PIO_MODES				0x0
#define AOE_SUPPORTED_DMA_MODES				0x001F	// mode 0,1,2,3,4
#define AOE_SUPPORTED_ULTRA_DMA_MODES		0x0

// BSD macro
#define MTOD(m, t)						((t)mbuf_data(m))

// These structures and macros are a little ugly, but they are fast and portable.
// All data in the structures is handled as UInt16 as the byte alignment may vary across processors
// We therefore warn against any compilation for different processors.

#if TARGET_RT_BIG_ENDIAN
#warning check structures for this different endianness
#endif

//----------------------------------------------------------------------------------------
//										aoe_header
//----------------------------------------------------------------------------------------


#define AOE_HEADER_GETVER(p)		((ntohs((p)->ah_verflagserr)>>12)&0x0F)
#define AOE_HEADER_GETFLAG(p)		((ntohs((p)->ah_verflagserr)>>8)&0x0F)
#define AOE_HEADER_GETERR(p)		((ntohs((p)->ah_verflagserr)>>0)&0xFF)

#define AOE_HEADER_GETMAJOR(p)		ntohs((p)->ah_major)
#define AOE_HEADER_GETMINOR(p)		((ntohs((p)->ah_minorcmd)>>8)&0xFF)
#define AOE_HEADER_GETCMD(p)		((ntohs((p)->ah_minorcmd)>>0)&0xFF)
#define AOE_HEADER_GETTAG(p)		(\
										(((UInt32)ntohs( (p)->ah_tag[0] ))<<16)\
										| ntohs((p)->ah_tag[1])\
									)

#define AOE_HEADER_CLEAR(p)			{\
										(p)->ah_verflagserr = 0;\
										(p)->ah_major = 0;\
										(p)->ah_minorcmd = 0;\
										(p)->ah_tag[0] = 0;\
										(p)->ah_tag[1] = 0;\
									}
#define AOE_HEADER_SETVERFLAGERR(v,f,e)	htons((((v)&0xF)<<12)|(((f)&0xF)<<8)|((e)&0xF))
#define AOE_HEADER_SETMAJOR(m)			htons(m)
#define AOE_HEADER_SETMINORCMD(m,c)		htons((((m)&0xFF)<<8)|(((c)&0xFF)<<0))
#define AOE_HEADER_SETTAG1(t)			htons(((t)>>16)&0xFFFF)
#define AOE_HEADER_SETTAG2(t)			htons((t)&0xFFFF)

typedef struct _aoe_header
{
	UInt16 ah_verflagserr;
	
	UInt16 ah_major;
	UInt16 ah_minorcmd;
	UInt16 ah_tag[2];
} aoe_header;

//----------------------------------------------------------------------------------------
//										aoe_cfghdr
//----------------------------------------------------------------------------------------

#define AOE_CFGHEADER_GETBCOUNT(p)		ntohs((p)->ac_bufcnt)
#define AOE_CFGHEADER_GETFVERSION(p)	ntohs((p)->ac_fwver)
#define AOE_CFGHEADER_GETSCOUNT(p)		((ntohs((p)->ac_scnt_aoe_ccmd)>>8)&0xFF)
#define AOE_CFGHEADER_GETAOEVER(p)		((ntohs((p)->ac_scnt_aoe_ccmd)>>4)&0x0F)
#define AOE_CFGHEADER_GETCCMD(p)		((ntohs((p)->ac_scnt_aoe_ccmd)>>0)&0x0F)
	
#define AOE_CFGHEADER_GETCSLEN(p)		ntohs((p)->ac_cslen)
	
#define AOE_CFGHEADER_CLEAR(p)			{\
											(p)->ac_bufcnt = 0;\
											(p)->ac_fwver = 0;\
											(p)->ac_scnt_aoe_ccmd = 0;\
											(p)->ac_cslen = 0;\
										}
//#define AOE_HEADER_SETBUFCNT(b)			htons(b)		// Spec requires this to be zero so disable interface
//#define AOE_HEADER_SETFIRMWARE(f)			htons(f)		// Spec requires this to be zero so disable interface
#define AOE_HEADER_SETSECTOR_CMD(s,c)		htons((((s)&0xFF)<<8)|(((0)&0xF)<<4)|((c)&0xF))		// AoE field is set to zero
#define AOE_HEADER_SETCSTRLEN(c)			htons(c)

typedef struct _aoe_cfghdr_rd
{
	UInt16 ac_bufcnt;
	UInt16 ac_fwver;
	UInt16 ac_scnt_aoe_ccmd;
	UInt16 ac_cslen;
	UInt8 ac_cstring[1];		// Config string can be read from here
} aoe_cfghdr_rd;

typedef struct _aoe_cfghdr
{
	UInt16 ac_bufcnt;
	UInt16 ac_fwver;
	UInt16 ac_scnt_aoe_ccmd;
	UInt16 ac_cslen;
} aoe_cfghdr;

// Sometimes it's useful to have the full header available as well
typedef struct _aoe_cfghdr_rd_full
{
	aoe_header aoe;
	aoe_cfghdr_rd cfg;
} aoe_cfghdr_rd_full;

typedef struct _aoe_cfghdr_full
{
	aoe_header aoe;
	aoe_cfghdr cfg;
} aoe_cfghdr_full;
	
//----------------------------------------------------------------------------------------
//										aoe_atahdr
//----------------------------------------------------------------------------------------

// It's quicker to access this data as 32 bit
#define AOE_ATAHEADER_CLEAR(p)			{\
											UInt32 *p32 = (UInt32*) p;\
											p32[0]=0;\
											p32[1]=0;\
											p32[2]=0;\
										}

#define AOE_ATAHEADER_SETAFLAGSFEAT(a,b)	htons((((a)&0xFF)<<8)|(((b)&0xFF)<<0))
#define AOE_ATAHEADER_SETSCNTCMD(a,b)		htons((((a)&0xFF)<<8)|(((b)&0xFF)<<0))
#define AOE_ATAHEADER_SETLBA01(a,b)			htons((((a)&0xFF)<<8)|(((b)&0xFF)<<0))
#define AOE_ATAHEADER_SETLBA23(a,b)			htons((((a)&0xFF)<<8)|(((b)&0xFF)<<0))
#define AOE_ATAHEADER_SETLBA45(a,b)			htons((((a)&0xFF)<<8)|(((b)&0xFF)<<0))

#define AOE_ATAHEADER_GETAFLAGS(p)		((ntohs((p)->aa_aflags_errfeat)>>8)&0xFF)
#define AOE_ATAHEADER_GETERR(p)			((ntohs((p)->aa_aflags_errfeat)>>0)&0xFF)
#define AOE_ATAHEADER_GETSCNT(p)		((ntohs((p)->aa_scnt_cmdstat)>>8)&0xFF)
#define AOE_ATAHEADER_GETSTAT(p)		((ntohs((p)->aa_scnt_cmdstat)>>0)&0xFF)
#define AOE_ATAHEADER_GETLBA0(p)		((ntohs((p)->aa_lba0_1)>>8)&0xFF)
#define AOE_ATAHEADER_GETLBA1(p)		((ntohs((p)->aa_lba0_1)>>0)&0xFF)
#define AOE_ATAHEADER_GETLBA2(p)		((ntohs((p)->aa_lba2_3)>>8)&0xFF)
#define AOE_ATAHEADER_GETLBA3(p)		((ntohs((p)->aa_lba2_3)>>0)&0xFF)
#define AOE_ATAHEADER_GETLBA4(p)		((ntohs((p)->aa_lba4_5)>>8)&0xFF)
#define AOE_ATAHEADER_GETLBA5(p)		((ntohs((p)->aa_lba4_5)>>0)&0xFF)

typedef struct _aoe_atahdr
{
	UInt16 aa_aflags_errfeat;
	UInt16 aa_scnt_cmdstat;
	UInt16 aa_lba0_1;
	UInt16 aa_lba2_3;
	UInt16 aa_lba4_5;
	UInt16 aa_reserved;
} aoe_atahdr;

typedef struct _aoe_atahdr_rd
{
	UInt16 aa_aflags_errfeat;
	UInt16 aa_scnt_cmdstat;
	UInt16 aa_lba0_1;
	UInt16 aa_lba2_3;
	UInt16 aa_lba4_5;
	UInt16 aa_reserved;
	UInt16 aa_Data[1];		// Data can be read from here
} aoe_atahdr_rd;

//----------------------------------------------------------------------------------------
//										other headers
//----------------------------------------------------------------------------------------
	
// Full AoE header available as well
typedef struct _aoe_atahdr_full
{
	aoe_header aoe;
	aoe_atahdr ata;
} aoe_atahdr_full;

// Full AoE header available as well
typedef struct _aoe_atahdr_rd_full
{
	aoe_header aoe;
	aoe_atahdr_rd ata;
} aoe_atahdr_rd_full;

// For outgoing packets, we'll have the ethernet header appearing first
typedef struct _eth_aoe_header
{
	struct ether_header eth;
	aoe_header			aoe;
} eth_aoe_header;
	



#endif	//__AOE_H__
