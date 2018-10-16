/*
	UDP Transmission Protocol: Implementation
	P2P messaging using UDP sockets
	Author: Sebastian Lindfors
*/

#include "utp.h"

fd_set getSelectSet(int fd) {
	fd_set set;
	FD_ZERO(&set);
	FD_SET(fd, &set);
	return set;
}

struct timeval getSelectTimeout(int timeout) {
	struct timeval t;
	t.tv_sec = 0;
	t.tv_usec = timeout;
	return t;
}

/*******************************************************/
// Payload & window size setting/getting
int UTP_WINDOW  = 1;
int UTP_PAYLOAD = UTP_HANDSHAKE_SIZE;
int UTP_TIMEOUT = UTP_DEFAULT_TIMEOUT;


//////	Payload & window size setting/getting
void UTP_FORCE_WINDOW_SIZE(int size) {
	UTP_WINDOW = size > 1 ? size : UTP_WINDOW;
}

void UTP_FORCE_PAYLOAD_SIZE(int size) {
	UTP_PAYLOAD = size > 1 ? size : UTP_PAYLOAD;
}

void UTP_SET_WINDOW_SIZE(int recvSize, int sendSize) {
	UTP_FORCE_WINDOW_SIZE(recvSize > sendSize ? sendSize : recvSize);
}
void UTP_SET_PAYLOAD_SIZE(int recvSize, int sendSize) {
	UTP_FORCE_PAYLOAD_SIZE(recvSize > sendSize ? sendSize : recvSize);
}


int UTP_GET_WINDOW_SIZE() {
	return UTP_WINDOW;
}

int UTP_GET_PAYLOAD_SIZE() {
	return UTP_PAYLOAD;
}

int UTP_GET_FRAME_SIZE() {
	return sizeof(struct utp_pack) + (UTP_PAYLOAD * sizeof(char));
}

int UTP_GET_BUFFER_SIZE(int numFrames) {
	return UTP_GET_FRAME_SIZE() * numFrames;
}


//////	MD5 hash wrappers
void UTP_MD5_PREPARE(char* md5) {
	memset(md5, 0, MD5_DIGEST_LENGTH);
}

void UTP_MD5_ADD(struct utp_pack* frame) {
	char md5[MD5_DIGEST_LENGTH];
	UTP_MD5_PREPARE(frame->md5);
	MD5((unsigned char *) frame, UTP_GET_FRAME_SIZE(), md5);
	memcpy(frame->md5, md5, MD5_DIGEST_LENGTH);
}

int UTP_MD5_VERIFY(struct utp_pack* frame) {
	// create a local receive hash.
	char recv[MD5_DIGEST_LENGTH];
	UTP_MD5_PREPARE(recv);

	// (1) copy hash from frame.
	// (2) calculate a local version of the hash.
	memcpy(recv, frame->md5, MD5_DIGEST_LENGTH);
	UTP_MD5_ADD(frame);

	// compare the received hash to the local version.
	return (memcmp(recv, frame->md5, MD5_DIGEST_LENGTH) == 0) ? 1 : 0;
}


//////	Timer and timeout
int64_t UTP_TIME() {
	struct timeval t;
	gettimeofday(&t, NULL);
	return t.tv_sec * 1000000 + t.tv_usec;
}

int64_t UTP_GET_TIMEOUT() {
	return UTP_TIMEOUT;
}

void UTP_SET_TIMEOUT(int64_t timeout) {
	UTP_TIMEOUT = timeout;
}

int UTP_TIMEOUT_EXPIRED(int64_t timestamp) {
	int64_t now = UTP_TIME();
	return (timestamp + UTP_TIMEOUT) < now ? 1 : 0;
}


//////	Flags
int UTP_FLAG(struct utp_pack* frame, uint8_t option) {
	// Bitwise AND with option flag to check if it's set.
	return (((frame->flags) & option) == option);
}

int UTP_FLAG_EXACT(struct utp_pack* frame, uint8_t option) {
	return frame->flags == option;
}

void UTP_FLAG_ADD(struct utp_pack* frame, uint8_t option) {
	// Bitwise OR with option flag to include it in frame.
	frame->flags = frame->flags | option;
}

uint8_t UTP_TYPE(uint8_t flags) {
	// Remove the upper bits for various flags to extract the message type flag.
	flags = flags << 4;
	return  flags >> 4;
}


