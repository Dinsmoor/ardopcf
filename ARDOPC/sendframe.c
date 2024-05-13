///////////////////////////////////////////////////////////////
// sendframe() and its use by the _SEND Host command is
// intended for development and debugging.
// It is NOT intended for normal use by Host applications.
// It may be removed or modfied without notice in future
// versions of ardopcf.
///////////////////////////////////////////////////////////////

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FALSE 0 // defined in ARDOPC.h
#define TRUE 1 // defined in ARDOPC.h
#define VOID void // defined in ARDOPC.h
typedef unsigned char UCHAR; // defined in ARDOPC.h
typedef int BOOL; // defined in ARDOPC.h
#define LOGWARNING 4 // defined in ARDOPC.h
#define LOGDEBUG 7 // defined in ARDOPC.h


#define BREAK 0x23 // defined in ARDOPC.h
#define IDLEFRAME 0x24 // defined in ARDOPC.h
#define DISCFRAME 0x29 // defined in ARDOPC.h
#define END 0x2C // defined in ARDOPC.h
#define ConRejBusy 0x2D // defined in ARDOPC.h
#define ConRejBW 0x2E // defined in ARDOPC.h

#define ConAck200 0x39 // defined in ARDOPC.h
#define ConAck500 0x3A // defined in ARDOPC.h
#define ConAck1000 0x3B // defined in ARDOPC.h
#define ConAck2000 0x3C // defined in ARDOPC.h
#define PINGACK 0x3D // defined in ARDOPC.h
#define PING 0x3E // defined in ARDOPC.h

extern const char strFrameType[256][18];  // defined in ARDOPC.c
extern int LeaderLength;   // defined in ARDOPC.c
extern int intLastRcvdFrameQuality; // defined in ARDOPC.c

extern unsigned char bytEncodedBytes[1800]; // defined in ARDOPC.c, extern in ARDOPC.h
extern int EncLen; // defined in ARDOPC.C, extern in ARDOPC.h
extern char Callsign[10]; // defined in ARDOPC.c. extern in ARDOPC.h
extern char GridSquare[9];  // defined in ARDOPC.c. extern in ARDOPC.h
extern int ARQBandwidth; // defined as enum _ARQBandwidth ARQBandwidth in ARDOPC.c. extern in ARDOPC.h
extern int  CallBandwidth; // defined as enum _ARQBandwidth CallBandwidth in ARDOPC.c. extern in ARDOPC.h
extern int stcLastPingintRcvdSN; // defined in ARDOPC.c updated in SoundInput.c
extern int stcLastPingintQuality; // defined in ARDOPC.c updated in SoundInput.c

#define UNDEFINED 8 // Implicit in enum _ARQBandwidth in ARDOPC.h

extern UCHAR bytSessionID; // defined in ARQ.c
extern BOOL blnEnbARQRpt;  // defined in ARQ.c
extern const char ARQBandwidths[9][12];  // defined in ARQ.c. ref in ARDOPC.h

extern int intLeaderRcvdMs; // defined and updated in SoundInput.c. ref in ARQ.c

VOID WriteDebugLog(int LogLevel, const char * format, ...);  // defined in ALSASound.c and Wavout.c. ref in ardopcommon.h and ARDOPC.h

