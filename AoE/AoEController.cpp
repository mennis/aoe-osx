/*
 *  AoEController.cpp
 *  AoE
 *
 *  Copyright © 2009 Brantley Coile Company, Inc. All rights reserved.
 *
 *
 *
 *
 *
 * Each AoE target has a separate controller/device pair.
 * - Classically, ATA devices appear in device0/device1 pairs. There are assumptions about this in IOATAFamily and the protocol service IOATABlockStorage.
 *   AoE uses a single device for each controller (ie. there are no device1 in the system).
 * - This also provides more flexible queuing for multiple targets attached to the system.

 Here's a quick summary of our base class, IOATAController:
 
 When commands are sent on the bus:
 ---------------------------------
 executeCommand()	- Called to send command to ATA device (this will occur on the work loop)
					- The command is enqueued (either as normal, or at the front for immediate messages)
 dispatchNext()		- Command is read from the queue and is processed in different ways depending on it's opcode (see enum ataOpcode in IOATATypes.h).
					- Depending on the command, this is either: ATA I/O, Register Access, Queue Flush, or an ATA bus reset:
 kATAFnExecIO		~ calls handleExecIO() which starts timeout and the state machine in asyncIO()
	asyncIO()		- STATE MACHINE WITH THE FOLLOWING STATES:
	asyncCommand()	- starts DMA or performs copy with asyncData. Writes to TF with issueCommand()
	asyncData()		- transmit data copy (read/write) [PIO only]
 kATAFnRegAccess	~ handleRegAccess() calls registerAccess
	registerAccess()- read/write TF registers
 kATAFnBusReset		~ handleBusReset()
	handleBusReset()- 
 kATAFnQFlush		~ handleQueueFlush()
 issueCommand()		- Set values in memory (usually this would write directly to the TF)
 
 handleDeviceInterrupt()	- Called in ISR indicate transaction has completed
 completeIO()				- Called after a command has completed. This executes callback and calls dispatchNext
 synchronousIO()			- Commands that are sent without waiting for interrupts (polls for the response)
 asyncStatus()				- Get end result of IO from the device
 selectDevice()				- Perform device selection according to ATA standards document

 "Time outs" =  Called with handleExecIO starts when a command is dispatched. If a command times out, an error will be returned to the caller
 
 AoEController subclass handles the following differently
 ----------------------

 - asyncCommand()	- Initiates command with issueCommand(). If required, it starts DMA or performs copy with asyncData.
 - asyncData		- Usually handles the PIO read/write copies. In AoE, it just handles reads. This repeats until the transfer is complete
 - scanForDrives()	- always indicate there is a drive present
 - handleDeviceInterrupt() - Trigger when data has been received
 - selectDevice() - Nothing to do here for AoE
 - bus resets - not required for AoE
  - asyncStatus()	- Checks the status of the returned packet
 - allocateDoubleBuffer - used for read/writes. Set to max transfer size
 - handleRegAccess - Command to set ATA registers
 - issueCommand() - Initialise a packet for sending out the interface
 - registerAccess() - Copy ATA data to the outgoing packet
 
 Here's the mappings between the various formats and names
 
 			ATA			|			AoE			|	Task File registers
 -----------------------+-----------------------+-----------------------
 LBA low (r/w)			|		lba0/lba3		|	_tfSectorNReg
 LBA mid (r/w)			|		lba1/lba4		|	_tfCylLoReg
 LBA high (r/w)			|		lba2/lba5		|	_tfCylHiReg
 Device (r/w)			|		AFlags			|	_tfSDHReg
 Sector Count (r/w)		|		Sector Count	|	_tfSCountReg
 Command (w)			|		Cmd/Status		|	_tfStatusCmdReg
 Status (r)				|		Cmd/Status		|	_tfStatusCmdReg
 Features (w)			|		Err/Feature		|	_tfFeatureReg
 Error(r)				|		Err/Feature		|	_tfFeatureReg
 Data Registers (r/w)	|		(unused)		|	_tfDataReg
 Device Control (w)		|		(unused)		|	_tfAltSDevCReg
 Alternate Status (r)	|		(unused)		|	_tfAltSDevCReg
 
 
 AoE targets only support the following commands:
 
 kATAcmdRead				= 0x0020,						// Read command
 kATAcmdReadExtended		= 0x0024,						// Read Extended (with retries)
 kATAcmdWrite				= 0x0030,						// Write command
 kATAcmdWriteExtended		= 0x0034,						// Write Extended (with retries)
 kATAcmdFlushCache			= 0x00E7,						// Flush Cache
 kATAcmdDriveIdentify		= 0x00EC,						// Identify Drive command

 */

#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOKitKeys.h>
#include "AoEControllerInterface.h"
#include "AoEtherFilter.h"
#include "AoEService.h"
#include "AoEController.h"
#include "AoEDevice.h"
#include "../Shared/AoEcommon.h"
#include "debug.h"

__BEGIN_DECLS
#include <net/kpi_interface.h>
#include <sys/kernel_types.h>
#include <sys/kpi_mbuf.h>
__END_DECLS

#define LUN_UPDATE_TIME_MS								(10*1000)
#define DEFAULT_TIME_UNTIL_TARGET_OFFLINE_US			(60*1000*1000)

// Enable this to see the received/written data
//#define PRINT_DATA_MEMORY

#define super IOATAController

#pragma mark -
#pragma mark Set up/down

OSDefineMetaClassAndStructors(AOE_CONTROLLER_NAME, IOATAController);


bool AOE_CONTROLLER_NAME::init(AOE_CONTROLLER_INTERFACE_NAME* pProvider, int nShelf, int nSlot, ifnet_t ifnet_receive, u_char* pTargetsMACAddress, UInt32 MTU, int nMaxTransferSize, int nNumber)
{
	OSNumber* num;
	bool fRet;
	
	fRet = super::init(NULL);
	m_pProvider = pProvider;
	m_pAoEDevice = NULL;
	m_MTU = MTU;
	m_pReceivedATAHeader = NULL;
	m_fExtendedLBA = FALSE;
	m_nReadWriteRepliesRequired = 0;
	m_unReadBaseTag = 0;
	m_unReceivedTag = 0;
	m_PreviousWriteStatus = 0;
	m_PreviousWriteError = 0;
	m_aConfigString[0] = NULL;
	m_nBufferCount = 0;
	m_nMaxTransferSize = nMaxTransferSize;
	m_fRegistered = FALSE;
	m_ATAState = kATAOnlineEvent;	// Record the present state
	m_nOutstandingIdentTag = 0;
	m_IdentifiedCapacity = 0;
	
	// All our constraints are determined by the MTU and the remaining data in the packet
	m_nMaxSectorsPerTransfer = COUNT_SECTORS_FROM_MTU(m_MTU);
	debug("[%d.%d] Setting transfer sizes based on MTU of: %d bytes (%d sectors per transfer)\n", nShelf, nSlot, m_MTU, m_nMaxSectorsPerTransfer);
	
	memset(&m_target, 0, sizeof(TargetInfo));

	// Setup our structure
	m_target.nNumberOfInterfaces = 1;
	m_target.nShelf = nShelf;
	m_target.nSlot = nSlot;
	m_target.nTargetNumber = nNumber;		// 1-based
	m_target.aInterfaces[0] = ifnet_receive;
	m_target.aInterfaceNum[0] = ifnet_unit(ifnet_receive);
	m_target.nLastSentInterface = 0;

	ifnet_lladdr_copy_bytes(ifnet_receive, m_target.aaSrcMACAddress[0], ETHER_ADDR_LEN);
	bcopy(pTargetsMACAddress, m_target.aaDestMACAddress[0], ETHER_ADDR_LEN);
	clock_get_uptime(&m_time_since_last_comm);

	// Register properties:
	num = OSNumber::withNumber(nNumber, 16);
	setProperty(TARGET_PROPERTY, num);
	num->release();

	num = OSNumber::withNumber(nShelf, 16);
	setProperty(SHELF_PROPERTY, num);
	num->release();

	num =OSNumber::withNumber(nSlot, 8);
	setProperty(SLOT_PROPERTY, num);
	num->release();

	num =OSNumber::withNumber(nSlot, 32);
	setProperty(TARGET_NUMER_PROPERTY, nNumber);
	num->release();

	// Note: This is set here, but updated later in the target after it has determined it's size
	num =OSNumber::withNumber(0llu, 64);
	setProperty(CAPACITY_PROPERTY, num);
	num->release();

	update_interface_property();
	
	return fRet;
}


void AOE_CONTROLLER_NAME::uninit(void)
{
	debug("AOE_CONTROLLER_NAME::uninit\n");

	cancel_command(TRUE);

	removeProperty(SHELF_PROPERTY);
	removeProperty(SLOT_PROPERTY);
	removeProperty(CAPACITY_PROPERTY);
	removeProperty(CONFIG_STRING_PROPERTY);
	removeProperty(TARGET_NUMER_PROPERTY);
	removeProperty(BUFFER_COUNT_PROPERTY);
	removeProperty(TARGET_PROPERTY);
	removeProperty(IDENT_CAPACITY_PROPERTY);
	removeProperty(IDENT_MODEL_PROPERTY);
	removeProperty(IDENT_SERIAL_PROPERTY);
	
	if ( m_pAoEDevice )
	{
		m_pAoEDevice->uninit();
		m_pAoEDevice->terminate();
		CLEAN_RELEASE(m_pAoEDevice);
	}
}





/*---------------------------------------------------------------------------
 * Use to register a disk (and force it to mount)
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::registerDiskService(void)
{
	m_fRegistered = TRUE;

	debug("AOE_CONTROLLER_NAME::registerDiskService\n");
//#warning not registering disk service
	if ( m_pAoEDevice && (0==device_attached()) )
		m_pAoEDevice->registerService();
}



/*---------------------------------------------------------------------------
 * Inform our clients that we are now online
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::device_online(void)
{
	debug("[%d.%d] - marking device online\n", m_target.nSlot, m_target.nSlot);

	if ( m_ATAState==kATAOfflineEvent )
		executeEventCallouts( kATAOnlineEvent, kATADevice0DeviceID );
	else
		debugVerbose("Not executing event because device is already online\n");

	m_ATAState = kATAOnlineEvent;
}


/*---------------------------------------------------------------------------
 * Stop any commands that may be in progress
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::cancel_command(bool fClean)
{
	IOReturn err;

	debug("cancel_command(%d)\n", fClean);

	if ( fClean )
		err = kATANoErr;
	else
		err = kATADeviceError;

	// Stop any commands that may be in process
	executeEventCallouts( kATAOfflineEvent, kATADevice0DeviceID );
	if ( _currentCommand )
		_currentCommand->state = IOATAController::kATAComplete;	

	completeIO( err );
	m_ATAState = kATAOfflineEvent;
}







#pragma mark -
#pragma mark Handling responses

/*---------------------------------------------------------------------------
 * Handle the reception of an aoe packet. This is only really related to the config string
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::handle_aoe_cmd(ifnet_t ifnet_receive, aoe_cfghdr_rd* pCfgHeader, mbuf_t* pMBufData)
{
	int n, nSize;
	OSNumber* num;
	
	if ( mbuf_next(*pMBufData)!=NULL )
		debugError("Not copying across all of the config string");

	// TODO: If we get this error, we'll have to add some code to copy the data across from the chained mbufs
	
	/*
	mbuf_t ReceivedMBufCont = pMBufData ? mbuf_next(*pMBufData) : NULL;
	while ( ReceivedMBufCont )
	{
		length = mbuf_len(m_ReceivedMBufCont);
	 
		// do copy...
	 
		m_ReceivedMBufCont = mbuf_next(m_ReceivedMBufCont);
	}
	*/

	if ( (NULL==pCfgHeader) || (NULL==m_pProvider) )
		return;

	// Get our buffer count
	m_nBufferCount = AOE_CFGHEADER_GETBCOUNT(pCfgHeader);
	
	debug("[%d.%d] Buffer Count: %d\n", m_target.nShelf, m_target.nSlot, m_nBufferCount);
	
	num =OSNumber::withNumber(m_nBufferCount, 32);
	setProperty(BUFFER_COUNT_PROPERTY, num);
	num->release();

	m_pProvider->set_max_outstanding(ifnet_receive, m_target.nShelf, m_nBufferCount);

	switch ( AOE_CFGHEADER_GETCCMD(pCfgHeader) )
	{
		case CONFIG_STR_GET:
		{	
			// Handle the config string ----
			nSize = AOE_CFGHEADER_GETCSLEN(pCfgHeader);
			
			if ( nSize>MAX_CONFIG_STRING_LENGTH )
				debugError("[%d.%d] Config string is too large.", m_target.nShelf, m_target.nSlot);
			
			nSize = MIN(nSize, MAX_CONFIG_STRING_LENGTH);

			// Copy over config string
			for(n=0; n<nSize; n++)
				m_aConfigString[n] = pCfgHeader->ac_cstring[n];

			debug("[%d.%d] Config string: %s\n", m_target.nShelf, m_target.nSlot, m_aConfigString);

			OSString* pString = OSString::withCString(m_aConfigString);
			setProperty(CONFIG_STRING_PROPERTY, (OSObject*) pString );
			pString->release();
		}
		default:
			// not handled, do nothing
			break;
	}
}





