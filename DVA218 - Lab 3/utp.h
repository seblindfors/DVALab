/*
	UDP Transmission Protocol: Interface
	P2P messaging using UDP sockets
	Author: Sebastian Lindfors
*/

#ifndef UTP
#define UTP
#define UTP_ERROR 	// Remove this to run clean

// System headers
#include <sys/time.h> 	// Sequence number generation
#include <string.h>	// String comparisons
#include <stdlib.h>	// Heap allocation
#include <stdio.h> 	// printf(), fgets()

// IP & transport header
#include <arpa/inet.h>

// Checksum (sudo apt-get install libssl-dev)
#include <openssl/md5.h>

// Bit flags to define type of message
#define MSG (uint8_t) 0  // 0000 0000
#define NAK (uint8_t) 1  // 0000 0001
#define ACK (uint8_t) 2  // 0000 0010
#define SYN (uint8_t) 4  // 0000 0100
#define FIN (uint8_t) 8  // 0000 1000
// Upper flags to parse extra info
#define END (uint8_t) 16 // 0001 0000
#define REQ (uint8_t) 32 // 0010 0000
#define RES (uint8_t) 64 // 0100 0000

// Default parameters
#define UTP_DEFAULT_PORT 	5555
#define UTP_DEFAULT_WSIZE	16
#define UTP_DEFAULT_PSIZE	32
#define UTP_DEFAULT_TIMEOUT 	60000
#define UTP_HANDSHAKE_SIZE 	16
#define UTP_TEARDOWN_MAX 	16

// Frame structure (package)
struct utp_pack {
	int16_t size;				// Payload size
	int64_t seq;				// Sequence number
	int64_t time;				// Timestamp
	uint8_t flags;				// Flags bit field
	char 	md5[MD5_DIGEST_LENGTH]; 	// Checksum
	char	msg[];				// Dynamic payload
};

// Connection tracker
struct utp_conn {
	int32_t sock;				// Store socket ID
	int64_t seqSend;			// Init sequence
	int64_t seqRecv;			// Init sequence
	struct sockaddr_in local;	// Local address
	struct sockaddr_in remote;	// Remote address
};

// Track status of packet sequences
struct utp_tracker {
	int64_t sendLast;			// Highest send sequence
	int64_t recvLast;			// Highest recv sequence
	int64_t sendNext;			// Offset from init seq
	int64_t recvNext;			// Offset from init seq
};

// Window buffers
struct utp_window {
	struct utp_pack* send;		// Store sent frames
	struct utp_pack* recv;		// Store recv frames
	struct utp_pack* acks;		// Store acks for sent frames
};

/*******************************************************/
//////	Payload & window size setting/getting
void 	UTP_FORCE_WINDOW_SIZE(int size);
void 	UTP_FORCE_PAYLOAD_SIZE(int size);

void 	UTP_SET_WINDOW_SIZE(int recvSize, int sendSize);
void 	UTP_SET_PAYLOAD_SIZE(int recvSize, int sendSize);

int 	UTP_GET_WINDOW_SIZE();
int 	UTP_GET_FRAME_SIZE();
int 	UTP_GET_PAYLOAD_SIZE();

//////	MD5 hash wrappers
void 	UTP_MD5_PREPARE(char* md5);
void 	UTP_MD5_ADD(struct utp_pack* frame);
int 	UTP_MD5_VERIFY(struct utp_pack* frame);

//////	Timer and timeout
int64_t UTP_TIME();
int64_t UTP_GET_TIMEOUT();
void 	UTP_SET_TIMEOUT(int64_t timeout);
int 	UTP_TIMEOUT_EXPIRED(int64_t timestamp);

//////	Flags
int 	UTP_FLAG(struct utp_pack* frame, uint8_t option);
int 	UTP_FLAG_EXACT(struct utp_pack* frame, uint8_t option);
void	UTP_FLAG_ADD(struct utp_pack* frame, uint8_t option);
uint8_t UTP_TYPE(uint8_t flags);

//////	Pack preparation
void 	UTP_PACK_PROPERTIES(struct utp_pack* frame, int16_t size, int64_t seq, uint8_t flags);
void 	UTP_PACK_HANDSHAKE(struct utp_pack* frame, int64_t seq, uint8_t flags, int16_t psize, int16_t wsize);
void 	UTP_PACK_MESSAGE(struct utp_pack* frame, char* msg, int64_t seq, uint8_t flags);

//////	Message handling
int 	UTP_RECV(struct utp_conn* conn, struct utp_pack* frame, int timeout);
int 	UTP_SEND(struct utp_conn* conn, struct utp_pack* frame);

/*******************************************************/
//////	Connect/start
int 	UTP_OPEN_RECV(struct utp_conn *conn, int argc, char* argv[]);
int 	UTP_OPEN_SEND(struct utp_conn *conn, int argc, char* argv[]);
void 	UTP_HELP();

//////	Teardown
int 	UTP_CLOSE_RECV(struct utp_conn* conn, struct utp_pack* frame);
int 	UTP_CLOSE_SEND(struct utp_conn* conn, struct utp_pack* frame);
/*******************************************************/

#endif