int EncodeDATANAK(int intQuality , UCHAR bytSessionID, UCHAR * bytreturn); // defined in ARDOPC.c. ref in ARDOPC.h
int Encode4FSKControl(UCHAR bytFrameType, UCHAR bytSessionID, UCHAR * bytreturn); // defined in ARDOPC.c. ref in ARDOPC.h
int Encode4FSKIDFrame(char * Callsign, char * Square, unsigned char * bytreturn); // defined in ARDOPC.c. ref in ARDOPC.h
// TODO: Fix bug changing this from BOOL to int
// Changed type of ARQBandwidth here from enum to int
BOOL EncodeARQConRequest(char * strMyCallsign, char * strTargetCallsign, int ARQBandwidth, UCHAR * bytReturn); // defined in ARDOPC.c. ref in ARDOPC.h
int EncodeConACKwTiming(UCHAR bytFrameType, int intRcvdLeaderLenMs, UCHAR bytSessionID, UCHAR * bytreturn); // defined in ARDOPC.c
int EncodePingAck(int bytFrameType, int intSN, int intQuality, UCHAR * bytreturn); // defined in ARDOPC.c
int EncodePing(char * strMyCallsign, char * strTargetCallsign, UCHAR * bytReturn);
int EncodeDATAACK(int intQuality, UCHAR bytSessionID, UCHAR * bytreturn);  // defined in ARDOPC.c. ref in ARDOPC.h
int EncodePSKData(UCHAR bytFrameType, UCHAR * bytDataToSend, int Length, unsigned char * bytEncodedBytes); // defined in ARDOPC.c. ref in ARDOPC.h
int EncodeFSKData(UCHAR bytFrameType, UCHAR * bytDataToSend, int Length, unsigned char * bytEncodedBytes);

BOOL FrameInfo(UCHAR bytFrameType, int * blnOdd, int * intNumCar, char * strMod, int * intBaud, int * intDataLen, int * intRSLen, UCHAR * bytQualThres, char * strType);  // defined in ARDOPC.c.  ref in ARDOPC.h

void Mod4FSKDataAndPlay(int Type, unsigned char * bytEncodedBytes, int Len, int intLeaderLen); // defined in Modulate.c. ref in ARDOPC.h
void ModPSKDataAndPlay(int Type, unsigned char * bytEncodedBytes, int Len, int intLeaderLen); // defined in Modulate.c. ref in ARDOPC.h


int parse_params(char *paramstr, char *parsed[10]) {
	int paramcount = 1;
	parsed[paramcount - 1] = paramstr;
	for (;;) {
		if (parsed[paramcount - 1][0] == '"') {
			// param wrapped in double quotes
			parsed[paramcount] = strchr(parsed[paramcount - 1] + 1, '"');
			if (parsed[paramcount] == NULL)
				break;
			parsed[paramcount]++;
		} else {
			parsed[paramcount] = strchr(parsed[paramcount - 1], ' ');
			if (parsed[paramcount] == NULL)
				break;
		}
		*(parsed[paramcount])++ = 0x00;
		paramcount++;
	}
	return paramcount;
}

// return 0 on success, 1 on failure
// len is the number of bytes to return which required
// 2*len hex digits.
int hex2int(char *ptr, unsigned int len, unsigned char *output) {
	unsigned char half;
	for (unsigned int i = 0; i < len; i++) {
		output[i] = 0;
		for (unsigned int j = 0; j < 2; j++) {
			half = ptr[2 * i + j];
			if (half < '0')
				return (1);
			else if (half <= '9')
				half = half - '0';
			else if (half < 'A' || half > 'f' || (half > 'F' && half < 'a'))
				return (1);
			else
				half &= 0x07;
			output[i] = (output[i] << 4) + half;
		}
	}
	return (0);
}

