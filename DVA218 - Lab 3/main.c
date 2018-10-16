/*
	UDP Transmission Protocol: Example program
	P2P messaging using UDP sockets
	Author: Sebastian Lindfors
*/

#include "utp.h"
#include <pthread.h>
#include <unistd.h>

#define VERBOSE				// Remove this to prevent debug output
#define NIL		"___"		// Print when state is missing from i/o info
#define BUFFER_SIZE 	1024 		// General buffer size for input/output
#define THREAD_SLEEP 	20000		// How long should selective repeat sleep
#define QUIT_MSG 		"QUIT\n" 	// What to check for in input stream to send FIN


int  	running = 1, tdClean = 0; 	// (1) Thread condition (2) Clean exit
int32_t wsize, fsize, psize; 		// Size parameters: window, frame, payload

pthread_mutex_t 	fileAccess; 	// Mutex lock for socket writing/reading.

struct 	utp_window 	buffer;		// Buffers for send, recv and acks.
struct 	utp_tracker 	status;		// Status tracker for frame sequences.
struct 	utp_conn 	conn;		// Connection structure.
struct 	utp_pack* 	frame;		// Shared frame for sequential send/recv.

/*--------------------------------------------------
 * Helper functions
 *--------------------------------------------------*/
int sequenceInSpan(int64_t seq, int64_t offset) {
	// Determines if a frame sequence and related tracker offset
	// are aligned, in order to weed out packages that have already
	// arrived, packages that haven't been acknowledged, 
	// and packages that are not part of the current set.
	int64_t idx = (seq - offset);
	return 	(idx >= 0) && (idx < wsize);
}

struct utp_pack* getFrame(int64_t index, struct utp_pack* buffer) {
	// Returns a frame pointer in a buffer given an index offset
	return buffer + (index * fsize);
}

pthread_t createThreadForFunction(void* function) {
	// Create a new thread for a given function and return the handler.
	pthread_t thread;
	pthread_create(&thread, NULL, function, NULL);
	return thread;
}

int readInputWithQuit(char* stream, int* offset) {
	// (1) read a line from stdin and store in buffer.
	// (2) check for quit message.
	// (3) increase cursor offset.
	// (4) remove trailing newline.
	fgets(stream + *offset, BUFFER_SIZE - *offset, stdin);
	int quitRequest = !strcmp(stream + *offset, QUIT_MSG);
	*offset += strlen(stream + *offset) - 1;
	memset(stream + *offset, 0, 1);
	return quitRequest;
}

void _p(char* in, char* out, int64_t offs, int64_t seq, int64_t time, char* msg) {
// Prints <INPUT | OUTPUT> and message information
#ifdef VERBOSE //////////////////
	printf("<%s | %s> ", in, out);
	printf("%+04hhd ",((int8_t) offs));
	printf(" %05hhu ",((uint8_t) time));
	printf(" %05hhu ",((uint8_t) seq));
	printf("%s\n", 		msg);
#endif //////////////////////////
}

void debug(uint8_t input, uint8_t output) {
// Wrapper for message printing to keep this file more readable.
#ifdef VERBOSE //////////////////
	int64_t seq = frame->seq;
	int64_t tim = frame->time;
	char* 	msg = frame->msg;
	switch(input) {
		case NAK: _p(UTP_FLAG(frame,REQ)?"REQ":"NAK", "MSG", seq - status.sendNext, seq, tim, msg); return;
		case ACK: _p("ACK", NIL, seq - status.sendNext, seq, tim, ""); return;
		case FIN: _p("FIN", "ACK", 0, seq, tim, ""); return;
		case MSG: _p(UTP_FLAG(frame,END)?"END":"MSG", "ACK", seq-status.recvNext, seq, tim, msg); return;
	}
	switch(output) {
		case NAK: _p(NIL, "NAK", seq - status.recvNext, seq, tim, ""); return;
		case MSG: _p(NIL, UTP_FLAG(frame,END)?"END":"MSG", seq-status.sendNext, seq, tim, msg); return;
	}
#endif //////////////////////////
}

/*--------------------------------------------------
 *	Automatic repeat request (threads)
 *--------------------------------------------------
 * 	resend: 	resends a frame that has timed out
 * 	request: 	requests a frame that never arrived
 *--------------------------------------------------*/
void* resend(void* args) {
	while(running) {
		pthread_mutex_lock(&fileAccess);

		// Are there missing ACKs for sent frames?
		if (sequenceInSpan(status.sendLast, status.sendNext)) {

			int64_t lastIndex = status.sendLast - status.sendNext;
			int64_t sendSeq, acksSeq, sendTime;
			struct utp_pack* resPack;
			
			for(int64_t i = 0; i <= lastIndex; i++) {
				acksSeq  = getFrame(i, buffer.acks)->seq;
				resPack  = getFrame(i, buffer.send);
				sendSeq  = resPack->seq;
				sendTime = resPack->time;

				// If the sequence number on sent frame and received ack doesn't line up,
				// and the timeout for the frame has expired -> resend the frame
				if (sendSeq != acksSeq && UTP_TIMEOUT_EXPIRED(sendTime)) {
					_p(NIL,"RES", sendSeq-status.sendNext, sendSeq, sendTime, resPack->msg);
					UTP_FLAG_ADD(resPack, RES);
					UTP_SEND(&conn, resPack);
				}
			}
		}
		pthread_mutex_unlock(&fileAccess);
		usleep(THREAD_SLEEP);
	}
}


