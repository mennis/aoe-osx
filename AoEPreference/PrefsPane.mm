/*
 *  PrefsPane.mm
 *  AoEPreference
 *
 *  Copyright 2008 Brantley Coile Company. All rights reserved.
 *
 */

#import "PrefsPane.h"
#include "PreferenceLoadSave.h"
#include "AoEProperties.h"
#include "EthernetDetect.h"
#include "debug.h"

__BEGIN_DECLS
#include "ConfigString.h"
__END_DECLS

#define SAFE_DICT_ADD(x)			((x==NULL) ? (id)[NSNull null] : (x))

//#define WARN_ON_EXIT

// We hard code the path to AoEd here...
#ifdef DEBUG
const char* g_pszPath = "/Users/mennis/src/c/aoe-osx-aoed/build/Debug/AoEd";
#else
const char* g_pszPath = "/System/Library/Extensions/AoE.kext/Contents/Resources/AoEd";
#endif

#define TARGET_UPDATE_DELAY		1.0f		//s

@implementation PrefsPane

// view tags of our prefs UI (determined in the nib)
enum
{
	kEditControlTag		= 1,
	kWarningCheckTag	= 2,
	kAboutButtonTag		= 3
};


/*---------------------------------------------------------------------------------
 *  The preference pane is being initialized, remember our bundle ID for later.
 ---------------------------------------------------------------------------------*/

-(id)initWithBundle:(NSBundle*)bundle
{
	if ((self = [super initWithBundle:bundle]) != nil)
	{
		// Recall Preference settings
		PreferenceLoadSave Prefs;
		Prefs.recall_settings(&m_PrefStruct);
	}
	
	return self;
}


/*---------------------------------------------------------------------------------
 *  Set the main view of the preference pane before it returns.
 ---------------------------------------------------------------------------------*/

- (void)assignMainView
{
	EthernetDetect eth;
	AoEProperties AoEDriver;
	
	[self setMainView: mainView_10_4_or_earlier];

	// Setup our authorization handling
	[m_pAuthView setString:"AoE preference settings"];
	[m_pAuthView updateStatus:self];
	[m_pAuthView setAutoupdate:YES];
	[m_pAuthView setDelegate:self];

	[m_pTableView setDelegate:self];

	m_nNumOfEthernetPorts = eth.GetNumberOfInterfaces();
	m_fDriverLoaded = (0==AoEDriver.configure_matching());

	m_apENButtons[0] = m_pEN0Button;
	m_apENButtons[1] = m_pEN1Button;
	m_apENButtons[2] = m_pEN2Button;
	m_apENButtons[3] = m_pEN3Button;
	m_apENButtons[4] = m_pEN4Button;
	m_apENButtons[5] = m_pEN5Button;
	
	[self update_buttons:NO];
	
	[self update_target_listings:NULL];
}


/*---------------------------------------------------------------------------------
 *  mainViewDidLoad is invoked by the default implementation of loadMainView
 *  after the main nib file has been loaded and the main view of the preference
 *  pane has been set.  The default implementation does nothing.  Override
 *  this method to perform any setup that must be performed after the main
 *  nib file has been loaded and the main view has been set.
  ---------------------------------------------------------------------------------*/

- (void)mainViewDidLoad
{
	AoEProperties AoEDriver;
	EthernetDetect eth;
	int n, nValue;
	NSUserDefaults* standardUserDefaults;
	
	standardUserDefaults = [NSUserDefaults standardUserDefaults];

	if ( standardUserDefaults )
	{
		// Setup our ethernet BSD names
		char acEthernetName[100];

		// Show correct BSD name and hide control if it doesn't exist
		for(n=0; n<m_nNumOfEthernetPorts; n++)
		{
			eth.GetInterfaceName(n, acEthernetName, sizeof(acEthernetName));
			[m_apENButtons[n] setStringValue:[NSString stringWithCString:acEthernetName encoding: NSASCIIStringEncoding]];
			[m_apENButtons[n] setState:FALSE];
		}
		
		for(/*use old value of n*/; n<numberof(m_apENButtons); n++)
			[m_apENButtons[n] setHidden:TRUE];
	}
	
	// Restore ethernet interfaces in preference pane
	if ( 0==AoEDriver.configure_matching() )
		for(n=0; n<m_nNumOfEthernetPorts; n++)
			if ( 0==AoEDriver.get_en_interfaces(n, &nValue) )
				[m_apENButtons[nValue] setState:TRUE];
	
	for (n=0; n<[m_pTableView numberOfColumns]; n++)
	{
		NSTableColumn* tableColumn = [[m_pTableView tableColumns] objectAtIndex:n];
		
		// Change the image and begin the sort
		if ( [[tableColumn identifier] isEqualToString:kShelf] )
			[m_pTableView setIndicatorImage:[NSImage imageNamed:@"NSAscendingSortIndicator"] inTableColumn:tableColumn];  
	}
}

