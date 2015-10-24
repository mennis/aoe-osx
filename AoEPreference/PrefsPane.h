/*
 *  PrefsPane.h
 *  AoEPreference
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#ifndef __PREFS_PANE_H__
#define __PREFS_PANE_H__

#import <SecurityInterface/SFAuthorizationView.h>
#import <PreferencePanes/PreferencePanes.h>
#import "AboutWindController.h"
#include "AoEcommon.h"

#if MAX_SUPPORTED_ETHERNET_CONNETIONS!=6
#error Preference panel assumes MAX_SUPPORTED_ETHERNET_CONNETIONS==6
// Note: If this changes, you'll have to increase the number of buttons to select the additional ports
#endif

#define kShelf				@"Shelf"
#define kSlot				@"Slot"
#define kCapacityString		@"CapacityString"
#define kCapacity			@"Capacity"
#define kConfigString		@"Config"
#define kOwner				@"Owner"
#define kStatus				@"Status"
#define kInterfaces			@"Interfaces"

#define kClaimed			@"Claimed"
#define kOnline				@"Online"
#define kTargetNum			@"TargetNumber"
#define kInterfacesString	@"InterfacesString"

@interface PrefsPane : NSPreferencePane 
{	
	IBOutlet NSView*				mainView_10_4_or_earlier;
	IBOutlet SFAuthorizationView*	m_pAuthView;
	IBOutlet NSTableView*			m_pTableView;
	IBOutlet NSButton*				m_pEN0Button;
	IBOutlet NSButton*				m_pEN1Button;
	IBOutlet NSButton*				m_pEN2Button;
	IBOutlet NSButton*				m_pEN3Button;
	IBOutlet NSButton*				m_pEN4Button;
	IBOutlet NSButton*				m_pEN5Button;
	IBOutlet NSButton*				m_apENButtons[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	IBOutlet AboutWindController*	aboutWind;

	IBOutlet NSButton*				m_pDiscoverButton;
	IBOutlet NSButton*				m_pClaimedButton;
	
	NSMutableArray*					m_pTargets;
	
	int								m_nNumOfEthernetPorts;
	bool							m_fDriverLoaded;
	
	AoEPreferencesStruct			m_PrefStruct;
}

- (void)mainViewDidLoad;

-(NSDictionary*)create_target:(int)nShelf Slot:(int)nSlot Number:(int)nNumber Capacity:(UInt64)unCapacity ConfigString:(NSString*)pCString NumInterfaces:(int)nInterfaces Interfaces:(int*)anENInterfaces;
-(bool) target_connected_to_interface:(NSDictionary*)pTarget Interface:(int)nInterface;
-(bool) target_claimed_by_us:(NSDictionary*)pTarget;

-(void) ResortTable;

-(int) authorised:(bool)AskIfNot;

- (int)set_en_interfaces;
- (bool)confirm_en_press:(int)nInt;
-(void) handle_en_press:(int)nEN;

- (IBAction)aboutDoneAction:(id)sender;

- (IBAction)En0Pressed:(id)sender;
- (IBAction)En1Pressed:(id)sender;
- (IBAction)En2Pressed:(id)sender;
- (IBAction)En3Pressed:(id)sender;
- (IBAction)En4Pressed:(id)sender;
- (IBAction)En5Pressed:(id)sender;

-(int) send_to_aoed:(char**)apsz;
-(void) sortWithDescriptor:(id)descriptor;

- (IBAction)DiscoverPressed:(id)sender;
- (IBAction)ClaimedPressed:(id)sender;

- (void)update_target_listings:(NSTimer*)theTimer;
- (void)update_buttons:(bool)fHaveAuth;

// Authorization handling
- (void)authorizationViewDidAuthorize:(SFAuthorizationView *)view;
- (void)authorizationViewDidDeauthorize:(SFAuthorizationView *)view;

-(void) update_table;

#endif		//__PREFS_PANE_H__
@end
