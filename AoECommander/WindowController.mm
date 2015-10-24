//
//  WindowController.m
//  AoECommander
//
//  Copyright 2008 Brantley Coile Company. All rights reserved.
//

#include <unistd.h>
#import "WindowController.h"
#include "AoEDriverInterface.h"
#include "aoe.h"

@implementation WindowController

- (void)awakeFromNib
{
	[self UpdateEnabled];
	
	#if 1
	if ( 0 != geteuid() )
	{
		NSRunAlertPanel(@"Program will not work without root privelegas", @"",@"EXIT",@"",@"");
		[NSApp terminate:self];
	}
	#endif

	m_pDriver = new AoEDriverInterface;
	m_pDriver->connect_to_driver();
}

-(void)applicationWillTerminate:(NSNotification *)aNotification
{
	if ( m_pDriver )
		m_pDriver->disconnect();
	delete m_pDriver;
	m_pDriver = NULL;
}

-(void)UpdateEnabled
{
	bool fATA = 0==[m_pAoECommand indexOfSelectedItem];

	[m_p48LBA setEnabled:fATA];
	[m_pWrite setEnabled:fATA];
	[m_pAsync setEnabled:fATA];
	[m_pATAErr_Feature setEnabled:fATA];
	[m_pATASector_Count setEnabled:fATA];
	[m_pATACmd_Status setEnabled:fATA];
	[m_pATAlba0 setEnabled:fATA];
	[m_pATAlba1 setEnabled:fATA];
	[m_pATAlba2 setEnabled:fATA];
	[m_pATAlba3 setEnabled:fATA];
	[m_pATAlba4 setEnabled:fATA];
	[m_pATAlba5 setEnabled:fATA];
	[m_pATAData setEnabled:fATA];
	
	// Query Config
	[m_pQCConfigStringLength setEnabled:!fATA];
	[m_pQCConfigString setEnabled:!fATA];
	[m_pQCCCmd setEnabled:!fATA];
}

#define SHIFTER(DATA, SHIFT)		((DATA)<<(SHIFT))

-(void)FormatOutput
{
	bool fATA = 0==[m_pAoECommand indexOfSelectedItem];

	// Get all our data
	UInt32 nAoEVer = [m_pAoEVer intValue]&0xF;
	UInt32 nAoEFlags = [m_pAoEFlags intValue]&0xF;
	UInt32 nAoEError = [m_pAoEError intValue]&0xFF;
	UInt32 nAoEShelf = [m_pAoEShelf intValue]&0xFFFF;
	UInt32 nAoESlot = [m_pAoESlot intValue]&0xFF;
	UInt32 nAoETag = [m_pAoETag intValue]&0xFFFFFFFF;
	UInt32 nAoECommand = [m_pAoECommand indexOfSelectedItem]&0xFF;
	
	m_OutputData.nShelf = [m_pAoEShelf intValue]&0xFFFF;
	m_OutputData.nSlot = [m_pAoESlot intValue]&0xFF;
	
	// ATA Command
	UInt32 nATAAFlags = ([m_p48LBA state]<<6)|([m_pAsync state]<<1)|([m_pWrite state]<<0);
	UInt32 nATAErr_Feature = [m_pATAErr_Feature intValue]&0xFF;
	UInt32 nATASector_Count = [m_pATASector_Count intValue]&0xFF;
	UInt32 nATACmd_Status = [m_pATACmd_Status intValue]&0xFF;
	UInt32 nATAlba0 = [m_pATAlba0 intValue]&0xFF;
	UInt32 nATAlba1 = [m_pATAlba1 intValue]&0xFF;
	UInt32 nATAlba2 = [m_pATAlba2 intValue]&0xFF;
	UInt32 nATAlba3 = [m_pATAlba3 intValue]&0xFF;
	UInt32 nATAlba4 = [m_pATAlba4 intValue]&0xFF;
	UInt32 nATAlba5 = [m_pATAlba5 intValue]&0xFF;
	
	// Query Config
	UInt32 nQCConfigStringLength = [m_pQCConfigStringLength intValue]&0xFFFF;
//	UInt32 nQCConfigString = [m_pQCConfigString intValue]&0xFFFF;
	UInt32 nQCCCmd = [m_pQCCCmd indexOfSelectedItem]&0xF;

	
	// Clear our structs
	AOE_HEADER_CLEAR(&m_OutputData.AoEhdr);
	AOE_CFGHEADER_CLEAR(&m_OutputData.CFGhdr);
	AOE_ATAHEADER_CLEAR(&m_OutputData.ATAhdr);

	// Start to fill our data
	m_OutputData.Tag = TAG_USER_MASK + nAoETag;
	m_OutputData.fATA = fATA;
	
	m_OutputData.AoEhdr.ah_verflagserr = AOE_HEADER_SETVERFLAGERR(nAoEVer, nAoEFlags, nAoEError);
	m_OutputData.AoEhdr.ah_major = AOE_HEADER_SETMAJOR(nAoEShelf);
	m_OutputData.AoEhdr.ah_minorcmd = AOE_HEADER_SETMINORCMD(nAoESlot, nAoECommand);
	m_OutputData.AoEhdr.ah_tag[0] = AOE_HEADER_SETTAG1(m_OutputData.Tag);
	m_OutputData.AoEhdr.ah_tag[1] = AOE_HEADER_SETTAG2(m_OutputData.Tag);

	if ( fATA )
	{
		m_OutputData.ATAhdr.aa_aflags_errfeat =  AOE_ATAHEADER_SETAFLAGSFEAT(nATAAFlags, nATAErr_Feature);
		m_OutputData.ATAhdr.aa_scnt_cmdstat = AOE_ATAHEADER_SETSCNTCMD(nATASector_Count, nATACmd_Status);
		m_OutputData.ATAhdr.aa_lba0_1 = AOE_ATAHEADER_SETLBA01(nATAlba0, nATAlba1);
		m_OutputData.ATAhdr.aa_lba2_3 = AOE_ATAHEADER_SETLBA23(nATAlba2, nATAlba3);
		m_OutputData.ATAhdr.aa_lba4_5 = AOE_ATAHEADER_SETLBA45(nATAlba4, nATAlba5);
	}
	else
	{
		m_OutputData.CFGhdr.ac_scnt_aoe_ccmd = AOE_HEADER_SETSECTOR_CMD(2, nQCCCmd);
		m_OutputData.CFGhdr.ac_cslen = AOE_HEADER_SETCSTRLEN(nQCConfigStringLength);
//		m_OutputData.CFGhdr.ac_cstring = AOE_HEADER_SETCSTR(nQCConfigString);
	}
}

-(IBAction)Send:(id)aSender
{
	[self FormatOutput];
	[m_pAoETag setIntValue:MIN([m_pAoETag intValue]+1, MAX_TAG)];

	m_pDriver->force_packet_send(&m_OutputData);
}

-(IBAction)Logging:(id)aSender
{
	int nLogging = 1;
	m_pDriver->enable_logging(&nLogging);
}

-(IBAction)Command:(id)aSender
{
	[self UpdateEnabled];
}

@end