/*---------------------------------------------------------------------------
 * Handle the reception of an ata packet and deal with particular commands as they come in
 * Sometimes it's necessary to pre-process the received commands and that is done here
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::ata_response(aoe_atahdr_rd* pATAHeader, mbuf_t* pMBufData, UInt32 Tag)
{
	bool fReadyToIssueInterrupt;
	
	fReadyToIssueInterrupt = FALSE;		// Only handled if command is outstanding

	if ( NULL==pATAHeader )
	{
		debugError("Invaid ATA header\n");
		return -1;
	}
	
	debug("AOE_ATA_COMMAND RCV - (length=%d) [] AFlags=%#x Err=%#x SectorCount=%#x Status=%#x lba0=%#x lba1=%#x lba2=%#x lba3=%#x lba4=%#x lba5=%#x\n",
		  pMBufData ? mbuf_len(*pMBufData) : 0,
		  AOE_ATAHEADER_GETAFLAGS(pATAHeader),
		  AOE_ATAHEADER_GETERR(pATAHeader),
		  AOE_ATAHEADER_GETSCNT(pATAHeader),
		  AOE_ATAHEADER_GETSTAT(pATAHeader),
		  AOE_ATAHEADER_GETLBA0(pATAHeader),
		  AOE_ATAHEADER_GETLBA1(pATAHeader),
		  AOE_ATAHEADER_GETLBA2(pATAHeader),
		  AOE_ATAHEADER_GETLBA3(pATAHeader),
		  AOE_ATAHEADER_GETLBA4(pATAHeader),
		  AOE_ATAHEADER_GETLBA5(pATAHeader));
	
	// Store this info in member variables as it'll be accessed later when the read state machine is accessed
	m_ReceivedMBufCont = pMBufData ? mbuf_next(*pMBufData) : NULL;
	m_pReceivedATAHeader = pATAHeader;
	m_unReceivedATADataSize = (pMBufData && pATAHeader) ? mbuf_len(*pMBufData) - (&pATAHeader->aa_Data[0] - (UInt16*)pATAHeader)-16 : 0;
	
	m_unReceivedTag = Tag;

	// At this point, we can pre-process the data (if necessary and possible)
	if ( _currentCommand )
	{
		UInt8 PrevCommand = _currentCommand->getStatus();	//status/command are in the same register
		
		fReadyToIssueInterrupt = TRUE;	// All commands are except write commands

		//-----------------------------//
		// Translate previous commands //
		//-----------------------------//
		switch ( PrevCommand )
		{
			case kATAcmdReadDMAExtended:
				PrevCommand = kATAcmdReadExtended;
				break;
			case kATAcmdReadDMA:
				PrevCommand = kATAcmdRead;
				break;
			case kATAcmdWriteDMAExtended:
				PrevCommand = kATAcmdWriteExtended;
				break;
			case kATAcmdWriteDMA:
				PrevCommand = kATAcmdWrite;
				break;
		}

		debug("[%d.%d] Previous command was: %#x\n", m_target.nShelf, m_target.nSlot, PrevCommand);

		switch ( PrevCommand )
		{
			case kATAcmdDriveIdentify :
			{
				fReadyToIssueInterrupt = handle_identify(pATAHeader);
				break;
			}
			case kATAcmdWrite :
			case kATAcmdWriteExtended :
			{
				if ( 0==m_nReadWriteRepliesRequired )
					debugError("m_nReadWriteRepliesRequired is already zero, but received a response\n");
				
				--m_nReadWriteRepliesRequired;
				
				if ( m_nReadWriteRepliesRequired )
				{
					fReadyToIssueInterrupt = FALSE;
					debug("Write reply just received, %d more required before issuing an interrupt\n", m_nReadWriteRepliesRequired);
					
					// Check if an error was received in that transfer and store it if we aren't storing already
					// We wait until all transfers are complete before passing the error back to the system
					if ( !(m_PreviousWriteStatus || m_PreviousWriteError) )
					{
						m_PreviousWriteStatus = AOE_ATAHEADER_GETSTAT(pATAHeader);
						
						if ( m_PreviousWriteStatus & mATAError )
						{
							m_PreviousWriteError = AOE_ATAHEADER_GETERR(pATAHeader);
							debugVerbose("*Error* - WRITE error mid-transfer. Storing error state later...\n");
						}
					}
				}
				else
					debug("Issuing interrupt now that all write replies have been received\n");
				break;
			}
			case kATAcmdRead :
			case kATAcmdReadExtended :
				--m_nReadWriteRepliesRequired;
				
				completeDataRead(&fReadyToIssueInterrupt);
				break;
			case kATAcmdFlushCache:
			case kATAcmdFlushCacheExtended:
			case kATAcmdSetFeatures:
			case kATAcmdSleep:
				// These are faked responses. No additional processing is necessary, just report interrupt occurred
				break;
			default:
				debugError("Unexpected command received - check handling\n");
				break;
		}
	}
	else
	{
		// An ATA command was received without a valid command. It must have been sent by us

		debugVerbose("m_nOutstandingIdentTag=%d, receivedtag = %d\n", m_nOutstandingIdentTag, m_unReceivedTag);

		if ( m_nOutstandingIdentTag==m_unReceivedTag )
		{
			handle_identify(pATAHeader);
			fReadyToIssueInterrupt = FALSE;
			m_nOutstandingIdentTag = 0;			
		}
	}
	
	if ( !fReadyToIssueInterrupt )
		debug("Holding off on interrupt command as more replies are still expected\n");
	
	return fReadyToIssueInterrupt ? handleDeviceInterrupt() : 0;
}


/*---------------------------------------------------------------------------
 * Handle the reception of an identify command
 ---------------------------------------------------------------------------*/
bool AOE_CONTROLLER_NAME::handle_identify(aoe_atahdr_rd* pATAHeader)
{
	bool fReadyToIssueInterrupt;
	OSString* pModelNum;
	OSString* pPrevModelNum;
	OSString* pSerialNum;
	OSString* pPrevSerialNum;
	UInt16*	pDeviceIdentifyData16;
	UInt8*	pDeviceIdentifyData;
	UInt64	NumSectors;
	UInt8	checkSum;
	int n;

	pDeviceIdentifyData16 = (UInt16*) &pATAHeader->aa_Data[0];

	fReadyToIssueInterrupt = TRUE;
	
	// To be able to handle large transfers, we have to fake ATA's DMA support.
	// Since the DMA/PIO mode is determined partly by what is reported by the drive, 
	// we override the IDENTIFY command to appear as if DMA was supported.
	
	pDeviceIdentifyData16[53] |= 0x0002;		// DMA fields are valid
	pDeviceIdentifyData16[49] |= 0x100;			// Allow DMA
	pDeviceIdentifyData16[63] |= 0x7;			// Support mode 0,1,2
	
#if 0
#warning limiting the disk size as a test...
	// If necessary, the size of the drive can be adjusted here
	pDeviceIdentifyData16[100] = 0x5000;		// This is the number of sectors that the OS will see...
	pDeviceIdentifyData16[101] = pDeviceIdentifyData16[102] = pDeviceIdentifyData16[103] = 0;
#endif
	debug("[%d.%d] - size=%#x %#x %#x %#x\n", m_target.nShelf, m_target.nSlot, pDeviceIdentifyData16[100], pDeviceIdentifyData16[101], pDeviceIdentifyData16[102], pDeviceIdentifyData16[103]);
	
	// Use this to check the raw IDENTIFY data
	//for (n=0; n<512/2; n++)
	//	debug("ID[%d] = %#x\n", n, pDeviceIdentifyData16[n]);
		
		
	// If the checksum isn't there, the system prints the message:
	// :ATA Disk: Checksum Cookie not valid"
	// Because the AoE target doesn't appear to return the checksum, we calculate it manually and add it in and here
		
	/*
	 The (T13/1410D revision 3a) spec says:
	 
	 8.15.61 Word 255: Integrity word 
	 The use of this word is optional. If bits (7:0) of this word contain the signature A5h, bits (15:8) contain the data 
	 structure checksum. The data structure checksum is the two’s complement of the sum of all bytes in words
	 (254:0) and the byte consisting of bits (7:0) in word 255. Each byte shall be added with unsigned arithmetic, 
	 and overflow shall be ignored. The sum of all 512 bytes is zero when the checksum is correct. 
	 */
		
	// Calculate the checksum
	pDeviceIdentifyData = (UInt8*) &pATAHeader->aa_Data[0];
	
	// Add the checksum check byte
	pDeviceIdentifyData[kATADefaultSectorSize-2] = 0xA5;
	
	for (n = checkSum = 0; n<kATADefaultSectorSize-1; n++ )
		checkSum += pDeviceIdentifyData[n];
		
	pDeviceIdentifyData[kATADefaultSectorSize-1] = ~checkSum+1;
		
	// Check the checksum
	for (n = checkSum = 0; n<kATADefaultSectorSize; n++ )
		checkSum += pDeviceIdentifyData[n];
			
	if ( checkSum )
		debugError("Faked checksum is incorrect (= %#x)\n", checkSum);
				
	// Check if the IDENT was sent manually, or if it was part of the ATA drivers command
	if ( m_nOutstandingIdentTag==m_unReceivedTag )
	{
		// If we forced a send of IDENT, don't trigger an interrupt	
		fReadyToIssueInterrupt = FALSE;
		m_nOutstandingIdentTag = 0;
		
		debug("Publish Identified properties\n");

		// Publish IDENT properties - NOTE: the ATADevice publishes its own properties
		// when it receives an IDENT response, but those values affect how the device
		// appears to the OS. We store the actual values here that are internal just to this
		// kext.

#if defined(__BIG_ENDIAN__)
		// The identify device info needs to be byte-swapped on ppc (big-endian) 
		// systems becuase it is data that is produced by the drive, read across a 
		// 16-bit little-endian PCI interface, directly into a big-endian system.
		// Regular data doesn't need to be byte-swapped because it is written and 
		// read from the host and is intrinsically byte-order correct.	
		swapBytes16( pDeviceIdentifyData, kIDBufferBytes);
#else /* __LITTLE_ENDIAN__ */
		// Swap the strings in the identify data.
		swapBytes16( &pDeviceIdentifyData[46], 8);   // Firmware revision
		swapBytes16( &pDeviceIdentifyData[54], 40);  // Model number
		swapBytes16( &pDeviceIdentifyData[20], 20);  // Serial number
#endif
		// the 512 byte buffer should contain the correctly byte-ordered
		// raw identity info from the device at this point. 

		if( IOATADevConfig::sDriveSupports48BitLBA( ( const UInt16*) pDeviceIdentifyData16 ) )
		{
			UInt32 upperLBA, lowerLBA;

			// Determine the number of sectors
			IOATADevConfig::sDriveExtendedLBASize(   &upperLBA, &lowerLBA, ( const UInt16*) pDeviceIdentifyData16 );
			
			NumSectors = ( ((UInt64) upperLBA) << 32) | ((UInt64) lowerLBA );
			
			OSNumber* extendedCapacity = OSNumber::withNumber( NumSectors, 64 );
			setProperty(IDENT_CAPACITY_PROPERTY, (OSObject *) extendedCapacity);
			extendedCapacity->release();
			
			if ( m_IdentifiedCapacity && (NumSectors != m_IdentifiedCapacity) )
			{
				debugError("Device's capacity has changed!!!!\n");

				// Forcibly remove the target from the list
				m_pProvider->remove_target(m_target.nTargetNumber);
			}

			m_IdentifiedCapacity = NumSectors;
			debug("Capacity: %llu\n", m_IdentifiedCapacity);
		}
		else
		{
			// TODO: Handle 24bit LBA sector size
		}
	
		// Model number runs from byte 54 to 93 inclusive. Terminate byte 94 with a NULL
		pDeviceIdentifyData[94] = 0;
		pModelNum = OSString::withCString((const char*) &pDeviceIdentifyData[54]);
		debug("Model: %s\n", &pDeviceIdentifyData[54]);
		pPrevModelNum = (OSString*) getProperty(IDENT_MODEL_PROPERTY);
		if ( pPrevModelNum )
		{
			if ( !pPrevModelNum->isEqualTo(pModelNum) )
				debugError("Device's Model changed\n");
		}
		setProperty(IDENT_MODEL_PROPERTY, pModelNum);
		pModelNum->release();

		// serial number runs from byte 20 to byte 39 inclusive. Terminate byte 40 with a NULL
		pDeviceIdentifyData[40] = 0;
		pSerialNum = OSString::withCString( (const char*) &pDeviceIdentifyData[20]);
		debug("Serial: %s\n", &pDeviceIdentifyData[20]);
		pPrevSerialNum = (OSString*) getProperty(IDENT_SERIAL_PROPERTY);
		if ( pPrevSerialNum )
		{
			if ( !pPrevSerialNum->isEqualTo(pSerialNum) )
				debugError("Device's serial Number changed\n");
		}
		
		setProperty(IDENT_SERIAL_PROPERTY, pSerialNum);
		pSerialNum->release();
	}

	return fReadyToIssueInterrupt;
}


#pragma mark -
#pragma mark Send data