-(void) dealloc
{
	[m_pTargets release];

    [super dealloc];
}



/*---------------------------------------------------------------------------------
 *	"about" sheet about to be dismissed. This is used as a warning 
 ---------------------------------------------------------------------------------*/
- (void)aboutSheetDidEnd:(NSWindow*)sheet returnCode:(int)returnCode contextInfo:(void *)contextInfo
{
	[sheet orderOut:self];
}


/*---------------------------------------------------------------------------------
 *	The user dismissed the about window sheet.
 ---------------------------------------------------------------------------------*/

- (IBAction)aboutDoneAction:(id)sender
{
	[NSApp endSheet:[aboutWind window]];
}

#pragma mark --
#pragma mark - delegate methods

- (void)willSelect
{
//	debugVerbose("Our prefs panel is about to be selected.\n");
}

- (void)didSelect
{
//	debugVerbose("Our prefs panel is selected.\n");
}

- (void)willUnselect
{
//	debugVerbose("Our prefs panel is about to be un-selected.\n");
}

- (void)didUnselect
{
//	debugVerbose("Our prefs panel is now un-selected.\n");
}


/*---------------------------------------------------------------------------------
 *	Last chance before exiting
 ---------------------------------------------------------------------------------*/

- (void)confirmSheetDidEnd:(NSWindow*)sheet returnCode:(int)returnCode contextInfo:(void *)contextInfo
{
	[sheet orderOut:self];	// hide the sheet

	// decide how we want to unselect
	if (returnCode == NSAlertDefaultReturn)
		[self replyToShouldUnselect:NSUnselectNow];
	else
		[self replyToShouldUnselect:NSUnselectCancel];
}

/*---------------------------------------------------------------------------------
 *	shouldUnselect:
 *
 *	Delegate method to possibly block the unselection of this prefs pane.
 ---------------------------------------------------------------------------------*/

- (NSPreferencePaneUnselectReply)shouldUnselect
{
	NSPreferencePaneUnselectReply result = NSUnselectNow;

#ifdef WARN_ON_EXIT
	// normally we return "NSUnslectNow", but since we are opening a sheet,
	// we need to return "NSUnselectLater" and have our "confirmSheetDidEnd" selector perform the dismissal decision
	//
	NSBeginAlertSheet(	@"You are about to exit this preference pane.",
						@"Yes",
						@"No",
						nil,
						[[self mainView] window],
						self,
						nil,
						@selector(confirmSheetDidEnd:returnCode:contextInfo:),
						nil,
						@"Are you sure you want to exit?");
	result = NSUnselectLater;
#endif
	
	return result;
}

#pragma mark --
#pragma mark - Authorization handling

- (void)authorizationViewDidAuthorize:(SFAuthorizationView *)view
{
	if ( !m_fDriverLoaded )
		[NSApp beginSheet:[aboutWind window] modalForWindow:[[self mainView] window] modalDelegate:self didEndSelector:@selector(aboutSheetDidEnd:returnCode:contextInfo:) contextInfo:nil];
	
	[self update_buttons:YES];
}

- (void)authorizationViewDidDeauthorize:(SFAuthorizationView *)view;
{
	[self update_buttons:NO];
}


#pragma mark --
#pragma mark Button handling

-(void) handle_en_press:(int)nEN
{
	if ( ![m_apENButtons[nEN] state] && ![self confirm_en_press:nEN])
	{
		[m_apENButtons[nEN] setState:1];
		return;
	}
	
	[self set_en_interfaces];
	[self update_buttons:YES];
}