void* request(void* args) {
	while(running) {
		pthread_mutex_lock(&fileAccess);

		// Are there missing frames?
		if (sequenceInSpan(status.recvLast, status.recvNext)) {
			int64_t lastIndex = status.recvLast - status.recvNext;

			// Use last received frame as baseline as to WHEN requests should be sent.
			// If the last frame has timed out, it's a good time to start checking the
			// receive buffer for missing frames that need to be requested from sender.
			if (UTP_TIMEOUT_EXPIRED(getFrame(lastIndex, buffer.recv)->time)) {
				int64_t recvSeq;

				for(int64_t i = 0; i <= lastIndex; i++) {
					recvSeq = getFrame(i, buffer.recv)->seq;

					// If the received sequence number and the expected sequence number
					// does not form the correct buffer index, the frame is missing.
					if (recvSeq - status.recvNext != i) {
						UTP_PACK_PROPERTIES(frame, 0, status.recvNext + i, NAK | REQ);
						debug(-1, NAK);
						UTP_SEND(&conn, frame);
					}
				}
			}
		}
		pthread_mutex_unlock(&fileAccess);
		usleep(THREAD_SLEEP);
	}
}

/*--------------------------------------------------
 * Sliding window (utility functions)
 *--------------------------------------------------*/
// Shift a frame buffer by one frame ([0] << [1] << [2] etc)
void slide(void* dest) {
	memmove(dest, dest + fsize, (wsize - 1) * fsize);
}

// Inserts a frame into a buffer at a calculated index
void insert(void* dest, int64_t seq, int64_t offset) {
	int position = (seq - offset) * fsize;
	memcpy(dest + position, frame, fsize);
}

/*--------------------------------------------------
 * Sliding window (send)
 *--------------------------------------------------
 * sendFrames: 
 *	Splits input stream into frames and transmits
 * 	the message sequentially on socket.
 * slideWindow:
 * 	Slides the sending window forward when the
 * 	first frame on the link has been acknowledged.
 *--------------------------------------------------*/
void sendFrames(char* input, int* inPos, int* frameCount) {
	// assert window isn't overflown and that input stream has data
	while(*frameCount < wsize && *inPos > 0) {
		// populate frame with message from input stream
		UTP_PACK_MESSAGE(frame, input, conn.seqSend++, MSG);

		// check if input stream overflows payload
		if (*inPos > psize) {
			// (1) move input stream to exclude the MSG that frame was populated with.
			// (2) prepare the last payload chunk to read from input stream.
			// (3) reduce the position counter by one payload chunk.
			memmove(input, input + psize, BUFFER_SIZE - psize);
			memset(input + *inPos - psize, 0, psize);
			*inPos -= psize;
		}
		// entire input buffer fits in payload, reset the input buffer.
		else {
			memset(input, 0, BUFFER_SIZE);
			*inPos = 0;
		}

		// (1) send the prepared frame.
		// (2) copy the frame into the send buffer for potential resends.
		// (3) update the status tracker with the last frame sequence number.
		// (4) increment the frame counter/decrease sliding window potential.
		UTP_SEND(&conn, frame);
		insert(buffer.send, frame->seq, status.sendNext);
		status.sendLast = frame->seq;
		*frameCount += 1;
		debug(-1, MSG);
	}
}

void slideWindow(int* frameCount) {
	// while 1st ack lines up with send offset !(result is zero)
	while (!(buffer.acks->seq - status.sendNext)) {
		// (1) shift frame buffers by one frame
		// (2) increment send offset
		// (3) decrement the frame counter/increase sliding window potential.
		slide(buffer.send);
		slide(buffer.acks);
		status.sendNext++;
		*frameCount -= 1;
	}
}

/*--------------------------------------------------
 * Sliding window (receive)
 *--------------------------------------------------
 * Iterates over received packages and slides the
 * window forward when received sequence number and
 * expected sequence number are aligned.
 *--------------------------------------------------*/
void processReceived(char* output, int* offset) {
	// while 1st recv lines up with offset !(result is zero)
	while (!(buffer.recv->seq - status.recvNext)) {
		int16_t msgSize = buffer.recv->size;
		char* 	msg 	= buffer.recv->msg;

		// copy frame payload into output buffer
		memcpy(output + *offset, msg, msgSize);
		*offset += msgSize;

		// end of message is flagged in this frame, print buffer and reset.
		if (UTP_FLAG(buffer.recv, END)) {
			printf("> %s\n", output);
			memset(output, 0, BUFFER_SIZE);
			*offset = 0;
		}

		// slide the receiving window forward, increase offset.
		slide(buffer.recv);
		status.recvNext++;
	}
}

