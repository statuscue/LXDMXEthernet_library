/* LXArtNet.cpp
   Copyright 2015-2015 by Claude Heintz Design
   see LXDMXEthernet.h for license
   
   LXArtNet partially implements the Art-Net Ethernet Communication Standard.
   http://www.artisticlicence.com
   
   LXArtNet supports capturing a single universe of DMX data
   from Art-Net packets read from UDP.
   LXArtNet will automatically respond to ArtPoll packets
   by sending a unicast reply directly to the poll.
   
   LXArtNet does not support merge and will only accept ArtDMX output packets 
   from the first IP address that it receives a packet from.
   This can be reset by sending an ArtAddress cancel merge command.
      
	Art-Net(TM) Designed by and Copyright Artistic Licence (UK) Ltd
*/

#include "LXArtNet.h"

//shared static buffer for sending poll replies
uint8_t LXArtNet::_reply_buffer[ARTNET_REPLY_SIZE];

LXArtNet::LXArtNet ( IPAddress address )
{
	initialize(0);
	
    _my_address = address;
    _broadcast_address = INADDR_NONE;
}

LXArtNet::LXArtNet ( IPAddress address, IPAddress subnet_mask )
{
	initialize(0);
	
    _my_address = address;
    uint32_t a = (uint32_t) address;
    uint32_t s = (uint32_t) subnet_mask;
    _broadcast_address = IPAddress(a | ~s);
}

LXArtNet::LXArtNet ( IPAddress address, IPAddress subnet_mask, uint8_t* buffer )
{
	initialize(buffer);
    _my_address = address;
    uint32_t a = (uint32_t) address;
    uint32_t s = (uint32_t) subnet_mask;
    _broadcast_address = IPAddress(a | ~s);
}

LXArtNet::~LXArtNet ( void )
{
   if ( _owns_buffer ) {		// if we created this buffer, then free the memory
		free(_packet_buffer);
	}
	if ( _using_htp ) {
		free(_dmx_buffer_a);
		free(_dmx_buffer_b);
		free(_dmx_buffer_c);
	}
}

void  LXArtNet::initialize  ( uint8_t* b ) {
	if ( b == 0 ) {
		// create buffer
		_packet_buffer = (uint8_t*) malloc(ARTNET_BUFFER_MAX);
		_owns_buffer = 1;
	} else {
		// external buffer.  Size MUST be >=ARTNET_BUFFER_MAX
		_packet_buffer = b;
		_owns_buffer = 0;
	}
	
    memset(_packet_buffer, 0, ARTNET_BUFFER_MAX);
    
    _using_htp    = 0;
    _dmx_buffer_a = 0;
    _dmx_buffer_b = 0;
    _dmx_buffer_c = 0;
    
    _dmx_slots   = 0;
    _dmx_slots_a = 0;
    _dmx_slots_b = 0;
    _universe    = 0;
    _net         = 0;
    _sequence    = 1;
    
     _dmx_sender = INADDR_NONE;
     _dmx_sender_b = INADDR_NONE;
    
    initializePollReply();
}


uint8_t  LXArtNet::universe ( void ) {
	return _universe;
}

void LXArtNet::setUniverse ( uint8_t u ) {
	_universe = u;
}

void LXArtNet::setSubnetUniverse ( uint8_t s, uint8_t u ) {
   _universe = ((s & 0x0f) << 4) | ( u & 0x0f);
}

void LXArtNet::setUniverseAddress ( uint8_t u ) {
	if ( u != 0x7f ) {
	   if ( u & 0x80 ) {
	     _universe = (_universe & 0xf0) | (u & 0x07);
	   }
	}
}

void LXArtNet::setSubnetAddress ( uint8_t u ) {
	if ( u != 0x7f ) {
	   if ( u & 0x80 ) {
	     _universe = (_universe & 0x0f) | ((u & 0x07) << 4);
	   }
	}
}

void LXArtNet::setNetAddress ( uint8_t s ) {
	if ( s & 0x80 ) {
	  _net = (s & 0x7F);
	}
}

