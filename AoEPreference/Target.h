//
//  Target.h
//  AoEPreference
//
//  Copyright Brantley Coile Company 2008. All rights reserved.
//

#import <Cocoa/Cocoa.h>
#include "AoEcommon.h"

@interface Target : NSObject
{
@public
	NSNumber*	m_pShelf;
	NSNumber*	m_pSlot;
	NSNumber*	m_pClaimed;
	NSNumber*	m_pAvailable;
	NSNumber*	m_pCapacity;
	NSString*	m_pCapacityString;
	NSString*	m_pConfigString;
	NSString*	m_pOwnerString;
	NSString*	m_pStatus;
	NSString*	m_pInterfaces;
	
	bool		m_fClaimed;
	bool		m_fOnline;
	int			m_nTargetNumber;
	int			m_nInterfaces;
	int			m_anENInterfaces[MAX_SUPPORTED_ETHERNET_CONNETIONS];
}

- (id)initWithData:(int)nShelf Slot:(int)nSlot Number:(int)nNumber Capacity:(int)fpCapacity ConfigString:(NSString*)pCString NumInterfaces:(int)nInterfaces Interfaces:(int*)anENInterfaces;

-(bool) connected_to_interface:(int)nInterface;
-(bool) claimed_by_us;
@end