/*---------------------------------------------------------------------------
 * Create a new mbuf and initialise it for our target
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::create_mbuf_for_transfer(mbuf_t* pM, UInt32 Tag, bool fATA)
{
	aoe_atahdr_full* pAoEFullHeader;
	aoe_cfghdr_full* pAoECfgFullHeader;
	aoe_header*	pAoEHeader;
	errno_t	result;
	
	debugVerbose("create_mbuf_for_transfer.......................................................\n");
	
	if ( !pM )
	{
		debugError("pM should not be zero\n");
		return -1;
	}
	
	// Create our mbuf
	result = mbuf_gethdr(MBUF_WAITOK, MBUF_TYPE_DATA, pM);
	if (result != 0)
		return -1;
	
	mbuf_setlen(*pM, fATA ? sizeof(*pAoEFullHeader) : sizeof(*pAoECfgFullHeader));
	mbuf_pkthdr_setlen(*pM, mbuf_len(*pM));	
	mbuf_align_32(*pM, mbuf_len(*pM));

	// Add shortcuts
	pAoEHeader = MTOD(*pM, aoe_header*);
	
	// Fill out the config header
	AOE_HEADER_CLEAR(pAoEHeader);
	pAoEHeader->ah_verflagserr = AOE_HEADER_SETVERFLAGERR(AOE_SUPPORTED_VER, 0, 0);
	pAoEHeader->ah_major = AOE_HEADER_SETMAJOR(m_target.nShelf);
	pAoEHeader->ah_minorcmd = AOE_HEADER_SETMINORCMD(m_target.nSlot, fATA ? AOE_ATA_COMMAND : AOE_CFG_COMMAND);
	pAoEHeader->ah_tag[0] = AOE_HEADER_SETTAG1(Tag);
	pAoEHeader->ah_tag[1] = AOE_HEADER_SETTAG2(Tag);

	return 0;
}




/*---------------------------------------------------------------------------
 * Force a packet out an interface (this will be called from the User interface program)
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::force_packet_send(ForcePacketInfo* pForcedPacketInfo)
{
	aoe_atahdr_full* pAoEFullATAHeader;
	aoe_cfghdr_full* pAoEFullCfgHeader;
	aoe_header*	pAoEHeader;
	aoe_atahdr* pATACfg;
	aoe_cfghdr* pAoECfg;
	UInt32	Tag;
	errno_t	result;
	mbuf_t	m;
	
	debug("AOE_CONTROLLER_NAME::force_packet_send\n");
	
	// Create our mbuf (the size will vary depending on the type of packet we are going to send)
	result = mbuf_gethdr(MBUF_WAITOK, MBUF_TYPE_DATA, &m);
	if (result != 0)
	{
		debugError("Failed to create mbuf\n");
		return -1;
	}
	
	if ( NULL==m_pProvider )
		return -1;

	if ( pForcedPacketInfo->fATA )
	{
		mbuf_setlen(m, sizeof(*pAoEFullATAHeader));
		mbuf_pkthdr_setlen(m, sizeof(*pAoEFullATAHeader));
		
		mbuf_align_32(m, sizeof(*pAoEFullATAHeader));
		pAoEFullATAHeader = MTOD(m, aoe_atahdr_full*);
		
		// Add shortcuts
		pAoEHeader = &(pAoEFullATAHeader->aoe);
		pATACfg = &(pAoEFullATAHeader->ata);
		
		// Copy across AoE header info
		*pAoEHeader = pForcedPacketInfo->AoEhdr;
		*pATACfg = pForcedPacketInfo->ATAhdr;
		
		debug("AFlags=%#x Err=%#x SectorCount=%#x Status=%#x lba0=%#x lba1=%#x lba2=%#x lba3=%#x lba4=%#x lba5=%#x\n",
			  AOE_ATAHEADER_GETAFLAGS(pATACfg),
			  AOE_ATAHEADER_GETERR(pATACfg),
			  AOE_ATAHEADER_GETSCNT(pATACfg),
			  AOE_ATAHEADER_GETSTAT(pATACfg),
			  AOE_ATAHEADER_GETLBA0(pATACfg),
			  AOE_ATAHEADER_GETLBA1(pATACfg),
			  AOE_ATAHEADER_GETLBA2(pATACfg),
			  AOE_ATAHEADER_GETLBA3(pATACfg),
			  AOE_ATAHEADER_GETLBA4(pATACfg),
			  AOE_ATAHEADER_GETLBA5(pATACfg));
	}
	else
	{
		mbuf_setlen(m, sizeof(*pAoEFullCfgHeader));
		mbuf_pkthdr_setlen(m, sizeof(*pAoEFullCfgHeader));
		
		mbuf_align_32(m, sizeof(*pAoEFullCfgHeader));
		pAoEFullCfgHeader = MTOD(m, aoe_cfghdr_full*);
		
		// Add shortcuts
		pAoEHeader = &(pAoEFullCfgHeader->aoe);
		pAoECfg = &(pAoEFullCfgHeader->cfg);
		
		// Copy across AoE header info
		*pAoEHeader = pForcedPacketInfo->AoEhdr;
		*pAoECfg = pForcedPacketInfo->CFGhdr;
		
		debug("Buf count=%#x Firmware=%x Sector=%#x AoE=%#x CCmd=%#x Length=%#x\n",
			  AOE_CFGHEADER_GETBCOUNT(pAoECfg),
			  AOE_CFGHEADER_GETFVERSION(pAoECfg),
			  AOE_CFGHEADER_GETSCOUNT(pAoECfg),
			  AOE_CFGHEADER_GETAOEVER(pAoECfg),
			  AOE_CFGHEADER_GETCCMD(pAoECfg),
			  AOE_CFGHEADER_GETCSLEN(pAoECfg));
	}
	
	debug("GetVer=%#x Flags=%x Err=%#x Major=%#x Minor=%#x Cmd=%#x TAG=%#x\n",
		  AOE_HEADER_GETVER(pAoEHeader),
		  AOE_HEADER_GETFLAG(pAoEHeader),
		  AOE_HEADER_GETERR(pAoEHeader),
		  AOE_HEADER_GETMAJOR(pAoEHeader),
		  AOE_HEADER_GETMINOR(pAoEHeader),
		  AOE_HEADER_GETCMD(pAoEHeader),
		  AOE_HEADER_GETTAG(pAoEHeader));
	
	Tag = AOE_HEADER_GETTAG(pAoEHeader);
	
	// Send packet out our interface
	if ( pForcedPacketInfo->fATA )
		return m_pProvider->send_ata_packet(this, m, Tag, get_target_info());
	else
		return m_pProvider->send_aoe_packet(this, m, Tag, get_target_info());
}





/*---------------------------------------------------------------------------
 * Set our targets config string
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::set_config_string(const char* pszString, int nLength)
{
	aoe_cfghdr_full* pAoEFullCfgHeader;
	aoe_cfghdr* pCFGhdr;
	UInt32 Tag;
	mbuf_t m;

	debugVerbose("AOE_CONTROLLER_NAME::[%d.%d] set_config_string: %s\n", m_target.nShelf, m_target.nSlot, pszString);

	++nLength;		// copy across NULL termination as well

	if ( NULL==m_pProvider )
		return -1;

	// Initialise our mbuf
	Tag = m_pProvider->next_tag();
	if ( 0!=create_mbuf_for_transfer(&m, Tag, FALSE) )
		return -1;
	pAoEFullCfgHeader = MTOD(m, aoe_cfghdr_full*);
	
	// Add shortcuts
	pCFGhdr = &(pAoEFullCfgHeader->cfg);
	
	AOE_CFGHEADER_CLEAR(pCFGhdr);

	// Write data to mbuf
	pCFGhdr->ac_scnt_aoe_ccmd = AOE_HEADER_SETSECTOR_CMD(m_nMaxSectorsPerTransfer, CONFIG_STR_FORCE_SET);
	pCFGhdr->ac_cslen = AOE_HEADER_SETCSTRLEN(nLength);

	// Copy string to external storage (it may be too large to just copy into the mbufs data region)
	strncpy(m_aConfigString, pszString, nLength);
	attach_ext_to_mbuf(&m, m_aConfigString, nLength);
					   
	//--------------------------//
	// Output data to interface //
	//--------------------------//
	debugVerbose("Sending Cfg data out (Tag=%#x) str=%s len=%d\n", Tag, m_aConfigString, nLength);

	return m_pProvider->send_aoe_packet(this, m, Tag, get_target_info());
}






/*---------------------------------------------------------------------------
 * Attaches external data to an mbuf. This is mostly used for writing large amounts of data
 * to the AoE target device
 ---------------------------------------------------------------------------*/