void LXArtNet::enableHTP() {
#if defined(__AVR_ATmega168__) || defined(__AVR_ATmega328P__) || defined(__AVR_ATmega328__)
	// not enough memory on these to allocate these buffers
#else
	if ( ! _using_htp ) {
		_dmx_buffer_a = (uint8_t*) malloc(DMX_UNIVERSE_SIZE);
		_dmx_buffer_b = (uint8_t*) malloc(DMX_UNIVERSE_SIZE);
		_dmx_buffer_c = (uint8_t*) malloc(DMX_UNIVERSE_SIZE);
		for(int i=0; i<DMX_UNIVERSE_SIZE; i++) {
		   _dmx_buffer_a[i] = 0;
		   _dmx_buffer_b[i] = 0;
		   _dmx_buffer_c[i] = 0;
		}
		_using_htp = 1;
	}
#endif
}

int  LXArtNet::numberOfSlots ( void ) {
	return _dmx_slots;
}

void LXArtNet::setNumberOfSlots ( int n ) {
	_dmx_slots = n;
}

uint8_t LXArtNet::getSlot ( int slot ) {
	return _packet_buffer[ARTNET_ADDRESS_OFFSET+slot];
}

uint8_t LXArtNet::getHTPSlot ( int slot ) {
	return _dmx_buffer_c[slot-1];
}

void LXArtNet::setSlot ( int slot, uint8_t value ) {
	_packet_buffer[ARTNET_ADDRESS_OFFSET+slot] = value;
}

uint8_t* LXArtNet::dmxData( void ) {
	return &_packet_buffer[ARTNET_ADDRESS_OFFSET+1];
}

uint8_t* LXArtNet::replyData( void ) {
	return _reply_buffer;
}

uint8_t LXArtNet::readDMXPacket ( UDP* eUDP ) {
   uint16_t opcode = readArtNetPacket(eUDP);
   if ( opcode == ARTNET_ART_DMX ) {
   	return RESULT_DMX_RECEIVED;
   }
   return RESULT_NONE;
}

uint8_t LXArtNet::readDMXPacketContents ( UDP* eUDP, int packetSize ) {
	if ( packetSize > 0 ) {
		uint16_t opcode = readArtNetPacketContents(eUDP, packetSize);
		if ( opcode == ARTNET_ART_DMX ) {
			return RESULT_DMX_RECEIVED;
		}
		if ( opcode == ARTNET_ART_POLL ) {
			return RESULT_PACKET_COMPLETE;
		}
   }
   return RESULT_NONE;
}

/*
  attempts to read a packet from the supplied UDP object
  returns opcode
  sends ArtPollReply with IPAddress if packet is ArtPoll
  replies directly to sender unless reply_ip != INADDR_NONE allowing specification of broadcast
  only returns ARTNET_ART_DMX if packet contained dmx data for this universe
  Packet size checks that packet is >= expected size to allow zero termination or padding
*/
uint16_t LXArtNet::readArtNetPacket ( UDP* eUDP ) {
	uint16_t opcode = ARTNET_NOP;
	int packetSize = eUDP->parsePacket();
	if ( packetSize > 0 ) {
		packetSize = eUDP->read(_packet_buffer, ARTNET_BUFFER_MAX);
		opcode = readArtNetPacketContents(eUDP, packetSize);
	}
	return opcode;
}

uint16_t LXArtNet::readArtNetPacketContents ( UDP* eUDP, int packetSize ) {
   if ( ! _using_htp ) {
		_dmx_slots = 0;
		/* Buffer now may not contain dmx data for desired universe.
		After reading the packet into the buffer, check to make sure
		that it is an Art-Net packet and retrieve the opcode that
		tells what kind of message it is.                            */
	}
	
	uint16_t opcode = parse_header();
	switch ( opcode ) {
		case ARTNET_ART_DMX:
		   opcode = ARTNET_NOP;
			// ignore protocol version [10] hi byte [11] lo byte sequence[12], physical[13]
			if (( _packet_buffer[14] == _universe ) && ( _packet_buffer[15] == _net )) {
				packetSize -= 18;
				uint16_t slots = _packet_buffer[17];
				slots += _packet_buffer[16] << 8;
				if ( packetSize >= slots ) {					// double check we got all expected
					opcode = readArtDMX(eUDP, slots, packetSize);      // returns ARTNET_ART_DMX
				}
			}   // matched universe/net
			break;
		case ARTNET_ART_ADDRESS:
			if (( packetSize >= 107 ) && ( _packet_buffer[11] >= 14 )) {  //protocol version [10] hi byte [11] lo byte
				opcode = parse_art_address();
				send_art_poll_reply( eUDP );
			}
			break;
		case ARTNET_ART_POLL:
			if (( packetSize >= 14 ) && ( _packet_buffer[11] >= 14 )) {
				send_art_poll_reply( eUDP );
			}
			break;
	}
   return opcode;
}

