/*
 *  AboutWindController.h
 *  AoEPreference
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 */


#import <Cocoa/Cocoa.h>

@interface AboutWindController : NSWindowController
{
	IBOutlet NSTextField*	name;
	IBOutlet NSTextField*	version;
	IBOutlet NSTextField*	copyright;
}

@end