//#define DEBUG_MBUF_ATTACH
int AOE_CONTROLLER_NAME::attach_ext_to_mbuf(mbuf_t* pm, caddr_t MBufExtData, IOByteCount Size)
{
	mbuf_t NewMBuf;
	errno_t err;

	err = mbuf_allocpacket(MBUF_WAITOK, Size, 0, &NewMBuf);
	if ( 0!=err )
		debugError("Trouble creating mbuf (err=%d)\n", err);
	
	err = mbuf_copyback(NewMBuf, 0, Size, MBufExtData, MBUF_WAITOK);

	if ( 0!=err )
	{
		debugError("Trouble attaching cluster to our mbuf (err=%d)\n", err);
		return -1;
	}
	
	if ( NULL==NewMBuf )
	{
		debugError("NewMBuf is NULL\n");
		return -1;
	}
	
	if ( MBUF_TYPE_FREE==mbuf_type(NewMBuf) )
	{
		debugError("Trouble creating mbuf for cluster. Type = %#x\n", mbuf_type(NewMBuf));
		return -1;		// no point attaching the mbuf, we'll panic after it's sent because the mbuf will be freed again
	}
	
	// Update mbuf with the correct length (no attachcluster doesn't do this)
	mbuf_setlen(NewMBuf, Size);
	
	#ifdef DEBUG_MBUF_ATTACH
	debug("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
	debug("BEFORE APPEND:\n");
	debug("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
	debug("mbuf=%#x : nextpkt=%#x | next=%#x | type=%#x | flag=%#x | length = %d | hdrlen = %d\n", *pm, mbuf_nextpkt(*pm), mbuf_next(*pm), mbuf_type(*pm), mbuf_flags(*pm), mbuf_len(*pm), mbuf_pkthdr_len(*pm));
	debug("newmbuf=%#x : nextpkt=%#x | next=%#x | type=%#x | flag=%#x | length = %d\n", NewMBuf, mbuf_nextpkt(NewMBuf), mbuf_next(NewMBuf), mbuf_type(NewMBuf), mbuf_flags(NewMBuf), mbuf_len(NewMBuf));
	debug("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
	#endif

	err = mbuf_setnext(*pm, NewMBuf);
	
	if ( 0!=err )
	{
		debugError("Trouble chaining mbufs (err=%d)\n", err);
		mbuf_free(NewMBuf);
		return -1;
	}
	else
	{
		// Update our header with the total length of our chain
		mbuf_pkthdr_setlen(*pm, Size+mbuf_pkthdr_len(*pm));
	}
	
	#ifdef DEBUG_MBUF_ATTACH
	debug("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
	debug("AFTER APPEND:\n");
	debug("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
	debug("mbuf=%#x : nextpkt=%#x | next=%#x | type=%#x | flags=%#x | length = %d | hdrlen = %d\n", *pm, mbuf_nextpkt(*pm), mbuf_next(*pm), mbuf_type(*pm), mbuf_flags(*pm), mbuf_len(*pm), mbuf_pkthdr_len(*pm));
	debug("&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&\n");
	#endif
	
	return 0;
}


#pragma mark -
#pragma mark subclass overrides


/*---------------------------------------------------------------------------
 * provideBusInfo - provide information on the bus capability
 ---------------------------------------------------------------------------*/

IOReturn AOE_CONTROLLER_NAME::provideBusInfo(IOATABusInfo* infoOut)
{
	debug("AOE_CONTROLLER_NAME::provideBusInfo\n");

	if( infoOut == 0)
	{
		debugError("AOE_CONTROLLER_NAME nil pointer in provideBusInfo\n");
		return -1;
	}
	
	// Set to default
	infoOut->zeroData();

	infoOut->setSocketType( kInternalATASocket );
	
	// Allow Extended LBA if the device supports it
	infoOut->setExtendedLBA( true );

	// AoEController only ever has one unit attached to it
	infoOut->setUnits(1);

	infoOut->setPIOModes( AOE_SUPPORTED_PIO_MODES );
	infoOut->setDMAModes( AOE_SUPPORTED_DMA_MODES );
	infoOut->setUltraModes( AOE_SUPPORTED_ULTRA_DMA_MODES );
	infoOut->setDMAQueued(FALSE);
	infoOut->setMaxBlocksExtended( m_nMaxTransferSize/kATADefaultSectorSize );		// maximum number of blocks allowed in a single transfer of data
	
	// ATA protocol service driver doesn't support overlapped anyway. We just ensure that here as well
	infoOut->setOverlapped(FALSE);		

	return kATANoErr;
}




/*---------------------------------------------------------------------------
 *
 *	select the bus timing configuration for a particular device
 *  should be called by device driver after doing an identify device command and working out 
 *	the desired timing configuration.
 *	should be followed by a Set Features comand to the device to set it in a 
 *	matching transfer mode.
 *
 *--------------------------------------------------------------------------*/

IOReturn AOE_CONTROLLER_NAME::selectConfig( IOATADevConfig* pConfigRequest, UInt32 unitNumber)
{
	debug("AOE_CONTROLLER_NAME::selectConfig\n");

	// param check the pointers and the unit number.
	if ( pConfigRequest == 0 || unitNumber > 1 )
	{
		debugError("AOE_CONTROLLER_NAME bad param in setConfig\n");
		return -1;
	}

	debug("selectConfig: PACKET CONFIG %#x\n", pConfigRequest->getPacketConfig());
	debug("selectConfig: DMA CYCLE TIME %#x\n", pConfigRequest->getDMACycleTime());
	debug("selectConfig: PIO CYCLE TIME %#x\n", pConfigRequest->getPIOCycleTime());
	
	debug("selectConfig: DMA MODE %#x\n", pConfigRequest->getDMAMode());
	debug("selectConfig: PIO MODE %#x\n", pConfigRequest->getPIOMode());
	debug("selectConfig: ULTRA DMA MODE %#x\n", pConfigRequest->getUltraMode());
	
	// Check we aren't being told to do anything we can't
	if ( pConfigRequest->getUltraMode() && ((pConfigRequest->getUltraMode() & AOE_SUPPORTED_ULTRA_DMA_MODES ) == 0x00) )
	{
		debugError("AoE ULTRA mode not supported\n");
		return kATAModeNotSupported;
	}

	if ( pConfigRequest->getDMAMode() && ((pConfigRequest->getDMAMode() & AOE_SUPPORTED_DMA_MODES ) == 0x00)  )
	{
		debugError("AoE DMA mode not supported\n");
		return kATAModeNotSupported;
	}
	
	// all config requests must include a PIO mode
	if( pConfigRequest->getPIOMode() && ((pConfigRequest->getPIOMode() & AOE_SUPPORTED_PIO_MODES ) == 0x00) )
	{
		debugError("AoE PIO mode not supported\n");
		return kATAModeNotSupported;
	}

	// Update the device with the packet config timing info
	_devInfo[unitNumber].packetSend = pConfigRequest->getPacketConfig();
	debug("AoE setConfig packetConfig = %ld\n", _devInfo[unitNumber].packetSend );
	debug("AoE PIO cycle time is = %ld\n", pConfigRequest->getPIOCycleTime());

	return kATANoErr;
}

/*---------------------------------------------------------------------------
 *
 * Find out what the current bus timing configuration is for a particular device. 
 *
 * For AoE, we mostly use fixed values that are not dependent on the devices attached
 ---------------------------------------------------------------------------*/

IOReturn AOE_CONTROLLER_NAME::getConfig( IOATADevConfig* configRequest, UInt32 unitNumber)
{
	debug("AOE_CONTROLLER_NAME::getConfig\n");
	
	// param check the pointers and the unit number.
	if( configRequest == 0 || unitNumber > 1 )
	{
		debugError("HeathrowATA bad param in getConfig\n");
		return -1;
	}

	configRequest->setDMAMode(AOE_SUPPORTED_DMA_MODES);
	configRequest->setDMACycleTime(0);
	configRequest->setUltraMode(AOE_SUPPORTED_ULTRA_DMA_MODES);

	configRequest->setPIOMode(AOE_SUPPORTED_PIO_MODES);
	configRequest->setPIOCycleTime(500);		//ns

	configRequest->setPacketConfig( _devInfo[unitNumber].packetSend );

	return kATANoErr;
}


/*---------------------------------------------------------------------------
 *
 * Initialise the pointers to the ATA task file registers during start() time.
 *
 ---------------------------------------------------------------------------*/

bool AOE_CONTROLLER_NAME::configureTFPointers(void)
{
	debug("AOE_CONTROLLER_NAME::configureTFPointers\n");

	// None of these registers are used in AoE driver.
	// Just init as zero so we panic and can find the source of the access
	_tfDataReg = NULL;
	_tfFeatureReg = NULL;
	_tfSCountReg = NULL;
	_tfSectorNReg = NULL;
	_tfCylLoReg = NULL;
	_tfCylHiReg = NULL;
	_tfSDHReg = NULL;
	_tfStatusCmdReg = NULL;
	_tfAltSDevCReg = NULL;

	return TRUE;
}


/*---------------------------------------------------------------------------
 *	scan the bus to see if devices are attached. 
 *
 * For AoE, we always return that there is a device on our controller's bus
 * This function is only really used to complete the start routine of
 * IOATAController, and not really critical
 ---------------------------------------------------------------------------*/

UInt32 AOE_CONTROLLER_NAME::scanForDrives( void )
{
	UInt32 unitsFound;
	
	debug("AOE_CONTROLLER_NAME::scanForDrives\n");

	// We always report there is a device for and AOE_CONTROLLER
	unitsFound = 1;
	
	_devInfo[0].type = kATADeviceType;
	_devInfo[0].packetSend = kATAPIDRQFast;  // According to IOATAController::scanForDrives, this is the safest default setting
	
	// enforce ATA device selection protocol before issuing the next command.
	_selectedUnit = kATAInvalidDeviceID;

	return unitsFound;
}


/*---------------------------------------------------------------------------
 * Set the outgoing packet with the info that would normally be placed in our
 * registers
 ---------------------------------------------------------------------------*/
IOReturn AOE_CONTROLLER_NAME::registerAccess(bool isWrite)
{
	UInt32	RegAccessMask;
	bool isExtLBA;
	IOReturn err;
	IOExtendedLBA* extLBA;
	UInt8 Error;
	UInt8 Sector_Count;
	UInt8 Status;
	UInt8 lba[6];
	UInt16 Error16;
	UInt16 Sector_Count16;
	UInt16 lba_low16;
	UInt16 lba_mid16;
	UInt16 lba_high16;

	debug("AOE_CONTROLLER_NAME::registerAccess(%d)\n", isWrite);

	debugError("Check handling of 48bitLBA\n");

	RegAccessMask = _currentCommand->getRegMask();
	isExtLBA =  m_fExtendedLBA;			// ( _currentCommand->getFlags() & mATAFlag48BitLBA ) -- BUG in this check don't use_currentCommand->getFlags() & mATAFlag48BitLBA;
	err = kATANoErr;
	extLBA = _currentCommand->getExtendedLBA();

	if ( isWrite )
	{
		debugError("registerAccess() - WRITE not supported\n");
		return kIOReturnUnsupported;
	}

	// We look at the last received command to determine these values
	if ( NULL == m_pReceivedATAHeader )
	{
		debugError("Invalid ATA header in registerAccess\n");
		return kIOReturnUnsupported;
	}
	
	Error = AOE_ATAHEADER_GETERR(m_pReceivedATAHeader);
	Sector_Count = AOE_ATAHEADER_GETSCNT(m_pReceivedATAHeader);
	Status = AOE_ATAHEADER_GETSTAT(m_pReceivedATAHeader);
	lba[0] = AOE_ATAHEADER_GETLBA0(m_pReceivedATAHeader);
	lba[1] = AOE_ATAHEADER_GETLBA1(m_pReceivedATAHeader);
	lba[2] = AOE_ATAHEADER_GETLBA2(m_pReceivedATAHeader);
	lba[3] = AOE_ATAHEADER_GETLBA3(m_pReceivedATAHeader);
	lba[4] = AOE_ATAHEADER_GETLBA4(m_pReceivedATAHeader);
	lba[5] = AOE_ATAHEADER_GETLBA5(m_pReceivedATAHeader);
	
	// Convert to 16-bit values
	Error16 = Error;
	Sector_Count16 = Sector_Count;
	lba_low16 =  (lba[0]<<8) | (lba[3]);
	lba_mid16 =  (lba[1]<<8) | (lba[4]);
	lba_high16 = (lba[2]<<8) | (lba[5]);

	// Print data for debug
	if ( isExtLBA )
	{
		debugVerbose("~~~~~~~~~~~~~~~~\n");
		debugVerbose("ATA TF Registers (READS):\n");
		debugVerbose("ERROR  = %#x\n", Error16);
		debugVerbose("STATUS  = %#x\n", Status);
		debugVerbose("SECTOR COUNT = %#x\n", Sector_Count16);
		debugVerbose("CYCL LOW     = %#x\n", lba_low16);
		debugVerbose("CYCL MID     = %#x\n", lba_mid16);
		debugVerbose("CYCL HIGH    = %#x\n", lba_high16);
		debugVerbose("***************\n");
	}
	else
	{
		debugVerbose("~~~~~~~~~~~~~~~~\n");
		debugVerbose("ATA TF Registers (READS):\n");
		debugVerbose("ERROR  = %#x\n", Error);
		debugVerbose("STATUS  = %#x\n", Status);
		debugVerbose("SECTOR COUNT = %#x\n", Sector_Count);
		debugVerbose("CYCL LOW     = %#x\n", lba[0]);
		debugVerbose("CYCL MID     = %#x\n", lba[1]);
		debugVerbose("CYCL HIGH    = %#x\n", lba[2]);
		debugVerbose("***************\n");
	}

	// error/features register
	if (RegAccessMask & mATAErrFeaturesValid)
		if(isExtLBA )
			extLBA->setFeatures16(Error16);
		else
			_currentCommand->setFeatures(Error);

	// sector count register
	if (RegAccessMask & mATASectorCntValid)
		if(isExtLBA )
			extLBA->setSectorCount16(Sector_Count16);
		else
			_currentCommand->setSectorCount(Sector_Count);
	
	// sector number register
	if (RegAccessMask & mATASectorNumValid)
		if(isExtLBA )
			extLBA->setLBALow16(lba_low16);
		else
			_currentCommand->setSectorNumber(lba[0]);
	
	// cylinder low register
	if (RegAccessMask & mATACylinderLoValid)
		if(isExtLBA )
			extLBA->setLBAMid16(lba_mid16);
		else
			_currentCommand->setCylLo(lba[1]);

	// cylinder high register
	if (RegAccessMask & mATACylinderHiValid)
		if(isExtLBA )
			extLBA->setLBAHigh16(lba_high16);
		else
			_currentCommand->setCylHi(lba[2]);
	
	// status/command register
	if (RegAccessMask & mATAStatusCmdValid)
		_currentCommand->setCommand(Status);

	// ataTFSDH register
	if (RegAccessMask & mATASDHValid)
	{
		debugWarn("Reading Device status in registerAccess\n");
		//_currentCommand->setDevice_Head( *_tfSDHReg );
	}
	
	// alternate status/device control register
	if (RegAccessMask & mATAAltSDevCValid)
	{
		debugWarn("Reading Alternate status in registerAccess\n");
		//_currentCommand->setControl( *_tfAltSDevCReg );
	}
	
	// data register...
	if (RegAccessMask & mATADataValid)
	{
		debugWarn("Reading Data in registerAccess\n");
		//_currentCommand->setDataReg( *_tfDataReg );
	}

	return err;
}

IOReturn AOE_CONTROLLER_NAME::selectDevice( ataUnitID unit )
{
	debug("selectDevice(%d)\n", unit);

	// Nothing to do here for AoE, we just accept the device that is selected (there is only one device per controller anyway)
	// successful device selection.
	_selectedUnit = unit;

	return kATANoErr;
}


/*---------------------------------------------------------------------------
// This is the function that is called when we have received a response.
// For ATA, this is usually handled with an interrupt, for AoE, it's when
// we have received the command
 ---------------------------------------------------------------------------*/
 IOReturn AOE_CONTROLLER_NAME::handleDeviceInterrupt(void)
{
	debugVerbose("AOE_CONTROLLER_NAME::handleDeviceInterrupt()\n");

	// make sure there's a command active
	if( !_currentCommand )
	{
		debug("IOATA Device Int no command active\n");
		return kATADevIntNoCmd;
	}
	
	// Continue with the state machine
	return asyncIO();
}

IOReturn AOE_CONTROLLER_NAME::synchronousIO(void)
{
	debugError("synchronousIO() - REVIEW THIS FUNCTION as it's not implemented\n");

	// This function isn't called, so there is no support for it at the moment.

	return kIOReturnUnsupported;
}


/*---------------------------------------------------------------------------
 * This function isn't called, so there is no support for it at the moment.
 * If we had to handle it, just call issueCommand, but need to check handling of read/writes though...
 * For ATA, this is usually handled with an interrupt, for AoE, it's when
 * we have received the command
 ---------------------------------------------------------------------------*/
IOReturn AOE_CONTROLLER_NAME::handleRegAccess( void )
{
	// Place a note here so we can catch when its called
	debugError("handleRegAccess() - REVIEW THIS FUNCTION\n");

	return kIOReturnUnsupported;

/*	BaseClass code:
	IOReturn err = kATANoErr;
	
	// select the desired device
	err = selectDevice( _currentCommand->getUnit() );
	if( err )
	{
		_currentCommand->state = IOATAController::kATAComplete;
		completeIO( err );
		return err;
	}
	
	bool isWrite = (_currentCommand->getFlags() & mATAFlagIOWrite) ? true : false;
	
	err = registerAccess( isWrite );
	_currentCommand->state = IOATAController::kATAComplete;
	completeIO( err );
	return err;
*/
}


/*---------------------------------------------------------------------------
 Handle the status of a previously completed command. This is the time
 to pass any error information back to our caller
 ---------------------------------------------------------------------------*/
IOReturn AOE_CONTROLLER_NAME::asyncStatus(void)
{
	IOReturn err;
	UInt8 error;
	UInt8 status;

	if ( m_pReceivedATAHeader )
	{
		err = kATANoErr;
		error = 0x0;
		status = AOE_ATAHEADER_GETSTAT(m_pReceivedATAHeader);

		debugVerbose("AOE_CONTROLLER_NAME::asyncStatus()\n");

		// if err bit is set, read the error register
		if( status & mATAError )
		{
			error = AOE_ATAHEADER_GETERR(m_pReceivedATAHeader);

			err = kATADeviceError;

			// look for error results in the TF 
			if( _currentCommand->getFlags() & (mATAFlagTFAccess | mATAFlagTFAccessResult) )
				registerAccess( false );
			
			// if this command returns results in registers on successful completion
			// read them now. 
		}
		else if( _currentCommand->getFlags() & mATAFlagTFAccessResult )
			registerAccess( false );

		if ( error )
			debugError("asyncStatus() - status=%#x, error=%#x\n", status, error);
		else
			debugVerbose("asyncStatus() - status=%#x, error=%#x\n", status, error);

		_currentCommand->setEndResult(status, error);
		
		// Pass on any errors that may have occurs in previous writes
		if ( (m_PreviousWriteStatus&mATAError) && m_PreviousWriteError )
		{
			debugError("asyncStatus() - status=%#x, error=%#x [PREVIOUS ERROR]\n", m_PreviousWriteStatus, m_PreviousWriteError);
			_currentCommand->setEndResult(m_PreviousWriteStatus, m_PreviousWriteError);
			err = kATADeviceError;
		}
	}
	else
		debugError("m_pReceivedATAHeader is NULL, cannot complete asyncStatus\n");

	// These aren't required anymore
	m_PreviousWriteStatus = 0;
	m_PreviousWriteError = 0;

	return err;	
}


/*---------------------------------------------------------------------------
 * Complete any data reads
 ---------------------------------------------------------------------------*/

IOReturn AOE_CONTROLLER_NAME::asyncData(void)
{
	debugVerbose("AOE_CONTROLLER_NAME::asyncData\n");

	// For AoE, asyncData is only used for data reads
	return completeDataRead(FALSE);
}


IOReturn AOE_CONTROLLER_NAME::completeDataRead(bool* pfInterrupt)
{
	UInt16* pReceiveNetworkData;
	IOByteCount bytesRemaining;
	IOMemoryDescriptor* descriptor;
	IOByteCount xfrPosition, thisPass, bufferBytes;
	int nMaxTransferSize = m_nMaxSectorsPerTransfer*kATADefaultSectorSize;
	
	// first check and see if data is remaining for transfer
	bytesRemaining = _currentCommand->getByteCount() - _currentCommand->getActualTransfer();
	
	debug("AOE_CONTROLLER_NAME::asyncData (%d bytes remaining)\n", bytesRemaining);
	
	// nothing to do
	if(bytesRemaining < 1)
	{
		_currentCommand->state = kATAStatus;
		return kATANoErr;
	}
	
	// Check valid MBuf
	if ( NULL==m_pReceivedATAHeader )
	{
		debugError("m_pReceivedATAHeader is uninitialised");
		_currentCommand->state = kATAStatus;
		return kATADeviceError;
	}
	
	if( _currentCommand->getFlags() & mATAFlagIOWrite )
	{
		debugError("Writes not handled in this function for AoE\n");
		return -1;
	}
	
	// let the timeout through
	if ( checkTimeout() )
	{			
		_currentCommand->state = kATAStatus;
		return kATATimeoutErr;
	}
	
	descriptor = _currentCommand->getBuffer();
	
	// The IOMemoryDescriptor may not have a logical address mapping (aka, 
	// virtual address) within the kernel address space. This poses a problem 
	// when doing PIO data transfers, which means the CPU is reading/writing 
	// the device data register and moving data to/from a memory address in the 
	// host. This requires some kind of logical address. However, in protected 
	// memory systems it is costly to map the client's buffer and give it a 
	// virtual address.
	
	// so all PIO data is double buffered to a chunk of mapped and wired memory
	// in the kernel space. IOMemoryDescriptor provides methods to read/write 
	// the physical address it contains.
	
	debug("BASE TAG=%#x | RECEIVED TAG=%#x | position=%d * %d=%d\n", m_unReadBaseTag, m_unReceivedTag, (m_unReceivedTag-m_unReadBaseTag), _currentCommand->getTransferChunkSize(), (m_unReceivedTag-m_unReadBaseTag)*_currentCommand->getTransferChunkSize());
	
	//	xfrPosition = _currentCommand->getPosition() + _currentCommand->getActualTransfer();
	xfrPosition = (m_unReceivedTag-m_unReadBaseTag) * _currentCommand->getTransferChunkSize();
	
	thisPass = bytesRemaining;
	
	// pare down to the number of bytes between interrupts 
	// to be transferred. Do this chunk, then pend the 
	// next IRQ if bytes remain.
	if( thisPass > _currentCommand->getTransferChunkSize() )
		thisPass = _currentCommand->getTransferChunkSize();
	
	// Read data from the first mbuf
	pReceiveNetworkData = &m_pReceivedATAHeader->aa_Data[0];
	if ( thisPass > 0 )
	{
		bufferBytes = m_unReceivedATADataSize;
		
		// We need to check two things here:
		// 1/ We copy no more data than we are expecting in this packet
		bufferBytes = MIN(nMaxTransferSize, bufferBytes);
		// 2/ No more data than we are expecting in the total transfer
		bufferBytes = MIN(bytesRemaining, bufferBytes);
		
		// Keep track of how much data we can have left to transfer
		nMaxTransferSize -= bufferBytes;
		
		debug("This read/write: Position=%d, Size=%d\n", xfrPosition, bufferBytes);
		//debug("Copying from Mbuf (base=%#x)\n", pReceiveNetworkData);
		debug("'Reading' data:\n");
		print_mem((UInt8*)pReceiveNetworkData, bufferBytes);
		
		descriptor->writeBytes(xfrPosition, (void*)(pReceiveNetworkData), bufferBytes);
		
		// update indicators
		_currentCommand->setActualTransfer(_currentCommand->getActualTransfer() + bufferBytes);	
		bytesRemaining -= bufferBytes;
		xfrPosition += bufferBytes;
	}
	
	// Copy additional data from chained mbufs
	while ( m_ReceivedMBufCont )
	{
		debug("\tAdditional mbufs in chain. Continuing read...\n");
		
		thisPass = mbuf_len(m_ReceivedMBufCont);
		bufferBytes = (thisPass > _doubleBuffer.bufferSize )? _doubleBuffer.bufferSize : thisPass;

		// We need to check two things here:
		// 1/ We copy no more data than we are expecting in this packet
		bufferBytes = MIN(nMaxTransferSize, bufferBytes);
		// 2/ No more data than we are expecting in the total transfer
		bufferBytes = MIN(bytesRemaining, bufferBytes);

		// Keep track of how much data we can have left to transfer
		nMaxTransferSize -= bufferBytes;

		debug("\tThis read/write: Position=%d, Size=%d\n", xfrPosition, bufferBytes);
		//debug("\Copying from Mbuf (base=%#x)\n", MTOD(m_ReceivedMBufCont, void*));
		
		print_mem(MTOD(m_ReceivedMBufCont, UInt8*), bufferBytes);
		descriptor->writeBytes(xfrPosition, MTOD(m_ReceivedMBufCont, void*), bufferBytes);
		
		// update indicators
		_currentCommand->setActualTransfer(_currentCommand->getActualTransfer() + bufferBytes);	
		bytesRemaining -= bufferBytes;
		xfrPosition += bufferBytes;
		m_ReceivedMBufCont = mbuf_next(m_ReceivedMBufCont);
	}
	
	if ( m_nReadWriteRepliesRequired )
	{
		// next IRQ means more data
		debug("m_nReadWriteRepliesRequired=%d. Setting state to continue next time\n", m_nReadWriteRepliesRequired);
		
		_currentCommand->state = kATADataTx;
		if ( pfInterrupt )
			*pfInterrupt = FALSE;
	}
	else
	{
		// next IRQ is a status check for completion
		debug("0 bytes remaining for next transfer. Setting state to check status and move on\n");
		_currentCommand->state = kATAStatus;
		if ( pfInterrupt )
			*pfInterrupt = TRUE;
	}
	
	return kATANoErr;
}



/*---------------------------------------------------------------------------
 * Attach data from our storage data buffer to the outoing mbuf
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::append_write_data(mbuf_t* pm)
{
	IOByteCount bytesRemaining;
	IOMemoryDescriptor* descriptor;
	IOByteCount xfrPosition, thisPass, bufferBytes;
	caddr_t MBufExtData;

	if ( NULL==_currentCommand )
	{
		debugError("Invalid command in append_write_data\n");
		return -1;
	}
	
	// first check and see if data is remaining for transfer
	bytesRemaining = _currentCommand->getByteCount() - _currentCommand->getActualTransfer();
	
	debug("AOE_CONTROLLER_NAME::append_write_data (%d bytes remaining)\n", bytesRemaining);
	
	// nothing to do
	if(bytesRemaining < 1)
		return 0;

	descriptor = _currentCommand->getBuffer();

	xfrPosition = _currentCommand->getPosition() + _currentCommand->getActualTransfer();
	
	thisPass = bytesRemaining;
	
	// pare down to the number of bytes between interrupts 
	// to be transferred. Do this chunk, then pend the 
	// next IRQ if bytes remain.
	if( thisPass > _currentCommand->getTransferChunkSize() )
		thisPass = _currentCommand->getTransferChunkSize();
	
	// Update size of transfer and remaining data to transfer
	while( thisPass > 0 )
	{		
		bufferBytes = (thisPass > _doubleBuffer.bufferSize )? _doubleBuffer.bufferSize : thisPass;

		debug("Mem transfer remaining=%d. This read/write: Position=%d, Size=%d\n", bytesRemaining, xfrPosition, bufferBytes);

		// Have a look at the memory we will be writing
		MBufExtData = (caddr_t) _doubleBuffer.logicalBuffer+xfrPosition;
		print_mem((UInt8*)_doubleBuffer.logicalBuffer+xfrPosition, bufferBytes);

		// update indicators
		xfrPosition += bufferBytes; 	
		thisPass -= bufferBytes;
		_currentCommand->setActualTransfer(_currentCommand->getActualTransfer() + bufferBytes);	
		bytesRemaining -= bufferBytes;	
	}

	debug("%d bytes remaining for next WRITE.\n", bytesRemaining);

	return attach_ext_to_mbuf(pm, MBufExtData, bufferBytes);
}



void AOE_CONTROLLER_NAME::cluster_free(caddr_t add, u_int size, caddr_t add2)
{
	// memory is not freed after transfer
}


/*---------------------------------------------------------------------------
 *
 ---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------
 * Some useful debug code to check the contents of our packet
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::print_mem(UInt8* pMem, int nSize)
{
#ifndef PRINT_DATA_MEMORY
	nSize = 0;
#endif
	
	const int nBytePerRow = 16;
	int n;
	int nRow, nRows = nSize/nBytePerRow;
	
	if ( nSize )
		debug("print_mem(size=%d, rows=%d, leftover=%d)\n", nSize, nRows, nSize-nRows*nBytePerRow);
	
	for(nRow=0; nRow<nRows; nRow++)
	{
		debugShort("\t%#09x - ", nRow*nBytePerRow); 
		for(n=0; n<nBytePerRow; n++)
			debugShort("%02x ", pMem[nRow*nBytePerRow+n]);
		debugShort("\n");
	}

	// Print any left overs
	if ( nSize && (nSize-nRows*nBytePerRow) )
	{
		debugShort("\t%#09x - ", nRow*nBytePerRow); 
		for(n=0; n<nSize-nRows*nBytePerRow; n++)
			debugShort("%02x ", pMem[nRows*nBytePerRow+n]);
		debugShort("\n");
	}
}


/*---------------------------------------------------------------------------
 * Issue the command by preparing the data in the mbuf and adding any "write"
 * data to the outgoing mbuf cluster
 ---------------------------------------------------------------------------*/
IOReturn AOE_CONTROLLER_NAME::issueCommand(void)
{
	mbuf_t m;
	UInt32 Tag;
	UInt8 AFlags;
	UInt8 Feature;
	UInt8 Sector_Count;
	UInt8 Command;
	UInt8 lba[6];
	aoe_atahdr_full* pAoEFullATAHeader;
	aoe_header*	pAoEHeader;
	aoe_atahdr* pATAhdr;
	int nRet;
	
	nRet = kATANoErr;

	if( _currentCommand == 0 )
	{
		debug("IOATA can't issue nil command\n");
		return kATAUnknownOpcode;
	}

	if ( NULL==m_pProvider )
	{
		debugError("Provider is gone...aborting command\n");
		return kATAErrDevBusy;
	}

	debugVerbose("AOE_CONTROLLER_NAME::issue %s Command (flags=%#x)\n", _currentCommand->getFlags()&mATAFlagIOWrite?"write":"read", _currentCommand->getFlags());

	//---------------------//
	// Initialise our mbuf //
	//---------------------//

	Tag = m_pProvider->next_tag();
	if ( 0!=create_mbuf_for_transfer(&m, Tag, TRUE) )
		return -1;
	pAoEFullATAHeader = MTOD(m, aoe_atahdr_full*);
	
	// If non-zero, remember the tag of this command (this will allow us to sort out read packets that return in the wrong order)
	if ( 0==m_unReadBaseTag )
		m_unReadBaseTag = Tag;
	
	// Add shortcuts
	pAoEHeader = &(pAoEFullATAHeader->aoe);
	pATAhdr = &(pAoEFullATAHeader->ata);
	
	AOE_ATAHEADER_CLEAR(pATAhdr);

	//--------------//
	// Prepare data //
	//--------------//
	
	if ( is_extended_command() )		// if ( _currentCommand->getFlags() & mATAFlag48BitLBA ) -- BUG in this check don't use
	{
		IOExtendedLBA* extLBA;
		
		debugVerbose("48BitLBA Command\n");
		
		extLBA = _currentCommand->getExtendedLBA();
		
		AFlags = AOE_AFLAGS_E;		// extended LBA
		
		Feature = extLBA->getFeatures16() & 0xFF;			// AoE only has 8-bit register
		Sector_Count = extLBA->getSectorCount16() & 0xFF;	// We will never transfer more than 255 sectors (128k)
		Command = extLBA->getCommand();

		lba[3] = (extLBA->getLBALow16() & 0xFF00) >> 8;
		lba[0] = (extLBA->getLBALow16() & 0x00FF);
		lba[4] = (extLBA->getLBAMid16() & 0xFF00) >> 8;
		lba[1] = (extLBA->getLBAMid16() & 0x00FF);
		lba[5] = (extLBA->getLBAHigh16() & 0xFF00) >> 8;
		lba[2] = (extLBA->getLBAHigh16() & 0x00FF);

		// Additional checks
		if ( extLBA->getSectorCount16()&0xFF00 )
			debugError("Sector count is too large for AoE command\n");

		if ( extLBA->getFeatures16()&0xFF00 )
			debugError("Dropping Features as AoE doesn't have 16-bit register\n");
			
		// Print data for debug
		/*debugVerbose("~~~~~~~~~~~~~~~~\n");
		debugVerbose("ATA TF Registers:\n");
		debugVerbose("DEVICE  = %#x\n",  extLBA->getDevice());
		debugVerbose("FEATURE REG  = %#x\n",  extLBA->getFeatures16());
		debugVerbose("SECTOR COUNT = %#x\n",  extLBA->getSectorCount16());
		debugVerbose("CYCL LOW     = %#x\n",  extLBA->getLBALow16());
		debugVerbose("CYCL MID     = %#x\n",  extLBA->getLBAMid16());
		debugVerbose("CYCL HIGH    = %#x\n",  extLBA->getLBAHigh16());
		debugVerbose("COMMAND = %#x\n",  extLBA->getCommand());
		debugVerbose("***************\n");*/
	}
	else
	{
		debugVerbose("non - 48BitLBA Command\n");

		ataTaskFile* tfRegs = _currentCommand->getTaskFilePtr();
	
		if ( NULL==tfRegs )
		{
			debugError("tfRegs is NULL\n");
			return kATAErrUnknownType;
		}

		AFlags = 0;
		
		Feature = tfRegs->ataTFFeatures;
		Sector_Count = tfRegs->ataTFCount;
		Command = tfRegs->ataTFCommand;
		lba[3] = tfRegs->ataTFSDH;		// Device
		lba[0] = tfRegs->ataTFSector;
		lba[1] = tfRegs->ataTFCylLo;
		lba[2] = tfRegs->ataTFCylHigh;
		lba[4] = 0;
		lba[5] = 0;

		// Print data for debug
		/*debugVerbose("~~~~~~~~~~~~~~~~\n");
		debugVerbose("ATA TF Registers:\n");
		debugVerbose("DEVICE  = %#x\n",  tfRegs->ataTFSDH);
		debugVerbose("FEATURE REG  = %#x\n",  tfRegs->ataTFFeatures);
		debugVerbose("SECTOR COUNT = %#x\n",  tfRegs->ataTFCount);
		debugVerbose("CYCL LOW     = %#x\n",  tfRegs->ataTFCylLo);
		debugVerbose("CYCL HIGH    = %#x\n",  tfRegs->ataTFCylHigh);
		debugVerbose("COMMAND = %#x\n",  tfRegs->ataTFCommand);
		debugVerbose("***************\n");*/
	}

	//--------------------//
	// Write data to mbuf //
	//--------------------//
	
	if ( _currentCommand->getFlags()&mATAFlagIOWrite )
		AFlags |= AOE_AFLAGS_W;

	/*debugVerbose("AoE registers:\n");
	debugVerbose("AFlags			= %#x\n", AFlags);
	debugVerbose("Feature			= %#x\n", Feature);
	debugVerbose("Sector_Count		= %#x\n", Sector_Count);
	debugVerbose("Command			= %#x\n", Command);
	debugVerbose("lba[5,4,3,2,1,0]	= %#x %#x %#x %#x %#x %#x\n", lba[5], lba[4], lba[3], lba[2], lba[1], lba[0]);
	debugVerbose("~~~~~~~~~~~~~~~~\n");*/
	
	pATAhdr->aa_aflags_errfeat = AOE_ATAHEADER_SETAFLAGSFEAT(AFlags, Feature);
	pATAhdr->aa_scnt_cmdstat = AOE_ATAHEADER_SETSCNTCMD(Sector_Count, Command);
	pATAhdr->aa_lba0_1 = AOE_ATAHEADER_SETLBA01(lba[0], lba[1]);
	pATAhdr->aa_lba2_3 = AOE_ATAHEADER_SETLBA23(lba[2], lba[3]);
	pATAhdr->aa_lba4_5 = AOE_ATAHEADER_SETLBA45(lba[4], lba[5]);

	//-------------------------------//
	// Append any data to be written //
	//-------------------------------//
	
	if ( _currentCommand->getFlags()&mATAFlagIOWrite )
		if ( -1==append_write_data(&m) )
			nRet = kATAErrDevBusy;
		
	//--------------------------//
	// Output data to interface //
	//--------------------------//
	//debugVerbose("Sending Data out (Tag=%#x)\n", Tag);

	if ( -1==m_pProvider->send_ata_packet(this, m, Tag, get_target_info()) )
		nRet = kATAErrDevBusy;

	return nRet;
}

/*---------------------------------------------------------------------------
 * Resets aren't required for AoE, we just fake it by returning OK
 ---------------------------------------------------------------------------*/
IOReturn AOE_CONTROLLER_NAME::handleBusReset(void)
{
	IOReturn err;
	
	debug("AOE_CONTROLLER_NAME::handleBusReset() - Faking it...\n");
	
	err = kATANoErr;

	executeEventCallouts( kATAResetEvent, kATADevice0DeviceID );
	_currentCommand->state = IOATAController::kATAComplete;	
	
	completeIO( err );
	return err;
}


/*---------------------------------------------------------------------------
 * Take a large transfer block and split it up into smaller ones based on our
 * ethernet packet size. This fakes "DMA" transfers so we obtain a larger
 * block of memory from our protocol service driver.
 *
 * It supports both 48 and 24LBA addressing
 ---------------------------------------------------------------------------*/

IOReturn AOE_CONTROLLER_NAME::asyncCommand(void)
{
	const int nMaxTransferSize = m_nMaxSectorsPerTransfer*kATADefaultSectorSize;
	int n, nNumSectorsPerTranser;
	IOMemoryDescriptor* descriptor;
	IOReturn err;

	debug("ATAController: command flags = %lx [%s], packet size = %d\n", _currentCommand->getFlags(), (_currentCommand->getFlags()&mATAFlagIOWrite) ? "write" : "read",  _currentCommand->getPacketSize() );

	if ( !_currentCommand )
		return kATAErrUnknownType;

	if ( 0==m_target.nNumberOfInterfaces )
	{
		debugError("Cancelling command as no interfaces are enabled for this device\n");
		return kATADeviceError;
	}

	nNumSectorsPerTranser = _currentCommand->getTransferChunkSize()/kATADefaultSectorSize;

	// Set the chunk size to the min of the transfer size and the size we can squeeze into the ethernet packet
	_currentCommand->setTransferChunkSize(MIN(nMaxTransferSize, _currentCommand->getByteCount()));
	nNumSectorsPerTranser = _currentCommand->getTransferChunkSize()/kATADefaultSectorSize;

	// Set to zero to indicate the start of a group of packets
	m_unReadBaseTag = 0;
	
	// Bring write data into virtual memory
	if( (_currentCommand->getFlags() & mATAFlagIOWrite ) == mATAFlagIOWrite )
	{
		IOByteCount ActualBytesCopied;

		debug("Transfering %d sectors per transfer (Chunk size=%d)\n", nNumSectorsPerTranser, _currentCommand->getTransferChunkSize());

		if ( _currentCommand->getByteCount() > _doubleBuffer.bufferSize )
			debugError("Double buffer is not large enough for write transfer (needs %d and only have %d)\n", _currentCommand->getByteCount(), _doubleBuffer.bufferSize);

		debug("Copying %d bytes to double buffer\n", _currentCommand->getByteCount());
		descriptor = _currentCommand->getBuffer();
		ActualBytesCopied = descriptor->readBytes(0, (void*) _doubleBuffer.logicalBuffer, _currentCommand->getByteCount());
		
		if ( ActualBytesCopied != _currentCommand->getByteCount() )
			debugError("Only %d bytes copied, but expected %d\n", ActualBytesCopied, _currentCommand->getByteCount());

		print_mem((UInt8*)_doubleBuffer.logicalBuffer, _currentCommand->getByteCount());
		
		// Reset status/error states
		m_PreviousWriteStatus = 0;
		m_PreviousWriteError = 0;
	}

	// For reads/writes, we extend the number of reads that occur for a single call of asyncCommand
	if ( is_extended_command() )
	{
		IOExtendedLBA* extLBA = _currentCommand->getExtendedLBA();

		if ( extLBA )
		{
			// Translate DMA commands to regular commands for AoE protocol
			switch ( extLBA->getCommand() )
			{
				case kATAcmdReadDMAExtended:
					extLBA->setCommand(kATAcmdReadExtended);
					break;
				case kATAcmdReadDMA:
					extLBA->setCommand(kATAcmdRead);
					break;
				case kATAcmdWriteDMAExtended:
					extLBA->setCommand(kATAcmdWriteExtended);
					break;
				case kATAcmdWriteDMA:
					extLBA->setCommand(kATAcmdWrite);
					break;
			}
			
			// Handle read/write commands by issuing multiple transactions
			if ((extLBA->getCommand()==kATAcmdReadExtended) ||
				(extLBA->getCommand()==kATAcmdRead) ||
				(extLBA->getCommand()==kATAcmdWrite) ||
				(extLBA->getCommand()==kATAcmdWriteExtended)
				)
			{
				debug("\n");
				debug("$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n");
				debug("$$$$$ Beginning Block transfer $$$$$$$\n");
				debug("$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$\n");
		
				m_nReadWriteRepliesRequired = MAX(1, extLBA->getSectorCount16()/nNumSectorsPerTranser);
				
				debug("Transfer size is: %d - should call issueCommand() %d times\n", _currentCommand->getByteCount(), m_nReadWriteRepliesRequired + ((extLBA->getSectorCount16()-m_nReadWriteRepliesRequired*nNumSectorsPerTranser)>0?1:0));

				if ( extLBA->getSectorCount16() != _currentCommand->getByteCount()/kATADefaultSectorSize )
					debugError("Unexpected sector count\n");

				for(n=0; n<_currentCommand->getByteCount()/(kATADefaultSectorSize*nNumSectorsPerTranser); n++)
				{
					extLBA->setSectorCount16(nNumSectorsPerTranser);	
					err = issueCommand();
					increment_address(extLBA, nNumSectorsPerTranser);
				}
				
				// Transfer any left overs
				if ( _currentCommand->getByteCount()-m_nReadWriteRepliesRequired*(kATADefaultSectorSize*nNumSectorsPerTranser) )
				{
					nNumSectorsPerTranser = (_currentCommand->getByteCount()-m_nReadWriteRepliesRequired*(kATADefaultSectorSize*nNumSectorsPerTranser))/kATADefaultSectorSize;

					debug("NOTE: Transferring %d additional sectors...\n", nNumSectorsPerTranser);
					if ( nNumSectorsPerTranser>0 )
					{
						++m_nReadWriteRepliesRequired;
						extLBA->setSectorCount16(nNumSectorsPerTranser);
						err = issueCommand();
						increment_address(extLBA, nNumSectorsPerTranser);
					}
					else
					{
						debugError("Invalid sector number\n");
					}
				}
			}
			else
				err = issueCommand();
		}
		else
			err = issueCommand();

	}
	else
	{
		ataTaskFile* tfRegs = _currentCommand->getTaskFilePtr();

		if ( tfRegs )
		{
			// Translate DMA commands to regular commands for AoE protocol
			switch ( tfRegs->ataTFCommand )
			{
				case kATAcmdReadDMAExtended:
					tfRegs->ataTFCommand = kATAcmdReadExtended;
					break;
				case kATAcmdReadDMA:
					tfRegs->ataTFCommand = kATAcmdRead;
					break;
				case kATAcmdWriteDMAExtended:
					tfRegs->ataTFCommand = kATAcmdWriteExtended;
					break;
				case kATAcmdWriteDMA:
					tfRegs->ataTFCommand = kATAcmdWrite;
					break;
			}
			
			if ((tfRegs->ataTFCommand==kATAcmdRead) ||
				(tfRegs->ataTFCommand==kATAcmdReadExtended) ||
				(tfRegs->ataTFCommand==kATAcmdWrite) ||
				(tfRegs->ataTFCommand==kATAcmdWriteExtended)
				)
			{
				m_nReadWriteRepliesRequired = MAX(1, tfRegs->ataTFSector/nNumSectorsPerTranser);
				debug("Transfer size is: %d - should call issueCommand() %d times\n", _currentCommand->getByteCount(), m_nReadWriteRepliesRequired + ((tfRegs->ataTFSector-m_nReadWriteRepliesRequired*nNumSectorsPerTranser)>0?1:0));
				
				if ( tfRegs->ataTFSector != _currentCommand->getByteCount()/kATADefaultSectorSize )
					debugError("Unexpected sector count\n");
				
				debug("Overriding count of %d\n", tfRegs->ataTFCount);

				for(n=0; n<_currentCommand->getByteCount()/(kATADefaultSectorSize*nNumSectorsPerTranser); n++)
				{
					tfRegs->ataTFCount = nNumSectorsPerTranser;
					err = issueCommand();
					increment_address(tfRegs, nNumSectorsPerTranser);
				}
				
				// Transfer any left overs
				if ( _currentCommand->getByteCount()-m_nReadWriteRepliesRequired*(kATADefaultSectorSize*nNumSectorsPerTranser) )
				{
					nNumSectorsPerTranser = (_currentCommand->getByteCount()-m_nReadWriteRepliesRequired*(kATADefaultSectorSize*nNumSectorsPerTranser))/kATADefaultSectorSize;
					
					debug("NOTE: Transferring %d additional sectors...\n", nNumSectorsPerTranser);
					if ( nNumSectorsPerTranser>0 )
					{
						++m_nReadWriteRepliesRequired;
						tfRegs->ataTFCount = nNumSectorsPerTranser;
						err = issueCommand();
						increment_address(tfRegs, nNumSectorsPerTranser);
					}
					else
					{
						debugError("Invalid sector number\n");
					}
				}
			}
			else
				err = issueCommand();
		}
		else
			err = issueCommand();
	}
	
	if ( err )
	{
		debugError("asyncCommand - Failed to issueCommand\n");
		return err;
	}
	
	// if DMA operation, return with status pending.
	if( (_currentCommand->getFlags() & mATAFlagUseDMA ) == mATAFlagUseDMA )
	{
		_currentCommand->state = IOATAController::kATAStatus;	
		return err;
	}
	
#if 0
	// if PIO write operation, wait for DRQ and send the first sector
	// or sectors if multiple
	if( (_currentCommand->getFlags()  & (mATAFlagIOWrite | mATAFlagUseDMA | mATAFlagProtocolATAPI) )  == mATAFlagIOWrite )
	{
		// mark the command as data tx state.
		debugError("asyncCommand - Need to write data\n");
		_currentCommand->state = IOATAController::kATADataTx;
		// send first data segment.
		return asyncData();				
	}
#endif
	
	if( (_currentCommand->getFlags() & mATAFlagIORead ) == mATAFlagIORead )
	{
		// read data on next phase.
		_currentCommand->state = IOATAController::kATADataTx;
		debugVerbose("asyncCommand complete for this state. - Need to read data after next AoE packet is received\n");
	}
	else
	{
		// this is a PIO non-data command or a DMA command the next step is to check status.
		//debugVerbose("asyncCommand - PIO non-data command. Need to check status...\n");
		debugVerbose("asyncCommand - writing data. Need to check status after write is complete...\n");
		_currentCommand->state = IOATAController::kATAStatus;	
	}
	
	return err;
}

/*---------------------------------------------------------------------------
 * Create the double buffer for drive read/writes
 ---------------------------------------------------------------------------*/
bool AOE_CONTROLLER_NAME::allocateDoubleBuffer( void )
{
    debug("IOATAController::allocateDoubleBuffer(%d)\n", m_nMaxTransferSize);
	
	_doubleBuffer.logicalBuffer = (IOLogicalAddress) IOMallocContiguous(m_nMaxTransferSize, 4096 /*=alignment*/, &_doubleBuffer.physicalBuffer);
	
	if( _doubleBuffer.logicalBuffer == 0L)
		return false;
	
	_doubleBuffer.bufferSize = m_nMaxTransferSize;
	
	return true;
}


/*---------------------------------------------------------------------------
 * Determine if we're able to send more commands. We only fail this if there
 * are no interfaces available, otherwise, we let the base class handle it
 ---------------------------------------------------------------------------*/
bool AOE_CONTROLLER_NAME::busCanDispatch( void )
{
	bool fCanDispatch;
	
	fCanDispatch = m_pProvider && m_pProvider->interfaces_active(&m_target) && super::busCanDispatch();


	debugVerbose("busCanDispatch() - returning %d\n", fCanDispatch);
	
	return fCanDispatch;
}

#pragma mark -
#pragma mark Non required overrides

IOReturn AOE_CONTROLLER_NAME::startDMA( void )
{
	// Although DMA is used, this virtual isn't called for our class
	debugError("AOE_CONTROLLER_NAME::startDMA() shouldn't be called\n");

	return kATAModeNotSupported;
}

IOReturn AOE_CONTROLLER_NAME::stopDMA( void )
{
	// Some of the base classes stop the DMA without checking for its support.
	// Regardless of how we're running, we just silently return here

	return kATAModeNotSupported;
}

IOReturn AOE_CONTROLLER_NAME::writePacket( void )
{
	debugError("writePacket() - Not required for AoE - shouldn't be called\n");
	
	return 0;
}


UInt16 AOE_CONTROLLER_NAME::readExtRegister( IOATARegPtr8 inRegister )
{
	debugError("readExtRegister() - Not required for our override of registerAccess()\n");
	
	return 0;
}

void AOE_CONTROLLER_NAME::writeExtRegister( IOATARegPtr8 inRegister, UInt16 inValue )
{
	debugError("writeExtRegister() - Not required for our override of registerAccess()\n");
}

bool AOE_CONTROLLER_NAME::waitForU8Status (UInt8 mask, UInt8 value)
{
	debugError("AOE_CONTROLLER_NAME::waitForU8Status() - always returns TRUE for AoE\n");
	
	return TRUE;
}


IOReturn AOE_CONTROLLER_NAME::softResetBus( bool doATAPI )
{
	debugError("softResetBus() - Not required for our override of registerAccess()\n");
	
	return kIOReturnUnsupported;
}

bool AOE_CONTROLLER_NAME::ATAPISlaveExists( void )
{
	debugError("ATAPISlaveExists() - Not required for AoE\n");

	return FALSE;
}

#pragma mark -
#pragma mark target handling

void AOE_CONTROLLER_NAME::SetLBAExtendedSupport(bool f)
{
	debug("AOE_CONTROLLER_NAME::SetLBAExtendedSupport = %d\n", f);

	m_fExtendedLBA = f;
}

/*---------------------------------------------------------------------------
 * Determine if we're using an extended comand 
 *
 * If life was easy, we could check ( _currentCommand->getFlags() & mATAFlag48BitLBA ) 
 * to determine if we have a 48bit LBA command. Since there is a bug at the moment,
 * we have to figure it out. 
 * Based on this comment: http://lists.apple.com/archives/ata-scsi-dev/2007/Oct/msg00010.html
 *
 ---------------------------------------------------------------------------*/

bool AOE_CONTROLLER_NAME::is_extended_command(void)
{
	bool fUseExtendedCommand;
	ataTaskFile* tfRegs;


	// We'll assume it is only a problem for the extended read/writes, otherwise, it's fine
	fUseExtendedCommand =  _currentCommand->getFlags() & mATAFlag48BitLBA;
	
	if ( !fUseExtendedCommand )
	{
		tfRegs = _currentCommand->getTaskFilePtr();
		
		if ( tfRegs && (	(tfRegs->ataTFCommand==kATAcmdWriteExtended) ||
							(tfRegs->ataTFCommand==kATAcmdReadExtended)	))
			fUseExtendedCommand = TRUE;
	}

	return fUseExtendedCommand;
}


/*---------------------------------------------------------------------------
 * Add offset to LBA address values (48-bit)
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::increment_address(IOExtendedLBA* extLBA, int nInc)
{
	if ( extLBA )
	{
		UInt64 lba[6];
		
		#ifndef DEBUG
		UInt16 lbaOrig = extLBA->getLBALow16();
		UInt16 hbaOrig = extLBA->getLBAHigh16();
		UInt16 mbaOrig = extLBA->getLBAMid16();
		#endif

		// Convert address into actual address
		lba[3] = (extLBA->getLBALow16() & 0xFF00) >> 8;
		lba[0] = (extLBA->getLBALow16() & 0x00FF);
		lba[4] = (extLBA->getLBAMid16() & 0xFF00) >> 8;
		lba[1] = (extLBA->getLBAMid16() & 0x00FF);
		lba[5] = (extLBA->getLBAHigh16() & 0xFF00) >> 8;
		lba[2] = (extLBA->getLBAHigh16() & 0x00FF);

		UInt64 Add =(lba[5]<<40) |
					(lba[4]<<32) |
					(lba[3]<<24) |
					(lba[2]<<16) |
					(lba[1]<<8) |
					(lba[0]<<0) ;
		
		// Increment address
		Add += nInc;

		// convert back to required format
		lba[0] = (Add>> 0) & 0xFF;
		lba[1] = (Add>> 8) & 0xFF;
		lba[2] = (Add>>16) & 0xFF;
		lba[3] = (Add>>24) & 0xFF;
		lba[4] = (Add>>32) & 0xFF;
		lba[5] = (Add>>40) & 0xFF;
		
		extLBA->setLBALow16 ( (lba[3]<<8) | lba[0] );
		extLBA->setLBAMid16 ( (lba[4]<<8) | lba[1] );
		extLBA->setLBAHigh16( (lba[5]<<8) | lba[2] );

		debugVerbose("48LBA=[%x %x %x]+=%d = [%x %x %x]\n", (unsigned int)hbaOrig, (unsigned int)mbaOrig, (unsigned int)lbaOrig, nInc, (unsigned int)extLBA->getLBAHigh16(), (unsigned int)extLBA->getLBAMid16(), (unsigned int)extLBA->getLBALow16());
	}
}

/*---------------------------------------------------------------------------
 * Add offset to LBA address values (24-bit)
 ---------------------------------------------------------------------------*/

void AOE_CONTROLLER_NAME::increment_address(ataTaskFile* tfRegs, int nInc)
{
	if ( tfRegs )
	{
		UInt8 lba = tfRegs->ataTFSector;
		UInt8 mba = tfRegs->ataTFCylLo;
		UInt8 hba = tfRegs->ataTFCylHigh;
		UInt32 Add = (hba<<16) | (mba<<8) | (lba);
		
		Add += nInc;

		tfRegs->ataTFSector  = (Add>> 0)&0xFF;
		tfRegs->ataTFCylLo   = (Add>> 8)&0xFF;
		tfRegs->ataTFCylHigh = (Add>>16)&0xFF;
		
		debugVerbose("24LBA=[%x %x %x]+%d = [%x %x %x]\n", (unsigned int)hba, (unsigned int)mba, (unsigned int)lba, nInc, (unsigned int)tfRegs->ataTFCylHigh, (unsigned int)tfRegs->ataTFCylLo, (unsigned int)tfRegs->ataTFSector);
	}
}


/*---------------------------------------------------------------------------
 * Set the number of sectors for this controller
 ---------------------------------------------------------------------------*/

void AOE_CONTROLLER_NAME::set_number_sectors(UInt64 Sectors)
{
	OSNumber* num;
	
	num = OSNumber::withNumber(Sectors, 64);
//	if ( _provider )
//		_provider->setProperty(CAPACITY_PROPERTY, num);
	setProperty(CAPACITY_PROPERTY, num);
	num->release();
	
	m_target.NumSectors = Sectors;
}

/*---------------------------------------------------------------------------
 * Update the targets info with info from a recently connected interface
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::update_target_info(ifnet_t ifnet_receive, u_char* pTargetsMACAddress, bool fOnline)
{
	int n, nInterfaceNumber;
	bool fInterfaceExists;
	
	// Check if the interface is one we already know about
	fInterfaceExists = FALSE;
	for(n=0; n< m_target.nNumberOfInterfaces; n++)
		if ( ifnet_receive== m_target.aInterfaces[n] )
		{
			fInterfaceExists = TRUE;
			nInterfaceNumber = n;
			break;
		}

	if ( fInterfaceExists )
	{
		//debugVerbose("Device interface located at position: %d\n", nInterfaceNumber);
		if ( fOnline )
		{
			// Just update the time since last communication (this is used to timeout an offline device)
			clock_get_uptime(&m_time_since_last_comm);
		}
		else
		{
			// If the interface does exist and we're offline, remove it from the list //
			remove_interface(nInterfaceNumber);
		}
	}
	else
	{
		// If the interface doesn't exist, add it to the list
		if ( fOnline )
		{
			m_target.aInterfaces[m_target.nNumberOfInterfaces] = ifnet_receive;
			m_target.aInterfaceNum[m_target.nNumberOfInterfaces] = ifnet_unit(ifnet_receive);
			clock_get_uptime(&m_time_since_last_comm);
			ifnet_lladdr_copy_bytes(ifnet_receive, m_target.aaSrcMACAddress[m_target.nNumberOfInterfaces], ETHER_ADDR_LEN);
			bcopy(pTargetsMACAddress, m_target.aaDestMACAddress[m_target.nNumberOfInterfaces], ETHER_ADDR_LEN);
			++m_target.nNumberOfInterfaces;

			update_interface_property();
			debugVerbose("Add interface to device's list (%d interfaces currently connected)\n", m_target.nNumberOfInterfaces);
		}
	}
	
	return 0;
}

/*---------------------------------------------------------------------------
 * Remove any interfaces that are associated with this controller/device
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::remove_all_interfaces(void)
{
	debug("AOE_CONTROLLER_NAME::remove_all_interfaces\n");
	
	while ( m_target.nNumberOfInterfaces )
		remove_interface(0);
	
	update_interface_property();
}

/*---------------------------------------------------------------------------
 * Interface info is stored in the registry so user level programs can access
 * the info
 ---------------------------------------------------------------------------*/

void AOE_CONTROLLER_NAME::update_interface_property(void)
{
	int n;

	removeProperty(ATTACHED_INTERFACES_PROPERTY);

	if ( m_target.nNumberOfInterfaces )
	{
		OSArray* pInterfaces = OSArray::withCapacity(m_target.nNumberOfInterfaces);
		if ( pInterfaces )
		{
			for(n=0; n<m_target.nNumberOfInterfaces; n++)
			{
				debug("Adding interface to array\n");
				OSNumber* pNumber = OSNumber::withNumber(m_target.aInterfaceNum[n], 32);
				pInterfaces->setObject(pNumber);
				pNumber->release();
			}
			
			setProperty(ATTACHED_INTERFACES_PROPERTY, (OSObject* )pInterfaces);
			pInterfaces->release();
		}
	}
}


/*---------------------------------------------------------------------------
 * Remove an interface that is no longer in use
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::remove_interface(int nInterfaceNumber)
{
	// Move the interface at the end of the list to our current position, clear position and reduce the count
	m_target.aInterfaces[nInterfaceNumber] = m_target.aInterfaces[m_target.nNumberOfInterfaces-1];
	m_target.aInterfaceNum[nInterfaceNumber] = m_target.aInterfaceNum[m_target.nNumberOfInterfaces-1];
	bcopy(m_target.aaSrcMACAddress[nInterfaceNumber], m_target.aaSrcMACAddress[m_target.nNumberOfInterfaces-1], ETHER_ADDR_LEN);
	bcopy(m_target.aaDestMACAddress[nInterfaceNumber], m_target.aaDestMACAddress[m_target.nNumberOfInterfaces-1], ETHER_ADDR_LEN);
	
	m_target.aInterfaces[m_target.nNumberOfInterfaces-1] = 0;
	m_target.aInterfaceNum[m_target.nNumberOfInterfaces-1] = 0;
	memset(m_target.aaSrcMACAddress[m_target.nNumberOfInterfaces-1], 0, ETHER_ADDR_LEN);
	memset(m_target.aaDestMACAddress[m_target.nNumberOfInterfaces-1], 0, ETHER_ADDR_LEN);
	--m_target.nNumberOfInterfaces;
	update_interface_property();
	
	debugVerbose("remove interface from device's list (%d interfaces currently connected)\n", m_target.nNumberOfInterfaces);
}

int AOE_CONTROLLER_NAME::is_device(int nShelf, int nSlot)
{
	if ( (nShelf==m_target.nShelf) && (nSlot==m_target.nSlot) )
		return 0;
	
	return -1;
}

int AOE_CONTROLLER_NAME::device_attached(void)
{
	return (m_pAoEDevice!=NULL) ? 0 : -1;
}


/*---------------------------------------------------------------------------
 * Attach a device to a recently created controller
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::attach_device(void)
{
	debug("AOE_CONTROLLER_NAME::attach_device\n");

	m_pAoEDevice = AOE_DEVICE_NAME::create_aoe_device((IOATAController*)this, m_target.nShelf, m_target.nSlot);
  	
	if ( m_pAoEDevice )
	{
		// set the media notify property if available
		//if( _pciACPIDevice )
		{
			m_pAoEDevice->setProperty( kATANotifyOnChangeKey, 1, 32);						
		}

		if ( !m_pAoEDevice->attach( this ) )
		{
			CLEAN_RELEASE(m_pAoEDevice);
		}
		
		// NOTE: In the base class's attach_device code, start wasn't called in IODeviceNub handling. It now is...
		if ( !m_pAoEDevice->start(this) )
		{
			debugError("Trouble starting pController\n");
			m_pAoEDevice->detach(this);
			CLEAN_RELEASE(m_pAoEDevice);
			return;
		}

		// The AoEDevice will register itself after it has received the IDENTIFY command.
		// Doing so any earlier will cause a panic as the protocol service driver will not have all 
		// the info it needs to attach/
	}
}


int AOE_CONTROLLER_NAME::target_number(void)
{
	return m_target.nTargetNumber;
}

TargetInfo* AOE_CONTROLLER_NAME::get_target_info(void)
{
	return &m_target;
}

int AOE_CONTROLLER_NAME::cstring_is_ours(const char* pszString)
{
	return strncmp(pszString, m_aConfigString, MAX_CONFIG_STRING_LENGTH)==0 ? 0 : -1;
}

/*---------------------------------------------------------------------------
 * Iterate over our list of connected interfaces to check connections
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::connected_to_interface(ifnet_t enetifnet)
{
	int n;

	for(n=0; n < m_target.nNumberOfInterfaces; n++)
		if ( enetifnet== m_target.aInterfaces[n] )
			return 0;

	return -1;
}


/*---------------------------------------------------------------------------
 * Adjust the transfer size
 ---------------------------------------------------------------------------*/
int AOE_CONTROLLER_NAME::send_identify(void)
{
	aoe_atahdr_full* pAoEFullATAHeader;
	aoe_header*	pAoEHeader;
	aoe_atahdr* pATAhdr;
	UInt32	Tag;
	mbuf_t m;

	debugVerbose("AOE_CONTROLLER_NAME::send_identify\n");
	
	// Initialise our mbuf
	Tag = m_pProvider->next_tag();
	if ( 0!=create_mbuf_for_transfer(&m, Tag, TRUE) )
		return -1;
	pAoEFullATAHeader = MTOD(m, aoe_atahdr_full*);
	
	// Add shortcuts
	pAoEHeader = &(pAoEFullATAHeader->aoe);
	pATAhdr = &(pAoEFullATAHeader->ata);
	
	AOE_ATAHEADER_CLEAR(pATAhdr);
	
	// Write data to mbuf
	pATAhdr->aa_scnt_cmdstat = AOE_ATAHEADER_SETSCNTCMD(0, kATAcmdDriveIdentify);
	
	// Output data to interface
	m_nOutstandingIdentTag = 0;
	if ( 0==m_pProvider->send_ata_packet(this, m, Tag, get_target_info()) )
		m_nOutstandingIdentTag = Tag;

	return (0==m_nOutstandingIdentTag) ? -1 : 0;
}



/*---------------------------------------------------------------------------
 * Adjust the transfer size
 ---------------------------------------------------------------------------*/
void AOE_CONTROLLER_NAME::set_mtu_size(int nMTU)
{
	m_MTU = nMTU;
	m_nMaxSectorsPerTransfer = COUNT_SECTORS_FROM_MTU(m_MTU);

	debug("[%d.%d] Adjusting transfer sizes based on MTU of: %d bytes (%d sectors per transfer)\n", m_target.nShelf, m_target.nSlot, nMTU, m_nMaxSectorsPerTransfer);
}


#pragma mark -
#pragma mark retain/release debugging

/*---------------------------------------------------------------------------
 * Override retain/release functions to help check retain counts
 ---------------------------------------------------------------------------*/

#if 0
void AOE_CONTROLLER_NAME::retain() const
{
	debug("AOE_CONTROLLER_NAME::retain() - COUNT=%d\n", getRetainCount());
	super::retain();
	
/*
 // next time, try this code for checking the stack during retain/release:
 #include <execinfo.h>
 #include <stdio.h>
 ...
 void* callstack[128];
 int i, frames = backtrace(callstack, 128);
 char** strs = backtrace_symbols(callstack, frames);
 for (i = 0; i < frames; ++i) {
 printf("%s\n", strs[i]);
 }
 free(strs);
 */
}
void AOE_CONTROLLER_NAME::release(int when) const
{
	super::release(when);
	debug("AOE_CONTROLLER_NAME::release(%d) - COUNT=%d\n", when, getRetainCount());
}
void AOE_CONTROLLER_NAME::taggedRetain(const void *tag) const
{
	debug("AOE_CONTROLLER_NAME::taggedRetain() - COUNT=%d\n", getRetainCount());
	super::taggedRetain(tag);
}

void AOE_CONTROLLER_NAME::taggedRelease(const void *tag, const int when) const
{
	super::taggedRelease(tag, when);
	debug("AOE_CONTROLLER_NAME::taggedRelease(when=%d) - COUNT=%d\n", when, getRetainCount());
}
#endif
//////















/////////// THE FOLLOWING OVERIDES ARE JUST FOR DEBUGGING PURPOSES

#pragma mark -
#pragma mark debugging overrides

#ifdef DEBUGBUILD

IOReturn AOE_CONTROLLER_NAME::executeCommand(IOATADevice* nub, IOATABusCommand* command)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::executeCommand\n");

	return super::executeCommand(nub, command);
}

IOReturn AOE_CONTROLLER_NAME::handleCommand(	void*	param0,     /* the command */
							   void*	param1,		/* not used = 0 */
							   void*	param2,		/* not used = 0 */
							   void*	param3 )	/* not used = 0 */
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::handleCommand\n");

	return super::handleCommand(param0, param1, param2, param3);
}

IOReturn AOE_CONTROLLER_NAME::enqueueCommand( IOATABusCommand* command)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::enqueueCommand\n");

	return super::enqueueCommand(command);
}

IOATABusCommand* AOE_CONTROLLER_NAME::dequeueFirstCommand( void )
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::dequeueFirstCommand\n");

	return super::dequeueFirstCommand();	
}

IOReturn AOE_CONTROLLER_NAME::dispatchNext( void )
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::dispatchNext start\n");

	return super::dispatchNext();
}