uint16_t LXArtNet::readArtDMX ( UDP* eUDP, uint16_t slots, int packetSize ) {
	uint16_t opcode = ARTNET_NOP;
	if ( _using_htp ) {
	   if ( (uint32_t)_dmx_sender == 0 ) {		//if first sender, remember address
			_dmx_sender = eUDP->remoteIP();
			for(int j=0; j<DMX_UNIVERSE_SIZE; j++) {
				_dmx_buffer_b[j] = 0;	//insure clear buffer 'b' so cancel merge works properly
			}
	   }
		if ( _dmx_sender == eUDP->remoteIP() ) {
			_dmx_slots_a  = slots;
			if ( _dmx_slots_a > _dmx_slots_b ) {
				_dmx_slots = _dmx_slots_a;
			} else {
				_dmx_slots = _dmx_slots_b;
			}
			int di;
			int dc = _dmx_slots;
			int dt = ARTNET_ADDRESS_OFFSET + 1;
			  for (di=0; di<dc; di++) {
				 _dmx_buffer_a[di] = _packet_buffer[dt+di];
				if ( _dmx_buffer_a[di] > _dmx_buffer_b[di] ) {
					_dmx_buffer_c[di] = _dmx_buffer_a[di];
				} else {
					_dmx_buffer_c[di] = _dmx_buffer_b[di];
				}
			}
			opcode = ARTNET_ART_DMX;
		} else { 												// did not match sender a
			if ( (uint32_t)_dmx_sender_b == 0 ) {		// if 2nd sender, remember address
				_dmx_sender_b = eUDP->remoteIP();
			}
			if ( _dmx_sender_b == eUDP->remoteIP() ) {
			  _dmx_slots_b  = slots;
				if ( _dmx_slots_a > _dmx_slots_b ) {
					_dmx_slots = _dmx_slots_a;
				} else {
					_dmx_slots = _dmx_slots_b;
				}
			  int di;
			  int dc = _dmx_slots;
			  int dt = ARTNET_ADDRESS_OFFSET + 1;
			  for (di=0; di<dc; di++) {
				 _dmx_buffer_b[di] = _packet_buffer[dt+di];
				 if ( _dmx_buffer_a[di] > _dmx_buffer_b[di] ) {
					_dmx_buffer_c[di] = _dmx_buffer_a[di];
				 } else {
					_dmx_buffer_c[di] = _dmx_buffer_b[di];
				 }
			  }
			  opcode = ARTNET_ART_DMX;
			}  // matched sender b
		}     // did not match sender a
	} else {												// NOT _using_htp only allow one sender
		if ( (uint32_t)_dmx_sender == 0 ) {		// if first sender, remember address
			_dmx_sender = eUDP->remoteIP();
		}
		if ( _dmx_sender == eUDP->remoteIP() ) {
			_dmx_slots = slots;
			//zero remainder of buffer
		   for (int n=packetSize+18; n<ARTNET_BUFFER_MAX; n++) {
			  _packet_buffer[n] = 0;
		    }
		  opcode = ARTNET_ART_DMX;
		}	// matched sender
	
	}
	return opcode;
}

void LXArtNet::sendDMX ( UDP* eUDP, IPAddress to_ip ) {
   strcpy((char*)_packet_buffer, "Art-Net");
   _packet_buffer[8] = 0;        //op code lo-hi
   _packet_buffer[9] = 0x50;
   _packet_buffer[10] = 0;
   _packet_buffer[11] = 14;
   if ( _sequence == 0 ) {
     _sequence = 1;
   } else {
     _sequence++;
   }
   _packet_buffer[12] = _sequence;
   _packet_buffer[13] = 0;
   _packet_buffer[14] = _universe;
   _packet_buffer[15] = _net;
   _packet_buffer[16] = _dmx_slots >> 8;
   _packet_buffer[17] = _dmx_slots & 0xFF;
   //assume dmx data has been set
  
   eUDP->beginPacket(to_ip, ARTNET_PORT);
   eUDP->write(_packet_buffer, 18+_dmx_slots);
   eUDP->endPacket();
}