//////	Pack preparation
/*--------------------------------------------------
 * UTP_PACK_PROPERTIES
 *--------------------------------------------------
 * This function can be used to prepare a message
 * without a payload, such as an ACK, FIN etc,
 * but also functions as a general preparation call
 * to reset the payload buffer and set up parameters.
 *--------------------------------------------------*/
void UTP_PACK_PROPERTIES(struct utp_pack* frame, int16_t size, int64_t seq, uint8_t flags) {
	frame->flags 	= flags;
	frame->size  	= size;
	frame->seq 		= seq;
	memset(frame->msg, 0, UTP_PAYLOAD);
}

/*--------------------------------------------------
 * UTP_PACK_HANDSHAKE
 *--------------------------------------------------
 * This is a special kind of message that expects
 * handshake parameters, neatly fitted into a regular
 * package struct. This allows the API to function on
 * a single package type, but requires special parsing
 * when received on a socket. It should only be used
 * to negotiate terms between peers when connecting.

 * PROPERTIES:
	frame->seq 	: connection sequence number (seq)
	frame->flags: SYN-related flags (flags)
 	frame->size : maximum frame size (psize)
	frame->msg 	: window size converted to char array (wsize)
 *--------------------------------------------------*/
void UTP_PACK_HANDSHAKE(struct utp_pack* frame, int64_t seq, uint8_t flags, int16_t psize, int16_t wsize) {
	UTP_PACK_PROPERTIES(frame, UTP_HANDSHAKE_SIZE, seq, flags);
	frame->size = psize;
	sprintf(frame->msg, "%d", wsize);
}


/*--------------------------------------------------
 * UTP_PACK_MESSAGE
 *--------------------------------------------------
 * This function prepares a frame as a data message
 * by copying a data stream into the payload buffer,
 * and calculating payload size and flags depending
 * on the number of bytes read from the data stream.
 *--------------------------------------------------*/
void UTP_PACK_MESSAGE(struct utp_pack* frame, char* stream, int64_t seq, uint8_t flags) {
	// (1) Handle payload overflow.
	// (2) Append END as flag if message fits in single frame.
	int msgLength 	= strlen(stream);
	int overflow 	= (msgLength > UTP_PAYLOAD) ? 1 : 0;
	int psize 		= overflow ? UTP_PAYLOAD : msgLength;
	int modflag 	= overflow ? UTP_TYPE(flags) : (END | flags);

	UTP_PACK_PROPERTIES(frame, psize, seq, modflag);
	memcpy(frame->msg, stream, psize);
}


//////	Message handling
/*--------------------------------------------------
 * UTP_RECV
 *--------------------------------------------------
 * RETURN VALUE:
 	1: successful read from socket, checksum verified
 	0: successful read from socket, checksum failed
 	0: timeout
 *--------------------------------------------------*/
int UTP_RECV(struct utp_conn* conn, struct utp_pack* frame, int timeout) {
	fd_set  			fdrd = getSelectSet(conn->sock);
	struct timeval	 	time = getSelectTimeout(timeout);
	struct sockaddr* 	addr = (struct sockaddr *) &(conn->remote);
	int32_t 			size = (int32_t) UTP_GET_FRAME_SIZE();
	int32_t				alen = sizeof(conn->remote);

	if (select(conn->sock + 1, &fdrd, NULL, NULL, &time)) {
		recvfrom(conn->sock, frame, size, 0, addr, &alen);
		return UTP_MD5_VERIFY(frame);
	}
	return 0;
}


////////////////////////////////
//     CLEAN SEND FUNCTION    //
////////////////////////////////
#ifndef UTP_ERROR

int UTP_SEND(struct utp_conn* conn, struct utp_pack* frame) {
	struct sockaddr* 	addr = (struct sockaddr *) &(conn->remote);
	int32_t 			size = (int32_t) UTP_GET_FRAME_SIZE();
	int32_t				alen = sizeof(conn->remote);

	frame->time = UTP_TIME();
	UTP_MD5_ADD(frame);
	return sendto(conn->sock, frame, size, 0, addr, alen);
}

////////////////////////////////
//   VOLATILE SEND FUNCTION   //
////////////////////////////////
#else

int bonkers = 0;