/*--------------------------------------------------
 * Event handler (main thread)
 *--------------------------------------------------
 * Handles messaging with peer, reads data from
 * socket and stdin, and writes frames to socket.
 *--------------------------------------------------*/
void* eventHandler(void* args) {
	char* 	input = calloc(BUFFER_SIZE, sizeof(char));
	char* 	output = calloc(BUFFER_SIZE, sizeof(char));
	int 	inPos = 0, outPos = 0;

	fd_set 	files;
	int 	frameCount = 0;

	while(running) {
		FD_ZERO(&files);
		FD_SET(conn.sock, &files);
		FD_SET(fileno(stdin), &files);

		// Wait for message on socket(n) or stdin(0).
		// select(n, ...) uses i < n so need to increment span by 1 to catch socket.
		select(conn.sock + 1, &files, NULL, NULL, NULL);
		pthread_mutex_lock(&fileAccess);

		// A frame was successfully received on the socket.
		if(UTP_RECV(&conn, frame, 0)) {
			switch(UTP_TYPE(frame->flags)) {
				
				case NAK:
					debug(NAK, MSG);
					UTP_SEND(&conn, getFrame(frame->seq - status.sendNext, buffer.send));
					break;


				case MSG:
					debug(MSG, ACK);
					if (sequenceInSpan(frame->seq, status.recvNext)) {
						insert(buffer.recv, frame->seq, status.recvNext);

						if (frame->seq > status.recvLast)
							status.recvLast = frame->seq;

						processReceived(output, &outPos);
					}
					// respond with ACK regardless of whether this frame is expected.
					UTP_PACK_PROPERTIES(frame, 0, frame->seq, ACK);
					UTP_SEND(&conn, frame);
					break;


				case ACK:
					debug(ACK, -1);
					if (sequenceInSpan(frame->seq, status.sendNext)) {
						insert(buffer.acks, frame->seq, status.sendNext);
						slideWindow(&frameCount);
					}
					sendFrames(input, &inPos, &frameCount);
					break;


				case FIN:
					debug(FIN, ACK);
					running = 0;
					tdClean = UTP_CLOSE_RECV(&conn, frame);
					break;


			}
		}

		// There's a line of text available on stdin.
		else if (FD_ISSET(fileno(stdin), &files)) {
			if(readInputWithQuit(input, &inPos)) {
				running = 0;
				tdClean = UTP_CLOSE_SEND(&conn, frame);
			}
			else {
				sendFrames(input, &inPos, &frameCount);
			}
		}
		pthread_mutex_unlock(&fileAccess);
	}

	free(input);
	free(output);
}

/*--------------------------------------------------
 * Main: setup, start threads, teardown.
 *--------------------------------------------------*/
int main(int argc, char* argv[]) {
	// Start host?
	if (argc > 1 && (!strcmp(argv[1], "listen") || !strcmp(argv[1], "server"))) {
		running != UTP_OPEN_RECV(&conn, argc, argv);
	}
	// start peer?
	else if (argc > 1 && (!strcmp(argv[1], "connect") || !strcmp(argv[1], "client"))) {
		running != UTP_OPEN_SEND(&conn, argc, argv);
	}
	else {
		UTP_HELP();
		running = 0;
	}

	if (running) {
		srand(time(NULL));
		printf("Connection established.\n");
		printf("-----------------------------\n");
	#ifdef VERBOSE
		printf("Verbose printout notation:\n");
		printf("< IN | OUT> [frame] [time] [sequence] [msg]\n");
		printf("-----------------------------\n");
	#endif

		// Set up size parameters.
		wsize = UTP_GET_WINDOW_SIZE();
		fsize =	UTP_GET_FRAME_SIZE();
		psize = UTP_GET_PAYLOAD_SIZE();

		// Allocate the frame to hold data.
		frame = malloc(fsize);

		// Initialize window buffers
		buffer.recv = calloc(wsize, fsize);
		buffer.send = calloc(wsize, fsize);
		buffer.acks = calloc(wsize, fsize);

		// Initialize status tracker
		status.sendLast = 0;
		status.recvLast = 0;
		status.sendNext = conn.seqSend;
		status.recvNext = conn.seqRecv + 1;

		// Start threads
		pthread_t _resend  = createThreadForFunction(resend);
		pthread_t _request = createThreadForFunction(request);
		pthread_t _events  = createThreadForFunction(eventHandler);

		// Wait for threads to finish (exit).
		pthread_join(_resend, 	NULL);
		pthread_join(_request, 	NULL);
		pthread_join(_events, 	NULL);

		// Was teardown clean or did it time out?
		if (tdClean)
			printf("Teardown accepted. Final sequence: %ld\n", conn.seqSend);
		else
			printf("Teardown finished due to timeout.\n");

		free(buffer.recv);
		free(buffer.send);
		free(buffer.acks);
		free(frame);
		printf("Connection terminated.\n");
	}
	return 0;
}