/*
  sends ArtDMX packet to UDP object's remoteIP if to_ip is not specified
  ( remoteIP is set when parsePacket() is called )
  includes my_ip as address of this node
*/
void LXArtNet::send_art_poll_reply( UDP* eUDP ) {
	_reply_buffer[190] = _universe;
  
  IPAddress a = _broadcast_address;
  if ( a == INADDR_NONE ) {
    a = eUDP->remoteIP();   // reply directly if no broadcast address is supplied
  }
  eUDP->beginPacket(a, ARTNET_PORT);
  eUDP->write(_reply_buffer, ARTNET_REPLY_SIZE);
  eUDP->endPacket();
}

uint16_t LXArtNet::parse_header( void ) {
  if ( strcmp((const char*)_packet_buffer, "Art-Net") == 0 ) {
    return _packet_buffer[9] * 256 + _packet_buffer[8];  //opcode lo byte first
  }
  return ARTNET_NOP;
}

/*
  reads an ARTNET_ART_ADDRESS packet
  can set output universe
  can cancel merge which resets address of dmx sender
     (after first ArtDmx packet, only packets from the same sender are accepted
     until a cancel merge command is received)
*/
uint16_t LXArtNet::parse_art_address( void ) {
   setNetAddress(_packet_buffer[12]);
	//[14] to [31] short name <= 18 bytes
	//[32] to [95] long name  <= 64 bytes
	//[96][97][98][99]                  input universe   ch 1 to 4
	//[100][101][102][103]               output universe   ch 1 to 4
	setUniverseAddress(_packet_buffer[100]);
	//[102][103][104][105]                      subnet   ch 1 to 4
	setSubnetAddress(_packet_buffer[104]);
	//[105]                                   reserved
	uint8_t command = _packet_buffer[106]; // command
	switch ( command ) {
	   case 0x01:	//cancel merge: resets ip address used to identify dmx sender
	   	_dmx_sender =   (uint32_t)0;
	   	_dmx_sender_b = (uint32_t)0;
	   	break;
	   case 0x90:	//clear buffer
	   	_dmx_sender = (uint32_t)0;
	   	for(int j=18; j<ARTNET_BUFFER_MAX; j++) {
	   	   _packet_buffer[j] = 0;
	   	}
	   	_dmx_slots = 512;
	   	return ARTNET_ART_DMX;	// return ARTNET_ART_DMX so function calling readPacket
	   	   						   // knows there has been a change in levels
	   	break;
	}
	return ARTNET_ART_ADDRESS;
}

void  LXArtNet::initializePollReply  ( void ) {
	int i;
  for ( i = 0; i < ARTNET_REPLY_SIZE; i++ ) {
    _reply_buffer[i] = 0;
  }
  strcpy((char*)_reply_buffer, "Art-Net");
  _reply_buffer[7] = 0;
  _reply_buffer[8] = 0;        // op code lo-hi
  _reply_buffer[9] = 0x21;
  _reply_buffer[10] = ((uint32_t)_my_address) & 0xff;      //ip address
  _reply_buffer[11] = ((uint32_t)_my_address) >> 8;
  _reply_buffer[12] = ((uint32_t)_my_address) >> 16;
  _reply_buffer[13] = ((uint32_t)_my_address) >>24;
  _reply_buffer[14] = 0x36;    // port lo first always 0x1936
  _reply_buffer[15] = 0x19;
  _reply_buffer[16] = 0;       // firmware hi-lo
  _reply_buffer[17] = 0;
  _reply_buffer[18] = 0;       // subnet hi-lo
  _reply_buffer[19] = 0;
  _reply_buffer[20] = 0;       // oem hi-lo 0x1250
  _reply_buffer[21] = 0;
  _reply_buffer[22] = 0;       // ubea
  _reply_buffer[23] = 0;       // status
  _reply_buffer[24] = 0x6C;    //     ESTA Mfg Code
  _reply_buffer[25] = 0x78;
  strcpy((char*)&_reply_buffer[26], "ArduinoDMX");
  strcpy((char*)&_reply_buffer[44], "ArduinoDMX");
  _reply_buffer[173] = 1;    // number of ports
  _reply_buffer[174] = 0x80;  // can output from network
  _reply_buffer[182] = 0x80;  //  good output... change if error
  _reply_buffer[190] = _universe;
}