int UTP_SEND(struct utp_conn* conn, struct utp_pack* frame) {
	struct sockaddr* 	addr = (struct sockaddr *) &(conn->remote);
	int32_t 			size = (int32_t) UTP_GET_FRAME_SIZE();
	int32_t				alen = sizeof(conn->remote);

	frame->time = UTP_TIME();
	UTP_MD5_ADD(frame);

	// break something
	if ((rand() % 100) + (!bonkers) < bonkers) {
		// break checksum (cause resend)
		if ((rand() % 2)) {
			frame->md5[(rand() % MD5_DIGEST_LENGTH)] += (rand() % 10);
			return sendto(conn->sock, frame, size, 0, addr, alen);
		}
		// skip the sendto call (cause request)
	}
	else {
		return sendto(conn->sock, frame, size, 0, addr, alen);
	}
	return 0;
}

#endif


/*******************************************************/
/* Connect/start and teardown functions using the API  */

//////	Teardown
int UTP_CLOSE_RECV(struct utp_conn* conn, struct utp_pack* frame) {
	int countdown = UTP_TEARDOWN_MAX;
	memset(frame, 0, UTP_GET_FRAME_SIZE());

	while(!UTP_FLAG_EXACT(frame, ACK)) {
		UTP_PACK_PROPERTIES(frame, 0, frame->seq, FIN | ACK);
		UTP_SEND(conn, frame);
		UTP_RECV(conn, frame, UTP_GET_TIMEOUT());
		if (countdown-- < 0)
			return 0;
	}
	return 1;
}

int UTP_CLOSE_SEND(struct utp_conn* conn, struct utp_pack* frame) {
	int countdown = UTP_TEARDOWN_MAX;
	memset(frame, 0, UTP_GET_FRAME_SIZE());

	// Send FIN until a FIN|ACK is received.
	while(!UTP_FLAG_EXACT(frame, FIN | ACK)) {
		UTP_PACK_PROPERTIES(frame, 0, conn->seqSend++, FIN);
		UTP_SEND(conn, frame);
		UTP_RECV(conn, frame, UTP_TIMEOUT);
		if (countdown-- < 0)
			return 0;
	}
	// FIN was acknowledged, reset limiter and send final ACK.
	countdown = UTP_TEARDOWN_MAX;
	
	do { // Send final ACK while FIN|ACK is still flagged on recv frame.
		UTP_PACK_PROPERTIES(frame, 0, conn->seqSend++, ACK);
		UTP_SEND(conn, frame);
		if (countdown-- < 0)
			return 0;
	} while(UTP_RECV(conn, frame, UTP_TIMEOUT) && UTP_FLAG_EXACT(frame, FIN | ACK));

	return 1;
}



int cmdParse(char* param, int fallback, int offset, int argc, char* argv[]) {
	//printf("%d offset, %d argc\n", offset, argc);
	for(int i = offset; i < argc; i++)
		if (strcmp(argv[i], param) == 0)
			return atoi(argv[i+1]);
	return fallback;
}


void printHandshake() {
	// Print handshake parameters (negotiated values)
	printf("Handshake parameters:\n");
	printf("Window size: %d frames.\n", ((int) UTP_GET_WINDOW_SIZE()));
	printf("Frame size: %d bytes.\n", ((int) UTP_GET_FRAME_SIZE()));
	printf("Payload size: %d bytes.\n", ((int) UTP_GET_PAYLOAD_SIZE()));
}

void createInconsistency(int argc, char* argv[], int offset) {
#ifdef UTP_ERROR
	printf("-----------------------------\n");
	// Seed chance to mess with package on send
	bonkers	= cmdParse("-error", bonkers, offset, argc, argv);
	bonkers = bonkers > 99 ? 99 : bonkers;
	printf("%d%% chance to go bonkers.\n", bonkers);

	// Set up default timeout value (slow/fast selective repeat)
	UTP_SET_TIMEOUT(cmdParse("-timer", UTP_DEFAULT_TIMEOUT, 3, argc, argv));
	printf("Local timeout in usec: %ld\n", UTP_GET_TIMEOUT());
	printf("-----------------------------\n");
#endif
}