- (IBAction)En0Pressed:(id)sender { [self handle_en_press:0]; }
- (IBAction)En1Pressed:(id)sender { [self handle_en_press:1]; }
- (IBAction)En2Pressed:(id)sender { [self handle_en_press:2]; }
- (IBAction)En3Pressed:(id)sender { [self handle_en_press:3]; }
- (IBAction)En4Pressed:(id)sender { [self handle_en_press:4]; }
- (IBAction)En5Pressed:(id)sender { [self handle_en_press:5]; }

/*---------------------------------------------------------------------------------
 * Prompt user before disabling interface
 ---------------------------------------------------------------------------------*/

-(bool) confirm_en_press:(int)nInt
{
	int n;

	for (n=0; n<[m_pTargets count]; n++ )
	{
		NSDictionary* pTarget = [m_pTargets objectAtIndex:n];

		if ( [[pTarget objectForKey:kClaimed] boolValue] && [[pTarget objectForKey:kOnline] boolValue] && [self target_connected_to_interface:pTarget Interface:nInt] )
		{
			int nReturn = NSRunAlertPanel(@"Warning: Interface disabled", @"The interface you are about to disable connects to claimed devices. Are you sure you wish to disable this interface?", @"Cancel", @"Disable", @"");
			
			if ( nReturn == NSAlertDefaultReturn )
				return FALSE;
			
			// Only warn once
			break;
		}
	}
	
	return TRUE;
}


/*---------------------------------------------------------------------------------
 * Check all the buttons are up-to-date based on the present state
 ---------------------------------------------------------------------------------*/

- (void)update_buttons:(bool)fHaveAuth
{
	NSDictionary* pOurTarget;
	int nRow;
	
	// Claim is only enabled if a valid row is selected
	nRow = [m_pTableView selectedRow];
	if ( -1==nRow )
	{
		[m_pClaimedButton setEnabled:FALSE];
	}
	else
	{
		pOurTarget = [m_pTargets objectAtIndex:nRow];

		if ( pOurTarget )
			[m_pClaimedButton setEnabled: fHaveAuth && m_fDriverLoaded && [[pOurTarget objectForKey:kOnline] boolValue]];
	}

	[m_pEN0Button setEnabled: fHaveAuth && m_fDriverLoaded];
	[m_pEN1Button setEnabled: fHaveAuth && m_fDriverLoaded];
	[m_pEN2Button setEnabled: fHaveAuth && m_fDriverLoaded];
	[m_pEN3Button setEnabled: fHaveAuth && m_fDriverLoaded];
	[m_pEN4Button setEnabled: fHaveAuth && m_fDriverLoaded];
	[m_pEN5Button setEnabled: fHaveAuth && m_fDriverLoaded];
	
	[m_pDiscoverButton setEnabled: fHaveAuth && m_fDriverLoaded];
	[m_pTableView setEnabled: m_fDriverLoaded];
	
	// update_target_listings always occurs a little later as there is a delay between setting parameters and the changes actually occuring
	if ( m_fDriverLoaded )
		[NSTimer scheduledTimerWithTimeInterval:TARGET_UPDATE_DELAY target:self selector:@selector(update_target_listings:) userInfo:nil repeats:NO];
	
	// Just to make sure the selected row button is handled correctly as well
	[self update_table];
}

#pragma mark --
#pragma mark KEXT communication handling

/*---------------------------------------------------------------------------------
 * Set the number of ethernet interfaces
 ---------------------------------------------------------------------------------*/

-(int) set_en_interfaces
{
	int n, nECount;
	char* apsz[3];
	char pszEPort[10] = "-e";
	
	nECount = 0;
    apsz[0] = pszEPort;
	apsz[1] = NULL;
	
	// Create our command line string:
	for(n=0; n<m_nNumOfEthernetPorts; n++)
	{
		if ( [m_apENButtons[n] state] )
		{
			pszEPort[2+nECount*2] = '0' + n;
			pszEPort[3+nECount*2] = ',';
			++nECount;
		}
	}
	
	if ( 0==nECount )
		pszEPort[2] = NULL;
	else
		pszEPort[3+(nECount-1)*2] = NULL;
	
	debugVerbose("ENABLING INTERFACE STRING:%s\n", pszEPort);
	
	return [self send_to_aoed:apsz];
}


