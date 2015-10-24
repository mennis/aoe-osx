//
//  Loader.m
//  Loader
//
//  Copyright 2008 Brantley Coile Company. All rights reserved.
//

// This application just gets the authorization and executes the real app with the correct uid
// It was written as a Cocoa app so we can hide the real application in it's bundle


#import "Loader.h"
#import <Security/Authorization.h>
#import <Security/AuthorizationTags.h>

#define APP_NAME		@"RealAoECommander.app/Contents/MacOS/RealAoECommander"

@implementation Loader

-(void)awakeFromNib
{
	// Find the application based hidden in our bundle resource directory
	NSString* pProgramToLaunch = [[[NSBundle mainBundle] resourcePath] stringByAppendingPathComponent:APP_NAME];

	// Get authorization
	AuthorizationRef myAuthorizationRef;
	AuthorizationItem myItems;
	AuthorizationRights myRights;
	AuthorizationFlags myFlags;
	OSStatus myStatus;
		
	myItems.name = kAuthorizationRightExecute;
	myItems.valueLength = 0;
	myItems.value = NULL;
	myItems.flags = 0;
	myRights.count = 1;
	myRights.items = &myItems;
		
	myFlags = kAuthorizationFlagDefaults | kAuthorizationFlagInteractionAllowed | kAuthorizationFlagPreAuthorize | kAuthorizationFlagExtendRights;	
	myStatus = AuthorizationCreate(NULL,kAuthorizationEmptyEnvironment,myFlags,&myAuthorizationRef);
	myStatus = AuthorizationCopyRights(myAuthorizationRef, &myRights, NULL, myFlags, NULL);
	
	// If that went well, we run our app
	if ( errAuthorizationSuccess==myStatus ) 
	{
		const char* pPath = [pProgramToLaunch cStringUsingEncoding:NSASCIIStringEncoding];

		OSStatus stat = AuthorizationExecuteWithPrivileges(myAuthorizationRef, pPath, 0, NULL, NULL);
		if ( stat )
			NSLog(@"Failed to load application. Error=%d\n", stat);
	}

	// Nothing more to do in this app, so just exit
	[NSApp terminate:self];
}
@end