int UTP_OPEN_RECV(struct utp_conn *conn, int argc, char* argv[]) {
	int port 	= cmdParse("-port",	 UTP_DEFAULT_PORT,    2, argc, argv);
	int wsize 	= cmdParse("-wsize", UTP_DEFAULT_WSIZE,   2, argc, argv);
	int psize 	= cmdParse("-psize", UTP_DEFAULT_PSIZE,   2, argc, argv);

	memset(&(conn->remote), 0, sizeof(conn->remote));

	// Create potential to simulate unstable connection
	createInconsistency(argc, argv, 2);

	conn->local.sin_family 			= AF_INET;
	conn->local.sin_port 			= htons(port);
	conn->local.sin_addr.s_addr 	= htonl(INADDR_ANY);

	conn->seqSend	= UTP_TIME();
	conn->sock 		= socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (bind(conn->sock,
			(struct sockaddr *) &(conn->local),
			sizeof(conn->local)) < 0 ) {
		printf("Failed to bind socket.\n");
		return 1;
	}

	struct utp_pack* frame = malloc(sizeof(*frame) + UTP_HANDSHAKE_SIZE * sizeof(char));


	printf("Waiting for connection...\n");
	// Wait for a SYN from connecting client
	while(!UTP_FLAG_EXACT(frame, SYN))
		UTP_RECV(conn, frame, UTP_TIMEOUT);

	// Set up parameters
	UTP_SET_WINDOW_SIZE(wsize, atoi(frame->msg));
	UTP_SET_PAYLOAD_SIZE(psize, frame->size);


	printf("SYN received.\n");
	printf("Peer address: %s\n", inet_ntoa(conn->remote.sin_addr));
	printHandshake();
	
	printf("Sending SYNACK to client.\n");

	// Return with a SYNACK and the frame/window size.
	while(!UTP_FLAG_EXACT(frame, ACK)) {
		UTP_PACK_HANDSHAKE(frame, conn->seqSend++, (SYN|ACK), UTP_PAYLOAD, UTP_WINDOW);
		UTP_SEND(conn, frame);
		UTP_RECV(conn, frame, UTP_TIMEOUT);
	}

	printf("Final ACK received. Initial sequence: %05hhu\n", ((uint8_t) frame->seq));
	conn->seqRecv = frame->seq;

	free(frame);
	return 0;
}



int UTP_OPEN_SEND(struct utp_conn *conn, int argc, char* argv[]) {
	int port 	= cmdParse("-port",	 UTP_DEFAULT_PORT,    3, argc, argv);
	int wsize 	= cmdParse("-wsize", UTP_DEFAULT_WSIZE,   3, argc, argv);
	int psize 	= cmdParse("-psize", UTP_DEFAULT_PSIZE,   3, argc, argv);

	memset(&(conn->remote), 0, sizeof(conn->remote));

	// Create potential to simulate unstable connection
	createInconsistency(argc, argv, 3);

	conn->remote.sin_family 		= AF_INET;
	conn->remote.sin_port 			= htons(port);
	conn->remote.sin_addr.s_addr 	= inet_addr(argv[2]);

	conn->seqSend	= UTP_TIME();
	conn->sock 		= socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);

	// Set up initial hanshake frame
	struct utp_pack* frame = malloc(sizeof(*frame) + UTP_HANDSHAKE_SIZE * sizeof(char));
	
	// Send SYN to server.
	printf("Connecting to peer...\n");
	printf("SYN sent to %s...\n", argv[2]);
	printf("Waiting for SYNACK...\n");

	while (!UTP_FLAG_EXACT(frame, (SYN|ACK))) {
		UTP_PACK_HANDSHAKE(frame, conn->seqSend++, SYN, psize, wsize);
		frame->size = psize;
		UTP_SEND(conn, frame);
		UTP_RECV(conn, frame, UTP_TIMEOUT);
	}

	printf("SYNACK received. Initial sequence: %05hhu\n", ((uint8_t) frame->seq));

	// SYNACK received, should contain handshake parameters
	UTP_SET_WINDOW_SIZE(wsize, atoi(frame->msg));
	UTP_SET_PAYLOAD_SIZE(psize, frame->size);

	printHandshake();

	do { // Send final ACK while SYN|ACK is still flagged on recv frame.
		conn->seqRecv = frame->seq;
		UTP_PACK_HANDSHAKE(frame, conn->seqSend++, ACK, UTP_PAYLOAD, UTP_WINDOW);
		UTP_SEND(conn, frame);
	} while(UTP_RECV(conn, frame, UTP_TIMEOUT) && UTP_FLAG(frame, (SYN|ACK)));

	printf("Sending final ACK...\n");

	free(frame);
	return 0;
}



void UTP_HELP() {
	printf("UTP Interface Simulation Help:\n");
	printf("./program server [-flags]\n");
	printf("./program client <address> [-flags]\n");
	printf("-wsize <num>: Window size\n");
	printf("-psize <num>: Payload size\n");
	printf("-port <num>: Port number\n");
#ifdef UTP_ERROR
	printf("-error <num>: Error sim percent\n");
	printf("-timer <num>: Timeout in usec\n");
#endif
}