-(bool) target_connected_to_interface:(NSDictionary*)pTarget Interface:(int)nInterface
{
	int n;
	NSArray* pInterfaces;
	
	pInterfaces = [pTarget objectForKey:kInterfaces];
	
	for(n=0; n<[pInterfaces count]; n++)
		if ( [[pInterfaces objectAtIndex:n] intValue] == nInterface)
			return TRUE;
	
	return FALSE;
}

-(bool) target_claimed_by_us:(NSDictionary*)pTarget
{
	return cstr_ours([[pTarget objectForKey:kConfigString] cStringUsingEncoding:NSUTF8StringEncoding]);
}



/*---------------------------------------------------------------------------------
 * Handle the authorisation required to send data to aoed
 ---------------------------------------------------------------------------------*/

-(int) send_to_aoed:(char**)apsz
{
	if ( ![[NSFileManager defaultManager] fileExistsAtPath:[NSString stringWithCString:g_pszPath]] )
	{
		NSRunAlertPanel(@"Invalid Path to aoed", @"Please re-install AoE software", @"OK", @"", @"");
		return -1;
	}

	// Proceed with sending the info the kext
	if ( 0 == [self authorised:YES] )
	{
		SFAuthorization* pAuth = [m_pAuthView authorization];
		
		AuthorizationRef pARef = [pAuth authorizationRef];
		
		AuthorizationExecuteWithPrivileges(pARef, g_pszPath, kAuthorizationFlagDefaults, apsz, NULL);
	}
	else
	{
		debugError("Un-authorized, cannot communicate with daemon\n");
		return -1;
	}
	
	return 0;
}

/*---------------------------------------------------------------------------------
 * Check authorisation and optionally request it if it isn't already there
 ---------------------------------------------------------------------------------*/

-(int) authorised:(bool)AskIfNot
{
	if ( AskIfNot && (SFAuthorizationViewUnlockedState != [m_pAuthView authorizationState]) )
		[m_pAuthView authorize:m_pAuthView];

	return (SFAuthorizationViewUnlockedState == [m_pAuthView authorizationState]) ? 0 : -1;
}

#pragma mark --
#pragma mark Target handling

/*---------------------------------------------------------------------------------
 * Refresh device list
 ---------------------------------------------------------------------------------*/

- (IBAction)DiscoverPressed:(id)sender
{
	char pszDiscover[] = "-D";
	char* apsz[2];

    apsz[0] = pszDiscover;
	apsz[1] = NULL;

	[self send_to_aoed:apsz];	
	
	[self update_target_listings:NULL];
	[self update_buttons:(0 == [self authorised:NO])];
}


/*---------------------------------------------------------------------------------
 * Claim/Unclaim target by sending authorised info to aoed
 ---------------------------------------------------------------------------------*/

- (IBAction)ClaimedPressed:(id)sender
{
	AoEProperties AoEDriver;
	NSDictionary* pOurTarget;
	int nRow;
	char* apsz[2];
	char pszClaim[10] = "-";

	apsz[0] = pszClaim;
	apsz[1] = NULL;

	nRow = [m_pTableView selectedRow];
	pOurTarget = [m_pTargets objectAtIndex:nRow];

	// Check if the row is valid
	if ( -1==nRow )
		return;

	// Check authorisation
	if ( 0 != [self authorised:YES] )
		return;

	if ( 0 != AoEDriver.configure_matching() )
		return;
	if ( NULL==pOurTarget )
		return;
	
	if ( [[pOurTarget objectForKey:kClaimed] boolValue] )
	{
		if ( 0 != [self target_claimed_by_us:pOurTarget] )
		{
			int nReturn = NSRunAlertPanel(@"Warning: The target you have selected is currently owned by another computer",
										  @"Reclaiming this target may result in data corruption if the other computer is still attached. Are you sure you wish to reclaim this target?",
										  @"Cancel", @"OK", @"");
			
			if ( nReturn == NSAlertDefaultReturn )
				return;
		}

		pszClaim[1] = 'C';		// Unclaim
	}
	else
	{
		pszClaim[1] = 'c';		// Claim
	}

	// Set target number
	snprintf(pszClaim+2, sizeof(pszClaim)-2, "%d\0", [[pOurTarget objectForKey:kTargetNum] intValue]);

	debugVerbose("Claim/Unclaim target STRING:%s\n", pszClaim);
	
	[self send_to_aoed: apsz];

	// Disable this until timer has expired
	[m_pClaimedButton setEnabled:FALSE];

	// update_target_listings always occurs a little later as there is a delay between setting parameters and the changes actually occuring
	[NSTimer scheduledTimerWithTimeInterval:TARGET_UPDATE_DELAY target:self selector:@selector(update_target_listings:) userInfo:nil repeats:NO];
}

