#ifndef SERVER_CLIENT_HEADER
#define SERVER_CLIENT_HEADER

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include "debug.h"

#define MAX_PIPE_PATH_LENGTH 40

struct connect_req{
	char OP_CODE;
	char req_pipe_path[MAX_PIPE_PATH_LENGTH];
	char notif_pipe_path[MAX_PIPE_PATH_LENGTH];
};

struct connect_res{
	char OP_CODE;
	char result;
};


#endif