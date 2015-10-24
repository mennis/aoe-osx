/*
 *  AboutWindController.m
 *  AoEPreference
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */

#import "AboutWindController.h"

@implementation AboutWindController

// -------------------------------------------------------------------------------
//	infoValueForKey:key
// -------------------------------------------------------------------------------
-(id)infoValueForKey:(NSString*)key
{
	// InfoPlist.strings entries have priority over Info.plist ones.
	NSBundle* thisBundle = [NSBundle bundleWithIdentifier: @"net.corvus.AoEPreference"];
		
	if ([[thisBundle localizedInfoDictionary] objectForKey:key])
		return [[thisBundle localizedInfoDictionary] objectForKey:key];
	return [[thisBundle infoDictionary] objectForKey:key];
}

// -------------------------------------------------------------------------------
//	awakeFromNib:
// -------------------------------------------------------------------------------
- (void)awakeFromNib
{
	// display our NSBundle information
	
	NSString* nameStr = [self infoValueForKey:@"CFBundleName"];
	[name setStringValue: nameStr];
	
	NSString* versionStr = [self infoValueForKey:@"CFBundleVersion"];
	[version setStringValue: versionStr];
	
	NSString* copyrightStr = [self infoValueForKey:@"NSHumanReadableCopyright"];
	[copyright setStringValue: copyrightStr];
}

@end