/*---------------------------------------------------------------------------------
 * Create our target
 ---------------------------------------------------------------------------------*/


-(NSDictionary*)create_target:(int)nShelf Slot:(int)nSlot Number:(int)nNumber Capacity:(UInt64)unCapacity ConfigString:(NSString*)pCString NumInterfaces:(int)nInterfaces Interfaces:(int*)anENInterfaces
{
	NSString*	pCapacityString;
	NSString*	pOwnerString;
	NSString*	pStatus;
	NSString*	pInterfaceString;
	NSMutableArray*	pInterfaces;
	bool		fClaimed;
	bool		fOnline;
	int			nTargetNumber;
	
	double fpCapacitykb;
	double fpCapacityMb;
	double fpCapacityGb;
	double fpCapacityTb;
	int n;
	
	nTargetNumber = nNumber;
	
	if ( pCString )
	{
		const char* str = get_owner_from_cstr([pCString cStringUsingEncoding:NSUTF8StringEncoding]);
		pOwnerString = [NSString stringWithUTF8String:str];
	}
	else
	{
		pCString = @"";
		pOwnerString = @"";
	}

	pInterfaces = [[NSMutableArray alloc] init];
	for(n=0; n<nInterfaces; n++)
		[pInterfaces addObject:[NSNumber numberWithInt:anENInterfaces[n]]];
	
	fClaimed = [pCString length]!=0;
	
	fpCapacitykb = unCapacity/2;			// convert from sectors size (512) to kB
	fpCapacityMb = fpCapacitykb/1024;
	fpCapacityGb = fpCapacityMb/1024;
	fpCapacityTb = fpCapacityGb/1024;
	
	if ( fpCapacityTb<1.0 && fpCapacityGb>1.0 )
		pCapacityString = [NSString stringWithFormat:@"%.1f GBytes", fpCapacityGb];
	else if ( fpCapacityGb<1.0 && fpCapacityMb>1.0 )
		pCapacityString = [NSString stringWithFormat:@"%.1f MBytes", fpCapacityMb];
	else if ( fpCapacityMb<1.0 && fpCapacitykb>1.0 )
		pCapacityString = [NSString stringWithFormat:@"%.1f kBytes", fpCapacitykb];
	else if ( fpCapacitykb<1.0 )
		pCapacityString = [NSString stringWithFormat:@"Unknown"];
	else
		pCapacityString = [NSString stringWithFormat:@"%.1f TBytes", fpCapacityTb];
	
	fOnline = (nInterfaces!=0);
	
	if ( fOnline )
	{
		pStatus = @"Online";

		pInterfaceString = [NSString stringWithFormat:@"en%d", anENInterfaces[0]];

		for(n=1; n<nInterfaces; n++)
			pInterfaceString = [pInterfaceString stringByAppendingString:[NSString stringWithFormat:@",en%d",  anENInterfaces[n]]];
	}
	else
	{
		pInterfaceString = NULL;
		pStatus = @"Offline";
	}
	
	return [NSDictionary dictionaryWithObjectsAndKeys:	[NSNumber numberWithInt:nShelf], kShelf,
														[NSNumber numberWithInt:nSlot], kSlot,
														pCapacityString, kCapacityString,
														[NSNumber numberWithUnsignedLongLong:unCapacity], kCapacity,
														pCString, kConfigString,
														pOwnerString, kOwner,
														pStatus, kStatus,
														pInterfaceString, kInterfacesString,
														[NSNumber numberWithBool:fClaimed], kClaimed,
														[NSNumber numberWithBool:fOnline], kOnline,
														[NSNumber numberWithInt:nTargetNumber], kTargetNum,
														pInterfaces, kInterfaces,
														nil];
}


