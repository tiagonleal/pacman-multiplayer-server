#include "server.h"

char* global_server_pipe;
int global_max_games;
struct connect_req* fila_pedidos;
int write_p = 0;
int read_p = 0;
pthread_mutex_t thread_lock;
sem_t n_empty;
sem_t n_filled;
char* global_levels_dir;
struct client_information* clientes_ativos;
pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
volatile __sig_atomic_t sigsur1_rec = 0;

/**
 * Thread que gere os movimentos do Pacman de um cliente.
 * Lê comandos do pipe do cliente e move o Pacman no tabuleiro.
 * Termina quando o Pacman morre, atinge o portal ou o cliente desconecta.
 * 
 * @param arg Ponteiro para struct pacman_thread_arg com board, pipe_fd e flag de vitória
 * @return NULL
 */
void* server_pacman_t(void* arg){

	struct pacman_thread_arg* args = (struct pacman_thread_arg*) arg;
	board_t* board = args->board;
	int pipe_fd = args->pipe_fd;

	pacman_t* pacman = &board->pacmans[0];
	char command;
	int lidos;

	while(pacman->alive){

		char opcode;
		lidos = read(pipe_fd, &opcode, 1);

		if (lidos <= 0)
			break;
		
		if (opcode == OP_CODE_DISCONNECT) break;

		// Ignora opcodes que não sejam de jogar
		else if (opcode != OP_CODE_PLAY)
			continue;

		lidos = read(pipe_fd, &command, 1);

		if (lidos <= 0) break;

		command_t cmd;
		cmd.command = command;
		cmd.turns = 1;

		int res = move_pacman(board, 0, &cmd);

		debug("%c\nRes: %d\n", command, res);

		//Vitoria
		if (res == REACHED_PORTAL){
			*(args->vitoria) = 1;
			break;		
		}
	}
	return NULL;
}


/**
 * Thread que controla o movimento automático de um fantasma
 * O fantasma move-se seguindo os movimentos definidos no ficheiro
 * 
 * @param arg Ponteiro para ghost_thread_arg_t com board, índice do fantasma e flag de paragem
 * @return NULL
 */
void* ghost_thread(void *arg) {
    ghost_thread_arg_t *ghost_arg = (ghost_thread_arg_t*) arg;
    board_t *board = ghost_arg->board;
    int ghost_ind = ghost_arg->ghost_index;
	int *should_stop = ghost_arg->should_stop;

    free(ghost_arg);
    ghost_t* ghost = &board->ghosts[ghost_ind];

    while (! (*should_stop)) {
		// Tempo de espera
        sleep_ms(board->tempo * (1 + ghost->passo));
		if (*should_stop) break;
		// Move
        move_ghost(board, ghost_ind, &ghost->moves[ghost->current_move%ghost->n_moves]);
    }
	return NULL;
}


/**
 * Thread que envia periodicamente o estado do tabuleiro ao cliente
 * Envia o opcode, dimensões, tempo, flags de vitória/game over, pontos e dados do tabuleiro
 * 
 * @param arg Ponteiro para struct board_update com board, fd de notificação e flag de paragem
 * @return NULL
 */
void* send_board_updates(void* arg){
	struct board_update* args = (struct board_update*) arg;
	board_t* board = args->board;

	int notif_fd = args-> notif_fd;
	int* should_stop = args-> should_stop;

	while(!(*should_stop)){
		sleep_ms(board->tempo);

		char opcode = OP_CODE_BOARD;

		char* board_data = get_board_displayed(board);

		// Vitoria se deve parar mas pacman ainda está vivo
		int victory = *should_stop && board->pacmans[0].alive;
		int game_over = !board-> pacmans[0].alive;
		int accumulated_points = board->pacmans[0].points;

		// Envia todos os campos do protocolo ao cliente
		size_t written = 0;
		written += write(notif_fd, &opcode, sizeof(char));
		written += write(notif_fd,&board->width, sizeof(int));
		written += write(notif_fd,&board->height, sizeof(int));
		written += write(notif_fd,&board->tempo, sizeof(int));
		written += write(notif_fd,&victory, sizeof(int));
		written += write(notif_fd,&game_over, sizeof(int));
		written += write(notif_fd,&accumulated_points, sizeof(int));
		written += write(notif_fd,board_data, board-> width * board-> height);

		free(board_data);

		if (written <= 0) {
            debug("Erro a enviar update\n");
            break;
        }
	}
	return NULL;
}


/**
 * Função principal que recebe pedidos de conexão dos clientes
 * Lê do pipe do servidor e coloca os pedidos numa fila circular (produtor-consumidor)
 * Reabre o pipe quando todos os escritores fecham para evitar EOF permanente
 */