void AOE_CONTROLLER_NAME::executeEventCallouts( ataEventCode event, ataUnitID unit )
{
	debugVerbose("AOE_CONTROLLER_NAME:: - super::executeEventCallouts(event=%d, unit=%d)\n", event, unit);

	super::executeEventCallouts(event, unit);
}	

void AOE_CONTROLLER_NAME::completeIO( IOReturn commandResult )
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::completeIO start = %ld\n",(long int)commandResult);

	super::completeIO(commandResult);
}

IOReturn AOE_CONTROLLER_NAME::startTimer( UInt32 inMS)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::startTimer(%d)\n", inMS);

	return super::startTimer(inMS);	
}

void AOE_CONTROLLER_NAME::stopTimer(void)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::stopTimer\n");

	super::stopTimer();
}

IOReturn AOE_CONTROLLER_NAME::handleExecIO( void )
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::handleExecIO\n");

	return super::handleExecIO();	
}

IOReturn AOE_CONTROLLER_NAME::asyncIO(void)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::asyncIO\n");

	return super::asyncIO();
}

IOReturn AOE_CONTROLLER_NAME::txDataIn (IOLogicalAddress buf, IOByteCount length)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::txDataIn\n");

	return super::txDataIn(buf, length);
}

IOReturn AOE_CONTROLLER_NAME::txDataOut(IOLogicalAddress buf, IOByteCount length)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::txDataOut\n");

	return super::txDataOut(buf, length);
}

IOByteCount	AOE_CONTROLLER_NAME::readATAPIByteCount (void)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::readATAPIByteCount\n");

	return super::readATAPIByteCount();
}

AOE_CONTROLLER_NAME::transState AOE_CONTROLLER_NAME::determineATAPIState(void)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::determineATAPIState\n");

	return super::determineATAPIState();
}

void AOE_CONTROLLER_NAME::handleOverrun( IOByteCount length)
{
	debugVerbose("AOE_CONTROLLER_NAME::  - super::handleOverrun\n");

	super::handleOverrun(length);
}

void AOE_CONTROLLER_NAME::handleTimeout( void )
{
	debugError("Timeout occurred for the previous function\n");
	
	super::handleTimeout();
}
#endif
