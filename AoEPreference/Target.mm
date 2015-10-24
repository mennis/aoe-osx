//
//  Target.mm
//  AoEPreference
//
//  Copyright 2008 Brantley Coile Company. All rights reserved.
//

#import "Target.h"
#import "ConfigString.h"

@implementation Target

- (id)initWithData:(int)nShelf Slot:(int)nSlot Number:(int)nNumber Capacity:(int)fpCapacity ConfigString:(NSString*)pCString NumInterfaces:(int)nInterfaces Interfaces:(int*)anENInterfaces
{
	float fpCapacitykb;
	float fpCapacityMb;
	float fpCapacityGb;
	float fpCapacityTb;
	int n;

	self = [super init];
	
	if ( self )
	{
		m_pShelf = [[NSNumber numberWithInt:nShelf] retain];
		m_pSlot = [[NSNumber numberWithInt:nSlot] retain];
		m_nTargetNumber = nNumber;
		m_pCapacity = [[NSNumber numberWithInt:fpCapacity] retain];	 // This value is in sectors
		
		if ( pCString )
		{
			const char* str = get_owner_from_cstr([pCString cStringUsingEncoding:NSUTF8StringEncoding]);
			m_pOwnerString = [[NSString stringWithUTF8String:str] copy];
		}

		m_pConfigString = [pCString copy];
		m_pClaimed = [[NSNumber numberWithInt:[m_pConfigString length]!=0] retain];
		m_pAvailable = [[NSNumber numberWithInt:[m_pConfigString length]!=0] retain];
		
		for(n=0; n<nInterfaces; n++)
			m_anENInterfaces[n] = anENInterfaces[n];
		
		m_fClaimed = [m_pConfigString length]!=0;

		fpCapacitykb = [m_pCapacity intValue]/2;			// convert from sectors size (512) to kB
		fpCapacityMb = fpCapacitykb/1024;
		fpCapacityGb = fpCapacityMb/1024;
		fpCapacityTb = fpCapacityGb/1024;
		
		if ( fpCapacityTb<10.0 && fpCapacityGb>1.0 )
			m_pCapacityString = [[NSString stringWithFormat:@"%.1f GBytes", fpCapacityGb] copy];
		else if ( fpCapacityGb<1.0 && fpCapacityMb>1.0 )
			m_pCapacityString = [[NSString stringWithFormat:@"%.1f MBytes", fpCapacityMb] copy];
		else if ( fpCapacityMb<1.0 && fpCapacitykb>1.0 )
			m_pCapacityString = [[NSString stringWithFormat:@"%.1f kBytes", fpCapacitykb] copy];
		else if ( fpCapacitykb<1.0 )
			m_pCapacityString = [[NSString stringWithFormat:@"Unknown"] copy];
		else
			m_pCapacityString = [[NSString stringWithFormat:@"%.1f TBytes", fpCapacityTb] copy];
		
		m_nInterfaces = nInterfaces;
		m_fOnline = (nInterfaces!=0);
		
		if ( m_fOnline )
		{
			m_pStatus = @"Online";
			
			m_pInterfaces = [NSString stringWithFormat:@"en%d", anENInterfaces[0]];

			for(n=1; n<nInterfaces; n++)
				m_pInterfaces = [m_pInterfaces stringByAppendingString:[NSString stringWithFormat:@",en%d",  anENInterfaces[n]]];

			m_pInterfaces = [m_pInterfaces copy];
		}
		else
			m_pStatus = @"Offline";
	}
	return self;
}

-(bool) connected_to_interface:(int)nInterface
{
	int n;
	
	for(n=0; n<m_nInterfaces; n++)
		if ( m_anENInterfaces[n] == nInterface)
			return TRUE;

	return FALSE;
}

-(bool) claimed_by_us
{
	return cstr_ours([m_pConfigString cStringUsingEncoding:NSUTF8StringEncoding]);
}

@end