void receive_clients(){

	struct connect_req pedido;
	int server_fd = open(global_server_pipe, O_RDONLY);
	if (server_fd == -1){
		perror(PIPE_ERROR);
		exit(1);
	}

	while(1){

		// Verifica se foi recebido SIGUSR1 para imprimir top5
		if (sigsur1_rec) {
			sigsur1_rec = 0;
			top5();
		}

		size_t lidos = read(server_fd, &pedido, sizeof(pedido));	

		// Reabre pipe quando não há escritores
		if (lidos == 0){
			close(server_fd);
			server_fd = open(global_server_pipe, O_RDONLY);
			if (server_fd == -1){
				perror(PIPE_ERROR);
				exit(1);
			}
			continue;
		}

		// Ignora leituras parciais
		if (lidos != sizeof(pedido))
			continue;

		// Produtor: espera por slot vazio e adiciona pedido à fila circular
		sem_wait(&n_empty);

		pthread_mutex_lock(&thread_lock);
		fila_pedidos[write_p] = pedido;
		write_p = (write_p + 1) % global_max_games;
		pthread_mutex_unlock(&thread_lock);

		sem_post(&n_filled);
	}
	close(server_fd);
}


/**
 * Thread operador que processa os jogos dos cliente
 * Retira pedidos da fila, carrega níveis sequencialmente e gere as threads do jogo
 * Cada operador pode gerir um cliente de cada vez (modelo consumidor)
 * 
 * @param arg Não utilizado
 * @return NULL
 */
void* operador(void* arg __attribute__((unused))){
	struct connect_req pedido;
	struct connect_res resposta;
	int notif_fd;
	int request_fd;
	struct dirent* entrada;
	DIR* dir;
	board_t game;

	while (1){
		int* accumulated_points = malloc(sizeof(int));
		*accumulated_points = 0;

		// Consumidor: espera por pedido na fila circular
		sem_wait(&n_filled);

		pthread_mutex_lock(&thread_lock);
		pedido = fila_pedidos[read_p];
		read_p = (read_p + 1) % global_max_games;
		pthread_mutex_unlock(&thread_lock);

		sem_post(&n_empty);

		// Abre pipes de comunicação com o cliente
		request_fd = open(pedido.req_pipe_path, O_RDONLY);
		if (request_fd == -1){
			perror(PIPE_ERROR);
            continue;
		}

		notif_fd = open(pedido.notif_pipe_path, O_WRONLY);
		if (notif_fd == -1){
			perror(PIPE_ERROR);
			continue;
		}

		//Envia confirmação de conexão ao cliente
		resposta.OP_CODE = OP_CODE_CONNECT;
		resposta.result = 0;
		write(notif_fd, &resposta, sizeof(resposta));
		
		//Extrai ID do cliente do nome do pipe
		int client_id = atoi(pedido.req_pipe_path + strlen("/tmp/pac_req_"));
		int slot = -1;

		//Regista cliente na lista de clientes ativos
		pthread_mutex_lock(&client_lock);
		for (int i = 0; i < global_max_games; i++) {
			if (! clientes_ativos[i].active) {
				slot = i;
				clientes_ativos[i].id = client_id;
				clientes_ativos[i]. active = 1;
				clientes_ativos[i].pontuacao = accumulated_points;
				break;
			}
		}
		pthread_mutex_unlock(&client_lock);

		dir = opendir(global_levels_dir);
		if (!dir){
			perror(LEVEL_ERROR);
			close(request_fd);
			close (notif_fd);
			continue;
		}

		int level_won = 1;

		pthread_t t_pacman;
		pthread_t* t_ghost;
		pthread_t t_board_upd;

		//Itera por todos os niveis enquanto o jogador ganhar
		while (level_won == 1 && (entrada = readdir(dir)) != NULL){

			//Filtra apenas ficheiros .lvl
			char* ext = strrchr(entrada-> d_name, '.');
			if (!ext || strcmp(ext, ".lvl") != 0) continue;

			if (load_level(&game, entrada->d_name, global_levels_dir, *accumulated_points) == -1){
				perror(LEVEL_ERROR);
				continue;
			}

			level_won = 0;
			int exit_status = 0;
			int should_stop_updates = 0;

			//Cria thread do pacman
			struct pacman_thread_arg pacman_args;
			pacman_args.board = &game;
			pacman_args.pipe_fd = request_fd;
			pacman_args.vitoria = &exit_status;
			pthread_create(&t_pacman, NULL, server_pacman_t, &pacman_args);

			int should_stop_game = 0;

			//Cria uma thread para cada fantasma
			t_ghost = malloc(game.n_ghosts * sizeof(pthread_t));
			for (int i = 0; i < game.n_ghosts; i++){
				ghost_thread_arg_t* ghost_args = malloc(sizeof(ghost_thread_arg_t));
				ghost_args->board = &game;
				ghost_args->ghost_index = i;
				ghost_args->should_stop = &should_stop_game;
				pthread_create(&t_ghost[i], NULL, ghost_thread, ghost_args);
			}

			//Cria thread de atualização do tabuleiro
			struct board_update update_args;
			update_args.board = &game;
			update_args.notif_fd = notif_fd;
			update_args.should_stop = &should_stop_updates;
			pthread_create(&t_board_upd, NULL, send_board_updates, &update_args);

			//Espera que o pacman termine (morte, vitoria ou desconexao)
			pthread_join(t_pacman, NULL);

			// Sinaliza todas as threads para pararem
			should_stop_game = 1;
			should_stop_updates = 1;

			// Verifica se ganhou o nível
			if (exit_status == 1) { 
                level_won = 1; 
                *accumulated_points = game.pacmans[0].points;
            }

			//Espera que todas as threads dos fantasmas terminem
			for (int i = 0; i < game.n_ghosts; i++)
			pthread_join(t_ghost[i], NULL);

			//Cancela e espera pela thread de updates
			pthread_cancel(t_board_upd);
            pthread_join(t_board_upd, NULL);

			free(t_ghost);

			unload_level(&game);
		}	

		//Remove cliente da lista de ativos
		if (slot != -1) {
			pthread_mutex_lock(&client_lock);
			clientes_ativos[slot].active = 0;    
			free(clientes_ativos[slot].pontuacao);
			clientes_ativos[slot].pontuacao = NULL; 
			pthread_mutex_unlock(&client_lock);
		}

		closedir(dir);
		close(request_fd);
		close(notif_fd);
	}



	return NULL;
}