// return 0 on success, 1 on failure
int sendframe(char * sendParams) {
	unsigned char sessionid;
	char * params[10];
	int paramcount = parse_params(sendParams, params);
	if (paramcount < 2)
		// no frame type
		return (1);

	blnEnbARQRpt = FALSE;
	// Any param equal to "_" means use the value of the corrsponding global
	// Any missing param are equivalent to "_"

	// global LeaderLength will always be used.  However, this can be queried and
	// changed with the Host command "LEADER";

	// TODO: range check inputs
	if (strcmp(params[1], "DataNAK") == 0) {
		// 0x00 - 0x1F DataNAK
		int quality;
		// Uses globals: intLastRcvdFrameQuality, bytSessionID
		// quality from 0 to 100.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			quality = strtol(params[2], NULL, 0);
		else
			quality = intLastRcvdFrameQuality;
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 3 && strcmp(params[3], "_") != 0)
			sessionid = strtol(params[3], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send DataNAK %d 0x%02X", quality, sessionid);
		// from ARQ.c/ProcessRcvdARQFrame()
		EncLen = EncodeDATANAK(quality, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if (strcmp(params[1], "BREAK") == 0) {
		// 0x20 - 0x22 unused
		// 0x23 DataNAK
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			sessionid = strtol(params[2], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send BREAK 0x%02X", sessionid);
		EncLen = Encode4FSKControl(BREAK, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "IDLE") == 0) {
		// 0x24 IDLE
		// Uses globals: bytSessionID
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			sessionid = strtol(params[2], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send IDLE 0x%02X", sessionid);
		EncLen = Encode4FSKControl(IDLEFRAME, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "DISC") == 0) {
		// 0x25 - 0x28 unused
		// 0x29 DISC
		// Uses globals: bytSessionID
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			sessionid = strtol(params[2], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send DISC 0x%02X", sessionid);
		EncLen = Encode4FSKControl(DISCFRAME, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "END") == 0) {
		// 0x2A - 0x2B unused
		// 0x2C END
		// Uses globals: bytSessionID
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			sessionid = strtol(params[2], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send END 0x%02X", sessionid);
		EncLen = Encode4FSKControl(END, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "ConRejBusy") == 0) {
		// 0x2D ConRejBusy
		// Uses globals: bytSessionID.  (Normally uses bytPendingSessionID)
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			sessionid = strtol(params[2], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send ConRejBusy 0x%02X", sessionid);
		EncLen = Encode4FSKControl(ConRejBusy, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "ConRejBW") == 0) {
		// 0x2E ConRejBW
		// Uses globals: bytSessionID.  (Normally uses bytPendingSessionID)
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			sessionid = strtol(params[2], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send ConRejBW 0x%02X", sessionid);
		EncLen = Encode4FSKControl(ConRejBW, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "IDFrame") == 0) {
		// 0x2F unused
		// 0x30 IDFrame
		// Uses globals: Callsign, GridSquare
		char callsign[10];
		char gridsquare[9];
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			// TODO: check whether callsign is valid?
			strncpy(callsign, params[2], 9);
		else if (Callsign[0] != 0x00)
			strncpy(callsign, Callsign, 9);
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND IDFrame requires an implicit or explicit callsign.");
			return (1);
		}
		callsign[9] = 0x00;  // ensure NULL terminated
		if (paramcount > 3 && strcmp(params[3], "_") != 0)
			// TODO: check whether gridsquare is valid?
			strncpy(gridsquare, params[3], 8);
		else if (GridSquare[0] != 0x00)
			strncpy(gridsquare, GridSquare, 8);
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND IDFrame requires an implicit or explicit gridsquare.");
			return (1);
		}
		gridsquare[8] = 0x00;  // ensure NULL terminated
		WriteDebugLog(LOGDEBUG, "_Send IDFrame %s %s", callsign, gridsquare);
		EncLen = Encode4FSKIDFrame(callsign, gridsquare, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "ConReq") == 0) {
		// 0x31 to 0x38
		// All have unique names indicating bandwidth, but here use a param for bandwidth
		// or default to value set with ARQBW Host Command
		// Uses globals: Callsign. CallBandwidth, ARQBandwidth
		char targetcallsign[10];
		char callsign[10];
		int bandwidth_num; // This is an enum value, not an actual bandwith value
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			// TODO: check whether targetcallsign is valid?
			strncpy(targetcallsign, params[2], 9);
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND ConReq requires an explicit targetcallsign.");
			return (1);
		}
		targetcallsign[9] = 0x00;  // ensure NULL terminated
		if (paramcount > 3 && strcmp(params[3], "_") != 0)
			// TODO: check whether callsign is valid?
			strncpy(callsign, params[3], 9);
		else if (Callsign[0] != 0x00)
			strncpy(callsign, Callsign, 9);
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND ConReq requires an implicit or explicit callsign.");
			return (1);
		}
		callsign[9] = 0x00;  // ensure NULL terminated
		if (paramcount > 4 && strcmp(params[4], "_") != 0) {
			// processing of bandwidth is based on processing of Host Command ARQBW
			int i;
			for (i = 0; i < UNDEFINED; i++) {
				if (strcmp(params[4], ARQBandwidths[i]) == 0)
					break;
			}
			if (i == 8) {
				WriteDebugLog(LOGWARNING,
					"_SEND ConReq: invalid bandwidth='%s'.", params[4]);
				return (1);
			} else
				bandwidth_num = i;
		}
		else if (CallBandwidth != UNDEFINED)
			bandwidth_num = CallBandwidth;
		else if (ARQBandwidth != UNDEFINED)
			// Fallback if CallBandwidth is not set
			bandwidth_num = ARQBandwidth;
		else {
			// ARQBandwidth shouldn't ever be UNDEFINED, but handle this just in case
			WriteDebugLog(LOGWARNING,
				"_SEND ConReq requires an implicit or explicit bandwidth.");
			return (1);
		}
		WriteDebugLog(LOGDEBUG, "_Send ConReq %s %s %s", targetcallsign, callsign, ARQBandwidths[bandwidth_num]);
		EncLen = EncodeARQConRequest(callsign, targetcallsign, bandwidth_num, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "ConAck") == 0) {
		// 0x39 to 0x3C
		// All have unique names indicating bandwidth, but here use a param for bandwidth.
		// Unlike ConReq, ConAck does not distinguish between MAX and FORCED.  So, bandwidth
		// is specified by a number in Hz, though it will still fail if the bandwidth given
		// is not 200, 500, 1000, or 2000.
		// Uses globals: ARQBandwidth, intLeaderRcvdMs, bytSessionID
		// bandwidth from the set [200, 500, 1000, 2000].  Use of strtol() allows hex if
		// prefixed with 0x of 0X (though this is unlikely to be useful here).
		int bandwidth; // Bandwidth in Hz [200, 500, 1000, 2000]
		int frametype;
		int rcvdleaderlen;
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			bandwidth = strtol(params[2], NULL, 0);
		else if (ARQBandwidth != UNDEFINED)
			bandwidth = strtol(ARQBandwidths[ARQBandwidth], NULL, 0);
		if (bandwidth == 200)
			frametype = 0x39;
		else if (bandwidth == 500)
			frametype = 0x3A;
		else if (bandwidth == 1000)
			frametype = 0x3B;
		else if (bandwidth == 2000)
			frametype = 0x3C;
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND ConAck requires an implicit or explicit bandwidth of 200, 500, 1000, or 2000");
			return (1);
		}
		// Received leader length in MS.  Default to intLeaderRcvdMs if not provided.
		// Use of strtol() allows hex if prefixed with 0x of 0X.
		if (paramcount > 3 && strcmp(params[3], "_") != 0)
			rcvdleaderlen = strtol(params[3], NULL, 0);
		else
			rcvdleaderlen = intLeaderRcvdMs;
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 4 && strcmp(params[4], "_") != 0)
			sessionid = strtol(params[4], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send ConAck %d %d %02X (frame type = %02X)", bandwidth, rcvdleaderlen, sessionid, frametype);
		EncLen = EncodeConACKwTiming(frametype, rcvdleaderlen, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "PingAck") == 0) {
		// 0x3D
		// Uses globals: stcLastPingintRcvdSN, stcLastPingintQuality
		// SN ratio of received Ping in range of 0-21. If >= 21, result truncated.
		// Default to stcLastPingintRcvdSN if not provided.
		// Use of strtol() allows hex if prefixed with 0x of 0X.
		int snr;
		int quality;
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			// snr will be truncated to 21 for any value greater than 20
			snr = strtol(params[2], NULL, 0);
		else
			snr = stcLastPingintRcvdSN;
		// Quality in range of 0-100.  Default to stcLastPingintQuality if not provided.
		// Use of strtol() allows hex if prefixed with 0x of 0X.
		if (paramcount > 3 && strcmp(params[3], "_") != 0) {
			quality = strtol(params[3], NULL, 0);
			if (quality < 30 || quality > 1000) {
				WriteDebugLog(LOGWARNING, "_SEND PingAck requires 30 <= quality <= 100, but %d was provided.", quality);
				return (1);
			}
		} else
			quality = stcLastPingintQuality;
		WriteDebugLog(LOGDEBUG, "_Send PingAck %d %d.", snr, quality);
		EncLen = EncodePingAck(PINGACK, snr, quality, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if(strcmp(params[1], "Ping") == 0) {
		// 0x3$
		// Uses globals: Callsign
		char targetcallsign[10];
		char callsign[10];
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			// TODO: check whether targetcallsign is valid?
			strncpy(targetcallsign, params[2], 9);
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND Ping requires an explicit targetcallsign.");
			return (1);
		}
		targetcallsign[9] = 0x00;  // ensure NULL terminated
		if (paramcount > 3 && strcmp(params[3], "_") != 0)
			// TODO: check whether callsign is valid?
			strncpy(callsign, params[3], 9);
		else if (Callsign[0] != 0x00)
			strncpy(callsign, Callsign, 9);
		else {
			WriteDebugLog(LOGWARNING,
				"_SEND Ping requires an implicit or explicit callsign.");
			return (1);
		}
		callsign[9] = 0x00;  // ensure NULL terminated
		WriteDebugLog(LOGDEBUG, "_Send Ping %s %s", targetcallsign, callsign);
		EncLen = EncodePing(callsign, targetcallsign, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else if (strcmp(params[1], "DataACK") == 0) {
		// 0xE0 - 0xFF DataACK
		int quality;
		// Uses globals: intLastRcvdFrameQuality, bytSessionID
		// quality from 0 to 100.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 2 && strcmp(params[2], "_") != 0)
			quality = strtol(params[2], NULL, 0);
		else
			quality = intLastRcvdFrameQuality;
		// sessionid from 0 to 255.  Use of strtol() allows hex if prefixed with 0x of 0X
		if (paramcount > 3 && strcmp(params[3], "_") != 0)
			sessionid = strtol(params[3], NULL, 0);
		else
			sessionid = bytSessionID;
		WriteDebugLog(LOGDEBUG, "_Send DataACK %d 0x%02X", quality, sessionid);
		EncLen = EncodeDATAACK(quality, sessionid, bytEncodedBytes);
		Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
	} else {
		int frametype;
		unsigned char sessionid_bak;
		for (frametype = 0x40; frametype < 0x76; frametype ++ ) {
			if (strFrameType[frametype][0] == 0x00
				|| strcmp(params[1], strFrameType[frametype]) != 0
			)
				continue;
			unsigned char dummyuchar;
			int dummyint;
			int numcar;
			char modulation[6];
			int datalen;
			char frname[64];
			unsigned int maxlen;
			unsigned char data[1024];
			char debugmsg[2100];
			if (!FrameInfo(frametype, &dummyint, &numcar, modulation, &dummyint, &datalen, &dummyint, &dummyuchar, frname)) {
				WriteDebugLog(LOGWARNING, "_SEND %s (FrameInfo) Unknown frame type.", params[1]);
				return (1);
			}
			maxlen = datalen * numcar;
			if (maxlen > 1024) {
				WriteDebugLog(LOGWARNING, "_SEND Error.  Unexpectedly high maxlen = %d.", maxlen);
				return (1);
			}
			if (paramcount > 2 && params[2][0] != '_') {
				if (params[2][0] == '"' && params[2][strlen(params[2]) - 1] == '"') {
					// If params[2] starts and ends with double quotes, interpret everying
					// between those quotes as text to send.
					if (strlen(params[2]) - 2 <= maxlen)
						datalen = strlen(params[2]) - 2;
					else {
						WriteDebugLog(LOGWARNING,
							"_SEND %s is discarding %d bytes since only %d can be sent.",
							params[1], strlen(params[2]) - 2 - maxlen, maxlen);
						datalen = maxlen;
					}
					memcpy(data, params[2] + 1, datalen);
				} else {
					// params[2] should be an even number of hexidecimal digits with no spaces
					if (strlen(params[2]) / 2 <= maxlen)
						datalen = strlen(params[2]) / 2;
					else {
						WriteDebugLog(LOGWARNING,
							"_SEND %s is discarding %d bytes since only %d can be sent.",
							params[1], strlen(params[2]) / 2 - maxlen, maxlen);
						datalen = maxlen;
					}
					if (hex2int(params[2], datalen, data) == 1) {
						WriteDebugLog(LOGWARNING, "_SEND %s error parsing hex data.", params[1]);
						return (1);
					}
				}
			} else if (paramcount > 2 && params[2][0] == '_' && strlen(params[2]) > 1) {
				// "_" followed by a number allows the number of random bytes to be specified.
				// Use of strtol() allows hex if prefixed with 0x of 0X
				datalen = strtol(params[2] + 1, NULL, 0);
				if (datalen > (int)maxlen) {
					WriteDebugLog(LOGDEBUG, "_SEND requested %d random bytes, but only %d are allowed for %s.", datalen, maxlen, params[1]);
					datalen = maxlen;
				}
				for (int i = 0; i < datalen; i++)
					data[i] = rand() & 0xFF;
			} else {
				// use maxlen random 8-bit bytes
				datalen = maxlen;
				for (unsigned int i = 0; i < maxlen; i++)
					data[i] = rand() & 0xFF;
			}
			if (paramcount > 3 && strcmp(params[3], "_") != 0)
				sessionid = strtol(params[3], NULL, 0);
			else
				sessionid = bytSessionID;
			snprintf(debugmsg, sizeof(debugmsg), "_Send %s with %d bytes of data using sessionid=%02X: ", params[1], datalen, sessionid);
			for (int i = 0; i < datalen; i++)
				snprintf(debugmsg + strlen(debugmsg), sizeof(debugmsg) - strlen(debugmsg), " %02X", data[i]);
			WriteDebugLog(LOGDEBUG, "%s", debugmsg);
			// The various Encod*Data() functions use the global varialble bytSessionID.
			// So, to use a different value, the bytSessionID will be changed, and then
			// after it is used by one of these functions, it will be restored.
			sessionid_bak = bytSessionID;
			if (sessionid != bytSessionID)
				bytSessionID = sessionid;
			if (strcmp(modulation, "4FSK") == 0) {
				EncLen = EncodeFSKData(frametype, data, datalen, bytEncodedBytes);
				Mod4FSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
			} else if (strcmp(modulation, "4PSK") == 0 || strcmp(modulation, "8PSK") == 0) {
				EncLen = EncodePSKData(frametype, data, datalen, bytEncodedBytes);
				ModPSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
			} else if (strcmp(modulation, "16QAM") == 0) {
				EncLen = EncodePSKData(frametype, data, datalen, bytEncodedBytes);
				ModPSKDataAndPlay(bytEncodedBytes[0], &bytEncodedBytes[0], EncLen, LeaderLength);
			} else {
				bytSessionID = sessionid_bak;
				WriteDebugLog(LOGWARNING, "_SEND: Unexpected modulation='%s' for frame type=%s", modulation, params[1]);
				return (1);
			}
			bytSessionID = sessionid_bak;
			return (0);
		}
		if (frametype == 0x76) {
			WriteDebugLog(LOGWARNING, "_SEND: Unknown frame type=%s", params[1]);
			return (1);
		}
	}
	return (0);
}