/*---------------------------------------------------------------------------------
 * Refresh our target list
 ---------------------------------------------------------------------------------*/

- (void)update_target_listings:(NSTimer*)theTimer
{
	NSDictionary*		pNewTarget;
	AoEProperties AoEDriver;
	int n, nNumTargets;
	int anENInterfaces[MAX_SUPPORTED_ETHERNET_CONNETIONS];
	
	//debugVerbose("update_target_listings\n");

	if ( 0==AoEDriver.configure_matching() )
	{
		nNumTargets = AoEDriver.number_of_targets();

		[m_pTargets release];
		m_pTargets = [[NSMutableArray alloc] init];

		// Check if the target is already in our list
		for (n=0; n<nNumTargets; n++)
		{
			NSString* pConfigString = (NSString*) AoEDriver.get_config_string(n);
			int nInterfaces;
			
			if ( 0==[pConfigString length] )
			{
				[pConfigString release];
				pConfigString = NULL;
			}

			nInterfaces = AoEDriver.get_targets_en_interfaces(n, anENInterfaces);

			pNewTarget = [self create_target:AoEDriver.get_shelf_number(n) Slot:AoEDriver.get_slot_number(n) Number:AoEDriver.get_target_number(n) Capacity:AoEDriver.get_capacity(n) ConfigString:pConfigString NumInterfaces:nInterfaces Interfaces:anENInterfaces];
			//NSLog(@"Creating new target (%d): %@\n", n, pNewTarget);

			[m_pTargets addObject:pNewTarget];
		}

#if 0
		// Add some extra targets for testing the table
		pNewTarget = [self create_target:47 Slot:3 Number:nNumTargets+1 Capacity:2369765367 ConfigString:@"asdf" NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		pNewTarget = [self create_target:47 Slot:4 Number:nNumTargets+2 Capacity:30000 ConfigString:NULL NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		pNewTarget = [self create_target:47 Slot:5 Number:nNumTargets+3 Capacity:40000 ConfigString:NULL NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		pNewTarget = [self create_target:35 Slot:6 Number:nNumTargets+4 Capacity:450000 ConfigString:NULL NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		pNewTarget = [self create_target:766 Slot:57 Number:nNumTargets+5 Capacity:10000 ConfigString:NULL NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		pNewTarget = [self create_target:3556 Slot:996 Number:nNumTargets+6 Capacity:24000 ConfigString:NULL NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		pNewTarget = [self create_target:1245 Slot:1234 Number:nNumTargets+7 Capacity:554000 ConfigString:NULL NumInterfaces:0 Interfaces:NULL];
		[m_pTargets addObject:pNewTarget];
		nNumTargets = [m_pTargets count];
#endif
		
		// release any targets that are no longer in the list
		if ( 0==nNumTargets )
		{
			[m_pTargets release];
			m_pTargets = nil;
		}

		[self update_buttons:(0 == [self authorised:NO])];
		[self update_table];
	}
}

#pragma mark --
#pragma mark Table handling

/*---------------------------------------------------------------------------------
 * Set's claim/unclaim button based on row selection and refresh data
 ---------------------------------------------------------------------------------*/

-(void) update_table
{
	int nRow;
	
	nRow = [m_pTableView selectedRow];
	
	if ( nRow<[m_pTargets count] )
	{
		NSDictionary* pTarget = [m_pTargets objectAtIndex:nRow];

		if ( [[pTarget objectForKey:kOnline] boolValue] )
		{
			if ( [[pTarget objectForKey:kClaimed] boolValue] )
				[m_pClaimedButton setTitle:@"Unclaim"];
			else
				[m_pClaimedButton setTitle:@"Claim"];
		}
		//else
		// could display some text here for this case, but the button is disabled anyway
	}

	[self ResortTable];
	[m_pTableView reloadData];
}



/*---------------------------------------------------------------------------------
 * Just do a quick count of the the number of targets
 ---------------------------------------------------------------------------------*/

- (int)numberOfRowsInTableView:(NSTableView *)tableView
{
    return [m_pTargets count];
}


/*---------------------------------------------------------------------------------
 * Return our data depending on what is requested
 ---------------------------------------------------------------------------------*/

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn *)tableColumn row:(int)row
{
	NSDictionary* pOurTarget;

	pOurTarget = [m_pTargets objectAtIndex:row];

	return [pOurTarget valueForKey:[tableColumn identifier]];
}

/*---------------------------------------------------------------------------------
 * Handle a different selection
 ---------------------------------------------------------------------------------*/

- (void)tableViewSelectionDidChange:(NSNotification *)aNotification
{
	// Make sure everything is up to date after a selection has changed
	[self update_buttons: (0 == [self authorised:NO])];
}


/*---------------------------------------------------------------------------
 * Resort the table based on a possible selection
 ---------------------------------------------------------------------------*/

-(void) ResortTable
{
	id SortID;
	int i;
	NSArray* allColumns = [m_pTableView tableColumns];
	
	// Clear current images
	for (i=0; i<[m_pTableView numberOfColumns]; i++) 
	{
		NSTableColumn* tableColumn = [allColumns objectAtIndex:i];

		SortID = [tableColumn identifier];
		
		// Sort capacity by the number rather than the string
		if ( [SortID isEqualToString:kCapacityString] )
			SortID = kCapacity;
		
		// Change the image and begin the sort
		if ([m_pTableView indicatorImageInTableColumn:tableColumn]==[NSImage imageNamed:@"NSAscendingSortIndicator"])
			[self sortWithDescriptor:[[NSSortDescriptor alloc] initWithKey:SortID ascending:YES]];
		else if ([m_pTableView indicatorImageInTableColumn:tableColumn]==[NSImage imageNamed:@"NSDescendingSortIndicator"])
			[self sortWithDescriptor:[[NSSortDescriptor alloc] initWithKey:SortID ascending:NO]];
	}
}


/*---------------------------------------------------------------------------
 * Handle a clicked column by changing the image and reversing the sort
 * direction
 ---------------------------------------------------------------------------*/

- (void)tableView: (NSTableView*)inTableView didClickTableColumn:(NSTableColumn*)tableColumn
{  
	int i;
	NSArray* allColumns=[inTableView tableColumns];

	// Clear current images
	for (i=0; i<[inTableView numberOfColumns]; i++) 
		if ([allColumns objectAtIndex:i]!=tableColumn)
			[inTableView setIndicatorImage:nil inTableColumn:[allColumns objectAtIndex:i]];

	// Change the image and begin the sort
	if ([inTableView indicatorImageInTableColumn:tableColumn]!=[NSImage imageNamed:@"NSAscendingSortIndicator"])
		[inTableView setIndicatorImage:[NSImage imageNamed:@"NSAscendingSortIndicator"] inTableColumn:tableColumn];  
	else
		[inTableView setIndicatorImage:[NSImage imageNamed:@"NSDescendingSortIndicator"] inTableColumn:tableColumn];

	[self ResortTable];
}


- (void)sortWithDescriptor:(id)descriptor
{
	NSMutableArray* pSorted = [[NSMutableArray alloc] initWithCapacity:1];
	[pSorted addObjectsFromArray:[m_pTargets sortedArrayUsingDescriptors:[NSArray arrayWithObject:descriptor]]];
	[m_pTargets removeAllObjects];
	[m_pTargets addObjectsFromArray:pSorted];
	[m_pTableView reloadData];
	[pSorted release];
}

#pragma mark --
#pragma mark Tab handling

/*---------------------------------------------------------------------------------
 * Make sure everything is up to date after a selection has changed (just in case)
 ---------------------------------------------------------------------------------*/

- (void)tabView:(NSTabView *)tabView didSelectTabViewItem:(NSTabViewItem *)tabViewItem
{
	[self update_buttons:(0 == [self authorised:NO])];
}

@end