/**
 * Handler do sinal SIGUSR1.
 * Ativa a flag para imprimir o top 5 de forma assíncrona e segura.
 * 
 * @param sig
 */
void sigsur1_hand(int sig){
	(void) sig;
	sigsur1_rec = 1;
}


/**
 * Calcula e escreve no ficheiro top5.txt os 5 clientes com mais pontos
 * Ordena os clientes ativos por pontuação (decrescente) usando bubble sort
 * Thread-safe: usa mutex para aceder à lista de clientes
 */
void top5(){

	pthread_mutex_lock(&client_lock);

	//Array temporario para guardar IDs e pontos dos clientes ativos
	struct {
		int id;
		int points;
	} temp[global_max_games];

	int count = 0;
	int ativos = 0;
	for (int i = 0; i < global_max_games; i++){
		if (clientes_ativos[i].active)
			ativos++;

		if (clientes_ativos[i].active && clientes_ativos[i].pontuacao != NULL){
			temp[count].id = clientes_ativos[i].id;
			temp[count].points = *(clientes_ativos[i].pontuacao);
			count++;
		}
	}

	pthread_mutex_unlock(&client_lock);


	//Bubble sort
	for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (temp[j].points < temp[j + 1].points) {
                int tmp_id = temp[j].id;
                int tmp_pts = temp[j]. points;
                temp[j].id = temp[j + 1].id;
                temp[j]. points = temp[j + 1].points;
                temp[j + 1]. id = tmp_id;
                temp[j + 1]. points = tmp_pts;
            }
        }
    }

	//FIcheiro
	int fd = open("top5.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd != -1) {
        char buffer[256];
        int limit = (count < 5) ? count : 5;	//Número a imprimir, menos de 5 se não exisitrem 5

        for (int i = 0; i < limit; i++) {
            int len = snprintf(buffer, sizeof(buffer), "Cliente %d: %d pontos\n", temp[i].id, temp[i]. points);
            write(fd, buffer, len);
        }
        close(fd);
    }
	printf("Número de clientes ativos: %d\n", ativos);
}

/**
 * Função principal do servidor.
 * Inicializa estruturas, cria threads operadoras e começa a receber clientes.
 * 
 * @param argc Número de argumentos (deve ser 4)
 * @param argv [1] Diretório dos níveis, [2] Número máximo de jogos, [3] Caminho do pipe
 * @return 0 em caso de sucesso
 */
int main(int argc, char*argv[]){
	if (argc != 4)
		exit(1);

	signal(SIGPIPE, SIG_IGN); // ignorar SIGPIPE quando cliente desconecta

	// Bloqueia SIGUSR1 para as threads filhas não o receberem
	sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &mask, NULL);
	
	open_debug_file("server_log.txt");

	global_levels_dir = argv[1];
	global_max_games = atoi(argv[2]);
	global_server_pipe = argv[3];

	unlink (global_server_pipe); //Caso hajam restos

	if (mkfifo (global_server_pipe, 0666) == -1){
		perror(PIPE_ERROR);
		exit(1);
	}

	// Aloca fila circular de pedidos (produtor-consumidor)
	fila_pedidos = malloc(global_max_games * sizeof(struct connect_req));
	if (!fila_pedidos) {
		perror("Erro a alocar memória");
		exit(1);
	}
	clientes_ativos = calloc(global_max_games, sizeof(struct client_information));  // NOVO
	if (!clientes_ativos) {
		perror("Erro a alocar memória");
		exit(1);
	}

	pthread_mutex_init(&thread_lock, NULL);

	// Semáforos para controlo da fila: n_empty começa cheio, n_filled começa vazio
	sem_init(&n_empty, 0, global_max_games);
	sem_init(&n_filled, 0, 0);

	pthread_t operadores[global_max_games];
	for (int i = 0; i < global_max_games; i++)
		if (pthread_create(&operadores[i], NULL, operador, NULL) != 0){
			perror(THREAD_ERROR);
			exit(1);
		}
	
	struct sigaction sa;
	sa.sa_handler = sigsur1_hand;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, NULL);

	// Desbloqueia SIGUSR1 apenas na thread principal
	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);

	// Thread principal torna-se o produtor
	receive_clients();

	return 0;
}
