//
//  WindowController.h
//  AoECommander
//
//  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
//

#ifndef __WINDOW_CONTROLLER_H__
#define __WINDOW_CONTROLLER_H__

#import <Cocoa/Cocoa.h>
#include "AoEDriverInterface.h"
#include "AoEInterfaceCommands.h"
#include "AoEcommon.h"

class AoEDriverInterface;

@interface WindowController : NSObject
{
	// AoE Header
	IBOutlet NSTextField*	m_pAoEVer;
	IBOutlet NSTextField*	m_pAoEFlags;
	IBOutlet NSTextField*	m_pAoEError;
	IBOutlet NSTextField*	m_pAoEShelf;
	IBOutlet NSTextField*	m_pAoESlot;
	IBOutlet NSTextField*	m_pAoETag;
	IBOutlet NSPopUpButton*	m_pAoECommand;

	// ATA Command
	IBOutlet NSTextField*	m_pATAErr_Feature;
	IBOutlet NSTextField*	m_pATASector_Count;
	IBOutlet NSTextField*	m_pATACmd_Status;
	IBOutlet NSTextField*	m_pATAlba0;
	IBOutlet NSTextField*	m_pATAlba1;
	IBOutlet NSTextField*	m_pATAlba2;
	IBOutlet NSTextField*	m_pATAlba3;
	IBOutlet NSTextField*	m_pATAlba4;
	IBOutlet NSTextField*	m_pATAlba5;
	IBOutlet NSTextField*	m_pATAData;

	// Query Config
	IBOutlet NSTextField*	m_pQCConfigStringLength;
	IBOutlet NSTextField*	m_pQCConfigString;
	IBOutlet NSPopUpButton*	m_pQCCCmd;

	IBOutlet NSButton*		m_pSend;
	
	// Others
	IBOutlet NSButton*		m_p48LBA;
	IBOutlet NSButton*		m_pWrite;
	IBOutlet NSButton*		m_pAsync;
	
	// Additional Data
	ForcePacketInfo			m_OutputData;

	AoEDriverInterface*		m_pDriver;
}

// Actions
-(IBAction)Send:(id)aSender;
-(IBAction)Command:(id)aSender;
-(IBAction)Logging:(id)aSender;

// 
-(void)UpdateEnabled;
@end

#endif		//__WINDOW_CONTROLLER_H__
