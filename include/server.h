#ifndef SERVER_H
#define SERVER_H

#include "server_client_header.h"
#include "board.h"
#include "parser.h"
#include <dirent.h>
#include <signal.h>
#include "protocol.h"
#include <sys/types.h>
#include <pthread.h>
#include <semaphore.h>
#include "display.h"

#define PIPE_ERROR "Erro no Pipe"
#define THREAD_ERROR "Erro na Thread"
#define LEVEL_ERROR "Erro no nível"

struct pacman_thread_arg{
	board_t* board;
	int pipe_fd;
	int* vitoria;
	
};

typedef struct {
    board_t *board;
    int ghost_index;
	int* should_stop;
} ghost_thread_arg_t;

struct board_update{
	board_t* board;
	int notif_fd;
	int* should_stop;
};

struct client_information{
	int id;
	int active;
	int* pontuacao;
};


void* server_pacman_t(void* arg);

void* ghost_thread(void *arg);

void* send_board_updates(void* arg);

void receive_clients();

void* operador(void* arg);

void sigsur1_hand(int sig);

void top5(void);


#endif